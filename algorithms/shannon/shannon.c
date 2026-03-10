#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../../include/compressor.h"
#include "shannon.h"

/* -- constants & data structs -- */

static const unsigned char SHN_MAGIC[4] = {'S', 'H', 'N', '1'};

typedef struct SNode
{
    uint64_t freq;
    int symbol; // 0..255 for leaves, -1 for internal nodes
    struct SNode *left, *right;
} SNode;

typedef struct
{
    uint64_t bits;
    uint8_t len;
} Code;

typedef struct
{
    int symbol;
    uint64_t freq;
} SymbolFreq;

/* -- utility functions -- */

static void write_u64_le(unsigned char *buf, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        buf[i] = (unsigned char)((v >> (8 * i)) & 0xFF);
}

static uint64_t read_u64_le(const unsigned char *buf)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= ((uint64_t)buf[i]) << (8 * i);
    return v;
}

/* -- comparison for qsort -- */

static int compare_freq_desc(const void *a, const void *b)
{
    const SymbolFreq *sa = (const SymbolFreq *)a;
    const SymbolFreq *sb = (const SymbolFreq *)b;
    if (sa->freq > sb->freq)
        return -1;
    if (sa->freq < sb->freq)
        return 1;
    return sa->symbol - sb->symbol;
}

/* -- Shannon-Fano tree construction -- */

static SNode *node_create(uint64_t freq, int symbol)
{
    SNode *n = malloc(sizeof(*n));
    if (!n)
        return NULL;
    n->freq = freq;
    n->symbol = symbol;
    n->left = n->right = NULL;
    return n;
}

static void free_tree(SNode *n)
{
    if (!n)
        return;
    free_tree(n->left);
    free_tree(n->right);
    free(n);
}

/**
 * build Shannon-Fano tree recursively by partitioning symbols
 * based on cumulative frequency to balance the tree.
 */
static SNode *build_shannon_tree(SymbolFreq *symbols, int count)
{
    if (count == 0)
        return NULL;
    if (count == 1)
        return node_create(symbols[0].freq, symbols[0].symbol);

    // find split point that balances cumulative frequencies
    uint64_t total = 0;
    for (int i = 0; i < count; i++)
        total += symbols[i].freq;

    uint64_t target = total / 2;
    uint64_t cumsum = 0;
    int split = 0;
    for (int i = 0; i < count; i++)
    {
        cumsum += symbols[i].freq;
        if (cumsum >= target)
        {
            split = i + 1;
            break;
        }
    }

    if (split == 0)
        split = 1;
    if (split == count)
        split = count - 1;

    // create internal node and recursively build children
    SNode *node = node_create(total, -1);
    if (!node)
        return NULL;

    node->left = build_shannon_tree(symbols, split);
    node->right = build_shannon_tree(symbols + split, count - split);

    if (!node->left || !node->right)
    {
        free_tree(node);
        return NULL;
    }

    return node;
}

/* -- code generation -- */

static void build_codes_from_tree(SNode *root, Code codes[256])
{
    struct StackItem
    {
        SNode *node;
        uint64_t bits;
        uint8_t len;
    };

    struct StackItem *stack = malloc(sizeof(*stack) * 512);
    size_t sp = 0;

    if (!stack)
        return;

    for (int i = 0; i < 256; ++i)
    {
        codes[i].bits = 0;
        codes[i].len = 0;
    }

    if (!root)
    {
        free(stack);
        return;
    }

    stack[sp++] = (struct StackItem){.node = root, .bits = 0, .len = 0};

    while (sp > 0)
    {
        struct StackItem it = stack[--sp];
        SNode *n = it.node;

        if (!n->left && !n->right) // leaf node
        {
            if (n->symbol >= 0 && n->symbol < 256)
            {
                codes[n->symbol].bits = it.bits;
                codes[n->symbol].len = it.len;
            }
        }
        else // internal node
        {
            if (n->right)
            {
                stack[sp++] = (struct StackItem){
                    .node = n->right,
                    .bits = it.bits | ((uint64_t)1 << it.len),
                    .len = it.len + 1};
            }
            if (n->left)
            {
                stack[sp++] = (struct StackItem){
                    .node = n->left,
                    .bits = it.bits,
                    .len = it.len + 1};
            }
        }
    }

    free(stack);
}

/* -- bit writer -- */

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

static void bw_free(BitWriter *bw)
{
    free(bw->buf);
}

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

