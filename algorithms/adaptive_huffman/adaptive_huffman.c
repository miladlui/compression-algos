#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../../include/compressor.h"
#include "adaptive_huffman.h"

/* magic + format:
 * output = MAGIC(4) + original_size_u32 (4) + bitstream bytes
 */

static const unsigned char AHF_MAGIC[4] = {'A', 'H', 'F', '1'};

#define NYT_SYMBOL -2

typedef struct ANode
{
    uint32_t weight;
    int symbol; /* 0..255 for leaves, -1 for internal, -2 for NYT */
    struct ANode *parent;
    struct ANode *left, *right;
    uint32_t node_id;
} ANode;

typedef struct
{
    uint64_t bits;
    uint8_t len;
} Code;

/* --- utilities --- */
static void write_u32_le(unsigned char *buf, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        buf[i] = (unsigned char)((v >> (8 * i)) & 0xFF);
}
static uint32_t read_u32_le(const unsigned char *buf)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
        v |= ((uint32_t)buf[i]) << (8 * i);
    return v;
}

/* --- bit writer / reader (LSB-first) --- */
typedef struct
{
    unsigned char *buf;
    size_t cap;
    size_t pos;
    uint8_t bitbuf;
    uint8_t bitcount;
} BitWriter;

static int bw_init(BitWriter *bw, size_t expected_capacity)
{
    bw->cap = expected_capacity > 16 ? expected_capacity : 16;
    bw->buf = malloc(bw->cap);
    if (!bw->buf)
        return -1;
    bw->pos = 0;
    bw->bitbuf = 0;
    bw->bitcount = 0;
    return 0;
}
static void bw_free(BitWriter *bw) { free(bw->buf); }
static int bw_ensure(BitWriter *bw, size_t extra)
{
    if (bw->pos + extra + 1 > bw->cap)
    {
        size_t nc = (bw->cap * 2) + extra;
        unsigned char *tmp = realloc(bw->buf, nc);
        if (!tmp)
            return -1;
        bw->buf = tmp;
        bw->cap = nc;
    }
    return 0;
}
/* write LSB-first low `len` bits of `bits` */
static int bw_write_bits(BitWriter *bw, uint64_t bits, uint8_t len)
{
    for (uint8_t i = 0; i < len; ++i)
    {
        uint8_t bit = (bits >> i) & 1u;
        bw->bitbuf |= (bit << bw->bitcount);
        bw->bitcount++;
        if (bw->bitcount == 8)
        {
            if (bw_ensure(bw, 1) != 0)
                return -1;
            bw->buf[bw->pos++] = bw->bitbuf;
            bw->bitbuf = 0;
            bw->bitcount = 0;
        }
    }
    return 0;
}
static int bw_flush(BitWriter *bw)
{
    if (bw->bitcount > 0)
    {
        if (bw_ensure(bw, 1) != 0)
            return -1;
        bw->buf[bw->pos++] = bw->bitbuf;
        bw->bitbuf = 0;
        bw->bitcount = 0;
    }
    return 0;
}

typedef struct
{
    const unsigned char *buf;
    size_t len;
    size_t pos;
    uint8_t bitbuf;
    uint8_t bitcount;
} BitReader;
static void br_init(BitReader *br, const unsigned char *buf, size_t len)
{
    br->buf = buf;
    br->len = len;
    br->pos = 0;
    br->bitbuf = 0;
    br->bitcount = 0;
}
static int br_read_bit(BitReader *br, int *out_bit)
{
    if (br->bitcount == 0)
    {
        if (br->pos >= br->len)
            return -1;
        br->bitbuf = br->buf[br->pos++];
        br->bitcount = 8;
    }
    *out_bit = br->bitbuf & 1u;
    br->bitbuf >>= 1;
    br->bitcount--;
    return 0;
}

/* --- adaptive tree --- */
/* nodes: created for internal, NYT and symbol leaves.
 * We free entire subtree at teardown.
 */
typedef struct
{
    ANode *nodes[257]; /* 0..255 symbols, 256 = NYT */
    ANode *root;
    uint32_t next_node_id;
    int symbol_seen[256];
} AdaptiveTree;

static ANode *anode_create(int symbol, uint32_t weight, uint32_t node_id)
{
    ANode *n = malloc(sizeof(*n));
    if (!n)
        return NULL;
    n->weight = weight;
    n->symbol = symbol;
    n->parent = n->left = n->right = NULL;
    n->node_id = node_id;
    return n;
}

