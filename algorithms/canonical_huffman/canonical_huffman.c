#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../../include/compressor.h"
#include "canonical_huffman.h"

/* -- constants & data structs -- */
static const unsigned char CHF_MAGIC[4] = {'C', 'H', 'F', '1'};

typedef struct HNode
{
    uint64_t freq;
    int symbol; // 0..255 for leaves, -1 for internal nodes
    struct HNode *left, *right;
} HNode;

typedef struct
{
    HNode **data;
    size_t size;
    size_t capacity;
} MinHeap;

typedef struct
{
    uint64_t bits; /* stored LSB-first in low bits */
    uint8_t len;
} Code;

typedef struct
{
    int symbol;
    uint8_t code_len;
} SymbolCodeLen;

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

/* reverse lowest `len` bits of `v` */
static uint64_t reverse_bits(uint64_t v, uint8_t len)
{
    uint64_t r = 0;
    for (uint8_t i = 0; i < len; ++i)
    {
        r = (r << 1) | (v & 1);
        v >>= 1;
    }
    /* keep reversed bits in low `len` bits */
    return r;
}

/* -- min heap (for Huffman tree building) -- */
static MinHeap *heap_create(size_t capacity)
{
    MinHeap *h = malloc(sizeof(*h));
    if (!h)
        return NULL;
    h->data = malloc(sizeof(HNode *) * capacity);
    if (!h->data)
    {
        free(h);
        return NULL;
    }
    h->size = 0;
    h->capacity = capacity;
    return h;
}

static void heap_destroy(MinHeap *h)
{
    if (!h)
        return;
    free(h->data);
    free(h);
}

static int heap_push(MinHeap *h, HNode *n)
{
    if (h->size >= h->capacity)
    {
        size_t nc = h->capacity ? h->capacity * 2 : 4;
        HNode **tmp = realloc(h->data, sizeof(HNode *) * nc);
        if (!tmp)
            return -1;
        h->data = tmp;
        h->capacity = nc;
    }
    size_t i = h->size++;
    h->data[i] = n;
    while (i > 0)
    {
        size_t p = (i - 1) / 2;
        if (h->data[p]->freq <= h->data[i]->freq)
            break;
        HNode *t = h->data[p];
        h->data[p] = h->data[i];
        h->data[i] = t;
        i = p;
    }
    return 0;
}

static HNode *heap_pop(MinHeap *h)
{
    if (h->size == 0)
        return NULL;
    HNode *ret = h->data[0];
    h->data[0] = h->data[--h->size];
    size_t i = 0;
    while (1)
    {
        size_t l = 2 * i + 1, r = l + 1, smallest = i;
        if (l < h->size && h->data[l]->freq < h->data[smallest]->freq)
            smallest = l;
        if (r < h->size && h->data[r]->freq < h->data[smallest]->freq)
            smallest = r;
        if (smallest == i)
            break;
        HNode *t = h->data[i];
        h->data[i] = h->data[smallest];
        h->data[smallest] = t;
        i = smallest;
    }
    return ret;
}

/* -- Huffman tree node management -- */
static HNode *node_create(uint64_t freq, int symbol)
{
    HNode *n = malloc(sizeof(*n));
    if (!n)
        return NULL;
    n->freq = freq;
    n->symbol = symbol;
    n->left = n->right = NULL;
    return n;
}

static void free_tree(HNode *n)
{
    if (!n)
        return;
    free_tree(n->left);
    free_tree(n->right);
    free(n);
}

/* -- extract code lengths from Huffman tree -- */
static void extract_code_lengths(HNode *root, uint8_t lengths[256])
{
    struct StackItem
    {
        HNode *node;
        uint8_t depth;
    };

    struct StackItem *stack = malloc(sizeof(*stack) * 512);
    size_t sp = 0;

    if (!stack)
        return;

    memset(lengths, 0, 256);

    if (!root)
    {
        free(stack);
        return;
    }

    stack[sp++] = (struct StackItem){.node = root, .depth = 0};

    while (sp > 0)
    {
        struct StackItem it = stack[--sp];
        HNode *n = it.node;

        if (!n->left && !n->right) // leaf
        {
            if (n->symbol >= 0 && n->symbol < 256)
                lengths[(unsigned char)n->symbol] = it.depth;
        }
        else // internal node
        {
            if (n->right)
                stack[sp++] = (struct StackItem){.node = n->right, .depth = (uint8_t)(it.depth + 1)};
            if (n->left)
                stack[sp++] = (struct StackItem){.node = n->left, .depth = (uint8_t)(it.depth + 1)};
        }
    }

    free(stack);
}