static int bw_write_bits(BitWriter *bw, uint64_t bits, uint8_t len)
{
    for (uint8_t i = 0; i < len; ++i)
    {
        uint8_t bit = (bits >> i) & 1;
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

/* -- bit reader -- */

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
    *out_bit = br->bitbuf & 1;
    br->bitbuf >>= 1;
    br->bitcount--;
    return 0;
}

/* -- compression function -- */

static int shannon_compress(
    const unsigned char *input,
    size_t input_size,
    unsigned char **output,
    size_t *output_size)
{
    if (output == NULL || output_size == NULL)
        return -1;

    *output = NULL;
    *output_size = 0;

    // handle empty input
    if (input_size == 0)
    {
        *output = malloc(4 + 256 * 8);
        if (!*output)
            return -1;
        memcpy(*output, SHN_MAGIC, 4);
        memset(*output + 4, 0, 256 * 8);
        *output_size = 4 + 256 * 8;
        return 0;
    }

    // count frequencies
    uint64_t freq[256];
    memset(freq, 0, sizeof(freq));
    for (size_t i = 0; i < input_size; ++i)
        freq[input[i]]++;

    // collect symbols with non-zero frequencies
    SymbolFreq *symbols = malloc(sizeof(*symbols) * 256);
    if (!symbols)
        return -1;

    int symbol_count = 0;
    for (int s = 0; s < 256; ++s)
    {
        if (freq[s] > 0)
        {
            symbols[symbol_count].symbol = s;
            symbols[symbol_count].freq = freq[s];
            symbol_count++;
        }
    }

    // sort by frequency (descending)
    qsort(symbols, symbol_count, sizeof(*symbols), compare_freq_desc);

    // build Shannon-Fano tree
    SNode *root = build_shannon_tree(symbols, symbol_count);
    free(symbols);

    if (!root && symbol_count > 0)
        return -1;

    // generate codes from tree
    Code codes[256];
    memset(codes, 0, sizeof(codes));
    if (symbol_count > 0)
        build_codes_from_tree(root, codes);

    // initialize bit writer
    BitWriter bw;
    if (bw_init(&bw, input_size / 2 + 1024) != 0)
    {
        free_tree(root);
        return -1;
    }

    // encode symbols
    for (size_t i = 0; i < input_size; ++i)
    {
        unsigned char byte = input[i];
        if (bw_write_bits(&bw, codes[byte].bits, codes[byte].len) != 0)
        {
            bw_free(&bw);
            free_tree(root);
            return -1;
        }
    }

    if (bw_flush(&bw) != 0)
    {
        bw_free(&bw);
        free_tree(root);
        return -1;
    }

    // build output: magic + frequencies + bitstream
    size_t header_size = 4 + 256 * 8;
    size_t total = header_size + bw.pos;
    unsigned char *out = malloc(total);
    if (!out)
    {
        bw_free(&bw);
        free_tree(root);
        return -1;
    }

    memcpy(out, SHN_MAGIC, 4);
    for (int i = 0; i < 256; ++i)
        write_u64_le(out + 4 + i * 8, freq[i]);
    memcpy(out + header_size, bw.buf, bw.pos);

    *output = out;
    *output_size = total;

    bw_free(&bw);
    free_tree(root);

    return 0;
}

/* -- decompression function -- */

static int shannon_decompress(
    const unsigned char *input,
    size_t input_size,
    unsigned char **output,
    size_t *output_size)
{
    if (output == NULL || output_size == NULL)
        return -1;

    *output = NULL;
    *output_size = 0;

    // validate header
    if (input_size < 4 + 256 * 8)
        return -1;
    if (memcmp(input, SHN_MAGIC, 4) != 0)
        return -1;

    // read frequencies
    const unsigned char *p = input + 4;
    uint64_t freq[256];
    uint64_t total_symbols = 0;
    for (int i = 0; i < 256; ++i)
    {
        freq[i] = read_u64_le(p);
        p += 8;
        total_symbols += freq[i];
    }

    size_t header_size = 4 + 256 * 8;
    const unsigned char *bitstream = input + header_size;
    size_t bitstream_len = input_size - header_size;

    // handle empty input
    if (total_symbols == 0)
    {
        *output = malloc(1);
        if (!*output)
            return -1;
        *output_size = 0;
        return 0;
    }

    // collect symbols with non-zero frequencies
    SymbolFreq *symbols = malloc(sizeof(*symbols) * 256);
    if (!symbols)
        return -1;

    int symbol_count = 0;
    for (int s = 0; s < 256; ++s)
    {
        if (freq[s] > 0)
        {
            symbols[symbol_count].symbol = s;
            symbols[symbol_count].freq = freq[s];
            symbol_count++;
        }
    }

    // sort by frequency (descending)
    qsort(symbols, symbol_count, sizeof(*symbols), compare_freq_desc);

    // build Shannon-Fano tree
    SNode *root = build_shannon_tree(symbols, symbol_count);
    free(symbols);

    if (!root && symbol_count > 0)
        return -1;

    // allocate output buffer
    if (total_symbols > SIZE_MAX)
    {
        free_tree(root);
        return -1;
    }

    unsigned char *out = malloc((size_t)total_symbols);
    if (!out)
    {
        free_tree(root);
        return -1;
    }

    // decode bitstream
    BitReader br;
    br_init(&br, bitstream, bitstream_len);
    size_t out_pos = 0;

    // walk tree according to bits
    while (out_pos < total_symbols)
    {
        SNode *node = root;
        if (!node)
        {
            free(out);
            return -1;
        }

        while (node->left || node->right)
        {
            int bit;
            if (br_read_bit(&br, &bit) != 0)
            {
                free(out);
                free_tree(root);
                return -1;
            }
            node = bit ? node->right : node->left;
            if (!node)
            {
                free(out);
                free_tree(root);
                return -1;
            }
        }

        out[out_pos++] = (unsigned char)node->symbol;
    }

    *output = out;
    *output_size = out_pos;
    free_tree(root);
    return 0;
}

Compressor shannon_compressor = {
    .name = "shannon",
    .compress = shannon_compress,
    .decompress = shannon_decompress};