static void free_subtree(ANode *n)
{
    if (!n)
        return;
    free_subtree(n->left);
    free_subtree(n->right);
    free(n);
}
static void free_adaptive_tree(AdaptiveTree *tree)
{
    if (!tree)
        return;
    if (tree->root)
        free_subtree(tree->root);
    free(tree);
}

static AdaptiveTree *adaptive_tree_create(void)
{
    AdaptiveTree *t = malloc(sizeof(*t));
    if (!t)
        return NULL;
    memset(t, 0, sizeof(*t));
    t->next_node_id = 0;
    /* initial NYT as root */
    ANode *nyt = anode_create(NYT_SYMBOL, 0, t->next_node_id++);
    if (!nyt)
    {
        free(t);
        return NULL;
    }
    t->nodes[256] = nyt;
    t->root = nyt;
    return t;
}

/* Build code path from root->leaf, but store LSB-first so bw_write_bits emits root->leaf */
static int get_code_path(ANode *node, Code *code)
{
    if (!node)
        return -1;
    /* collect directions while walking up */
    unsigned char dirs[128];
    int depth = 0;
    ANode *cur = node;
    while (cur->parent)
    {
        dirs[depth++] = (cur->parent->right == cur) ? 1u : 0u;
        cur = cur->parent;
        if (depth >= (int)sizeof(dirs))
            return -1;
    }
    /* now dirs[0] is last step (leaf's parent), dirs[depth-1] is step from root */
    uint64_t bits = 0;
    for (int i = 0; i < depth; ++i)
    {
        /* we must emit bits in root->leaf order, but writer writes LSB-first
           so place the root-most bit at position 0, next at 1, ... */
        /* root-most bit is dirs[depth-1], becomes bit 0 */
        uint8_t b = dirs[depth - 1 - i];
        bits |= ((uint64_t)b << i);
    }
    code->bits = bits;
    code->len = (uint8_t)depth;
    return 0;
}

/* increment weight for node and all ancestors */
static void update_tree(AdaptiveTree *tree, ANode *node)
{
    ANode *cur = node;
    while (cur)
    {
        cur->weight++;
        cur = cur->parent;
    }
}

/* add new symbol: replace NYT with internal node (left=new NYT, right=new symbol leaf).
 * new symbol leaf weight must be 1 (we just transmitted it).
 * After insertion, propagate +1 to ancestors of the new internal node (not double-increment).
 */
static int add_symbol_to_tree(AdaptiveTree *tree, int symbol)
{
    if (!tree)
        return -1;
    if (tree->symbol_seen[symbol])
        return 0;

    ANode *old_nyt = tree->nodes[256];
    if (!old_nyt)
        return -1;

    ANode *new_nyt = anode_create(NYT_SYMBOL, 0, tree->next_node_id++);
    ANode *new_sym = anode_create(symbol, 1, tree->next_node_id++); /* new symbol weight = 1 */
    if (!new_nyt || !new_sym)
    {
        free(new_nyt);
        free(new_sym);
        return -1;
    }

    ANode *internal = anode_create(-1, 1, tree->next_node_id++); /* internal weight = new_nyt(0) + new_sym(1) =1 */
    if (!internal)
    {
        free(new_nyt);
        free(new_sym);
        return -1;
    }

    internal->left = new_nyt;
    internal->right = new_sym;
    new_nyt->parent = internal;
    new_sym->parent = internal;

    if (old_nyt->parent)
    {
        ANode *p = old_nyt->parent;
        if (p->left == old_nyt)
            p->left = internal;
        else
            p->right = internal;
        internal->parent = p;
    }
    else
    {
        /* old_nyt was root, internal becomes root */
        internal->parent = NULL;
        tree->root = internal;
    }

    /* replace NYT pointer and symbol leaf pointer */
    tree->nodes[256] = new_nyt;
    tree->nodes[symbol] = new_sym;
    tree->symbol_seen[symbol] = 1;

    /* propagate +1 to ancestors of internal (internal already accounts for new symbol) */
    if (internal->parent)
        update_tree(tree, internal->parent);

    return 0;
}

/* --- compress / decompress --- */