/* -- canonical code generation from lengths (LSB-first stored) -- */
static void generate_canonical_codes(const uint8_t lengths[256], Code codes[256])
{
    /* count per length */
    int maxlen = 0;
    int bl_count[65] = {0}; /* assume code lengths won't exceed 64 */
    for (int i = 0; i < 256; ++i)
    {
        int l = lengths[i];
        if (l > 0)
        {
            if (l > 64)
                l = 64;
            bl_count[l]++;
            if (l > maxlen)
                maxlen = l;
        }
    }

    if (maxlen == 0)
    {
        memset(codes, 0, sizeof(Code) * 256);
        return;
    }

    /* compute next_code per RFC-like algorithm (MSB-aligned) */
    uint64_t next_code[65];
    next_code[0] = 0;
    uint64_t code = 0;
    for (int bits = 1; bits <= maxlen; ++bits)
    {
        code = (code + (uint64_t)bl_count[bits - 1]) << 1;
        next_code[bits] = code;
    }

    /* assign codes in symbol order (by length then symbol value) */
    memset(codes, 0, sizeof(Code) * 256);
    for (int sym = 0; sym < 256; ++sym)
    {
        int len = lengths[sym];
        if (len > 0)
        {
            uint64_t c = next_code[len]++;
            /* c is MSB-aligned integer; we must store LSB-first for our bit-writer
               so reverse lowest `len` bits and keep in low bits of codes[sym].bits */
            uint64_t rev = reverse_bits(c, (uint8_t)len);
            codes[sym].bits = rev;
            codes[sym].len = (uint8_t)len;
        }
    }
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

/* LSB-first writer */
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

/* LSB-first reader */
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
static int canonical_huffman_compress(
    const unsigned char *input,
    size_t input_size,
    unsigned char **output,
    size_t *output_size)
{
    if (output == NULL || output_size == NULL)
        return -1;

    *output = NULL;
    *output_size = 0;

    /* header will be: magic(4) + lengths(256) + total_symbols(8) + bitstream */
    /* handle empty input: write zero total_symbols */
    if (input_size == 0)
    {
        size_t hdr = 4 + 256 + 8;
        unsigned char *out = malloc(hdr);
        if (!out)
            return -1;
        memcpy(out, CHF_MAGIC, 4);
        memset(out + 4, 0, 256);
        write_u64_le(out + 4 + 256, 0);
        *output = out;
        *output_size = hdr;
        return 0;
    }

    /* frequency table */
    uint64_t freq[256];
    memset(freq, 0, sizeof(freq));
    for (size_t i = 0; i < input_size; ++i)
        freq[input[i]]++;

    /* build heap */
    MinHeap *heap = heap_create(256);
    if (!heap)
        return -1;
    for (int s = 0; s < 256; ++s)
    {
        if (freq[s] > 0)
        {
            HNode *n = node_create(freq[s], s);
            if (!n)
            {
                heap_destroy(heap);
                return -1;
            }
            if (heap_push(heap, n) != 0)
            {
                free_tree(n);
                heap_destroy(heap);
                return -1;
            }
        }
    }

    /* special handling: if there's only one distinct symbol, create a parent with one child */
    HNode *root = NULL;
    if (heap->size == 1)
    {
        HNode *only = heap_pop(heap);
        HNode *parent = node_create(only->freq, -1);
        if (!parent)
        {
            free_tree(only);
            heap_destroy(heap);
            return -1;
        }
        parent->left = only; /* single child; extract_code_lengths will produce length 1 */
        root = parent;
        heap_destroy(heap);
        heap = NULL;
    }
    else
    {
        while (heap->size > 1)
        {
            HNode *a = heap_pop(heap);
            HNode *b = heap_pop(heap);
            HNode *p = node_create(a->freq + b->freq, -1);
            if (!p)
            {
                free_tree(a);
                free_tree(b);
                heap_destroy(heap);
                return -1;
            }
            p->left = a;
            p->right = b;
            if (heap_push(heap, p) != 0)
            {
                free_tree(p);
                heap_destroy(heap);
                return -1;
            }
        }
        root = heap_pop(heap);
        heap_destroy(heap);
        heap = NULL;
    }

    /* get code lengths and canonical codes (stored LSB-first in low bits) */
    uint8_t lengths[256];
    extract_code_lengths(root, lengths);

    Code codes[256];
    memset(codes, 0, sizeof(codes));
    generate_canonical_codes(lengths, codes);

    /* bit writer */
    BitWriter bw;
    if (bw_init(&bw, input_size / 2 + 128) != 0)
    {
        free_tree(root);
        return -1;
    }

    for (size_t i = 0; i < input_size; ++i)
    {
        unsigned char sym = input[i];
        if (codes[sym].len == 0)
        {
            bw_free(&bw);
            free_tree(root);
            return -1;
        }
        if (bw_write_bits(&bw, codes[sym].bits, codes[sym].len) != 0)
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

    /* build output: magic + lengths(256) + total_symbols(8) + bitstream */
    size_t header_size = 4 + 256 + 8;
    size_t total = header_size + bw.pos;
    unsigned char *out = malloc(total);
    if (!out)
    {
        bw_free(&bw);
        free_tree(root);
        return -1;
    }

    memcpy(out, CHF_MAGIC, 4);
    memcpy(out + 4, lengths, 256);
    write_u64_le(out + 4 + 256, (uint64_t)input_size);
    memcpy(out + header_size, bw.buf, bw.pos);

    *output = out;
    *output_size = total;

    bw_free(&bw);
    free_tree(root);
    return 0;
}

/* -- decompression function -- */
static int canonical_huffman_decompress(
    const unsigned char *input,
    size_t input_size,
    unsigned char **output,
    size_t *output_size)
{
    if (output == NULL || output_size == NULL)
        return -1;

    *output = NULL;
    *output_size = 0;

    /* validate header */
    if (input_size < 4 + 256 + 8)
        return -1;
    if (memcmp(input, CHF_MAGIC, 4) != 0)
        return -1;

    const unsigned char *lengths_buf = input + 4;
    uint8_t lengths[256];
    memcpy(lengths, lengths_buf, 256);

    uint64_t total_symbols = read_u64_le(input + 4 + 256);
    const unsigned char *bitstream = input + 4 + 256 + 8;
    size_t bitstream_len = input_size - (4 + 256 + 8);

    if (total_symbols == 0)
    {
        /* empty original */
        *output = malloc(0);
        *output_size = 0;
        return 0;
    }

    /* generate canonical codes (LSB-first stored) */
    Code codes[256];
    memset(codes, 0, sizeof(codes));
    generate_canonical_codes(lengths, codes);

    /* build a small code-table for lookup (len + code -> symbol) */
    typedef struct
    {
        uint64_t code;
        int symbol;
        uint8_t len;
    } CodeEntry;
    CodeEntry *ct = malloc(sizeof(CodeEntry) * 256);
    if (!ct)
        return -1;
    int ct_sz = 0;
    for (int s = 0; s < 256; ++s)
    {
        if (codes[s].len > 0)
        {
            ct[ct_sz].code = codes[s].bits;
            ct[ct_sz].len = codes[s].len;
            ct[ct_sz].symbol = s;
            ct_sz++;
        }
    }

    /* allocate output buffer */
    if (total_symbols > SIZE_MAX)
    {
        free(ct);
        return -1;
    }
    unsigned char *out = malloc((size_t)total_symbols);
    if (!out)
    {
        free(ct);
        return -1;
    }

    /* decode bitstream using LSB-first bits */
    BitReader br;
    br_init(&br, bitstream, bitstream_len);
    size_t out_pos = 0;

    while (out_pos < total_symbols)
    {
        uint64_t code_val = 0;
        int found = 0;

        /* read bits until we match a code */
        for (uint8_t len = 1; len <= 64; ++len)
        {
            int bit;
            if (br_read_bit(&br, &bit) != 0)
            {
                free(out);
                free(ct);
                return -1;
            }
            /* place bit at position len-1 (LSB-first assembly) */
            code_val |= ((uint64_t)bit << (len - 1));

            /* search for matching entry with same len */
            for (int i = 0; i < ct_sz; ++i)
            {
                if (ct[i].len == len && ct[i].code == code_val)
                {
                    out[out_pos++] = (unsigned char)ct[i].symbol;
                    found = 1;
                    break;
                }
            }
            if (found)
                break;
        }

        if (!found)
        {
            free(out);
            free(ct);
            return -1;
        }
    }

    *output = out;
    *output_size = out_pos;
    free(ct);
    return 0;
}

/* expose compressor */
Compressor canonical_huffman_compressor = {
    .name = "canonical_huffman",
    .compress = canonical_huffman_compress,
    .decompress = canonical_huffman_decompress};