static int adaptive_huffman_compress(
    const unsigned char *input, size_t input_size,
    unsigned char **output, size_t *output_size)
{
    if (!output || !output_size)
        return -1;
    *output = NULL;
    *output_size = 0;

    BitWriter bw;
    if (bw_init(&bw, input_size + 16) != 0)
        return -1;

    /* write header space: we'll write size at start of bw.buf directly */
    if (bw_ensure(&bw, 4) != 0)
    {
        bw_free(&bw);
        return -1;
    }
    write_u32_le(bw.buf, (uint32_t)input_size);
    bw.pos = 4;

    AdaptiveTree *tree = adaptive_tree_create();
    if (!tree)
    {
        bw_free(&bw);
        return -1;
    }

    for (size_t i = 0; i < input_size; ++i)
    {
        unsigned char sym = input[i];

        if (!tree->symbol_seen[sym])
        {
            /* send NYT code then raw 8-bit symbol */
            Code c;
            if (get_code_path(tree->nodes[256], &c) != 0)
            {
                free_adaptive_tree(tree);
                bw_free(&bw);
                return -1;
            }
            if (bw_write_bits(&bw, c.bits, c.len) != 0)
            {
                free_adaptive_tree(tree);
                bw_free(&bw);
                return -1;
            }
            if (bw_write_bits(&bw, sym, 8) != 0)
            {
                free_adaptive_tree(tree);
                bw_free(&bw);
                return -1;
            }

            /* add symbol: new symbol weight set to 1; propagate to ancestors */
            if (add_symbol_to_tree(tree, sym) != 0)
            {
                free_adaptive_tree(tree);
                bw_free(&bw);
                return -1;
            }
        }
        else
        {
            Code c;
            if (get_code_path(tree->nodes[sym], &c) != 0)
            {
                free_adaptive_tree(tree);
                bw_free(&bw);
                return -1;
            }
            if (bw_write_bits(&bw, c.bits, c.len) != 0)
            {
                free_adaptive_tree(tree);
                bw_free(&bw);
                return -1;
            }
            /* increment leaf and ancestors */
            update_tree(tree, tree->nodes[sym]);
        }
    }

    if (bw_flush(&bw) != 0)
    {
        free_adaptive_tree(tree);
        bw_free(&bw);
        return -1;
    }

    /* final output: MAGIC + bw.buf (which already begins with original_size) */
    size_t total = 4 + bw.pos;
    unsigned char *out = malloc(total);
    if (!out)
    {
        free_adaptive_tree(tree);
        bw_free(&bw);
        return -1;
    }
    memcpy(out, AHF_MAGIC, 4);
    memcpy(out + 4, bw.buf, bw.pos);

    *output = out;
    *output_size = total;

    free_adaptive_tree(tree);
    bw_free(&bw);
    return 0;
}

static int adaptive_huffman_decompress(
    const unsigned char *input, size_t input_size,
    unsigned char **output, size_t *output_size)
{
    if (!output || !output_size)
        return -1;
    *output = NULL;
    *output_size = 0;

    if (input_size < 4 + 4)
        return -1;
    if (memcmp(input, AHF_MAGIC, 4) != 0)
        return -1;

    uint32_t orig_size = read_u32_le(input + 4);
    const unsigned char *bitstream = input + 8;
    size_t bitstream_len = (input_size >= 8) ? (input_size - 8) : 0;

    BitReader br;
    br_init(&br, bitstream, bitstream_len);

    AdaptiveTree *tree = adaptive_tree_create();
    if (!tree)
        return -1;

    if (orig_size > SIZE_MAX)
    {
        free_adaptive_tree(tree);
        return -1;
    }
    unsigned char *out = malloc((size_t)orig_size);
    if (!out)
    {
        free_adaptive_tree(tree);
        return -1;
    }

    for (uint32_t i = 0; i < orig_size; ++i)
    {
        ANode *cur = tree->root;
        /* walk until leaf */
        while (cur->left || cur->right)
        {
            int bit;
            if (br_read_bit(&br, &bit) != 0)
            {
                free(out);
                free_adaptive_tree(tree);
                return -1;
            }
            cur = bit ? cur->right : cur->left;
            if (!cur)
            {
                free(out);
                free_adaptive_tree(tree);
                return -1;
            }
        }

        if (cur->symbol == NYT_SYMBOL)
        {
            /* read raw 8 bits */
            uint32_t sym = 0;
            for (int b = 0; b < 8; ++b)
            {
                int bit;
                if (br_read_bit(&br, &bit) != 0)
                {
                    free(out);
                    free_adaptive_tree(tree);
                    return -1;
                }
                sym |= (bit << b);
            }
            out[i] = (unsigned char)sym;
            if (add_symbol_to_tree(tree, (int)sym) != 0)
            {
                free(out);
                free_adaptive_tree(tree);
                return -1;
            }
        }
        else
        {
            out[i] = (unsigned char)cur->symbol;
            update_tree(tree, cur);
        }
    }

    *output = out;
    *output_size = (size_t)orig_size;
    free_adaptive_tree(tree);
    return 0;
}

Compressor adaptive_huffman_compressor = {
    .name = "adaptive_huffman",
    .compress = adaptive_huffman_compress,
    .decompress = adaptive_huffman_decompress};