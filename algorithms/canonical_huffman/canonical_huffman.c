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
    uint64_t bits;
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
    free(h->data);
    free(h);
}

static int heap_push(MinHeap *h, HNode *n)
{
    if (h->size >= h->capacity)
    {
        size_t nc = h->capacity * 2;
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
                lengths[n->symbol] = it.depth;
        }
        else // internal node
        {
            if (n->right)
                stack[sp++] = (struct StackItem){.node = n->right, .depth = it.depth + 1};
            if (n->left)
                stack[sp++] = (struct StackItem){.node = n->left, .depth = it.depth + 1};
        }
    }

    free(stack);
}

/* -- Canonical code generation from lengths -- */

/**
 * Generate canonical codes from code lengths.
 * canonical Huffman codes are assigned by:
 *   1. Sort symbols by code length, then by symbol value
 *   2. Start with code = 0
 *   3. For each symbol: assign current code, increment
 *   4. When code length increases, left-shift code
 */
static void generate_canonical_codes(const uint8_t lengths[256], Code codes[256])
{
    // collect non-zero length symbols
    SymbolCodeLen *sorted = malloc(sizeof(*sorted) * 256);
    if (!sorted)
        return;

    int count = 0;
    for (int i = 0; i < 256; i++)
    {
        if (lengths[i] > 0)
        {
            sorted[count].symbol = i;
            sorted[count].code_len = lengths[i];
            count++;
        }
    }

    // sort by code length, then by symbol
    for (int i = 0; i < count - 1; i++)
    {
        for (int j = i + 1; j < count; j++)
        {
            int should_swap = 0;
            if (sorted[i].code_len > sorted[j].code_len)
                should_swap = 1;
            else if (sorted[i].code_len == sorted[j].code_len &&
                     sorted[i].symbol > sorted[j].symbol)
                should_swap = 1;

            if (should_swap)
            {
                SymbolCodeLen tmp = sorted[i];
                sorted[i] = sorted[j];
                sorted[j] = tmp;
            }
        }
    }

    // initialize all codes to zero
    memset(codes, 0, sizeof(Code) * 256);

    // generate canonical codes
    uint64_t code = 0;
    uint8_t prev_len = 0;

    for (int i = 0; i < count; i++)
    {
        uint8_t len = sorted[i].code_len;
        int symbol = sorted[i].symbol;

        // when length increases, left-shift the code
        if (len > prev_len)
        {
            code <<= (len - prev_len);
            prev_len = len;
        }

        codes[symbol].bits = code;
        codes[symbol].len = len;
        code++;
    }

    free(sorted);
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

    // handle empty input
    if (input_size == 0)
    {
        *output = malloc(4 + 256);
        if (!*output)
            return -1;
        memcpy(*output, CHF_MAGIC, 4);
        memset(*output + 4, 0, 256);
        *output_size = 4 + 256;
        return 0;
    }

    // count frequencies
    uint64_t freq[256];
    memset(freq, 0, sizeof(freq));
    for (size_t i = 0; i < input_size; ++i)
        freq[input[i]]++;

    // build min-heap from frequencies
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
                free(n);
                heap_destroy(heap);
                return -1;
            }
        }
    }

    // special case: only one distinct symbol
    HNode *only;
    if (heap->size == 1)
    {
        only = heap_pop(heap);
        HNode *parent = node_create(only->freq, -1);
        if (!parent)
        {
            free(only);
            heap_destroy(heap);
            return -1;
        }
        parent->left = only;
        parent->right = node_create(only->freq, -1);
        if (!parent->right)
        {
            free_tree(parent);
            heap_destroy(heap);
            return -1;
        }
        heap_destroy(heap);
        heap = NULL;
        only = parent;
    }
    else
    {
        // build huffman tree
        while (heap->size > 1)
        {
            HNode *left = heap_pop(heap);
            HNode *right = heap_pop(heap);
            HNode *parent = node_create(left->freq + right->freq, -1);
            if (!parent)
            {
                free_tree(left);
                free_tree(right);
                heap_destroy(heap);
                return -1;
            }
            parent->left = left;
            parent->right = right;
            if (heap_push(heap, parent) != 0)
            {
                free_tree(parent);
                heap_destroy(heap);
                return -1;
            }
        }
        only = heap_pop(heap);
    }

    HNode *root = only;
    heap_destroy(heap);

    // extract code lengths from tree
    uint8_t lengths[256];
    extract_code_lengths(root, lengths);

    // generate canonical codes from lengths
    Code codes[256];
    memset(codes, 0, sizeof(codes));
    generate_canonical_codes(lengths, codes);

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

    // build header: magic + code lengths (compact representation)
    // store only code lengths (1 byte per symbol) instead of full tree/frequencies
    size_t header_size = 4 + 256;
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

    // validate header
    if (input_size < 4 + 256)
        return -1;
    if (memcmp(input, CHF_MAGIC, 4) != 0)
        return -1;

    // read code lengths
    const unsigned char *lengths_buf = input + 4;
    uint8_t lengths[256];
    memcpy(lengths, lengths_buf, 256);

    // Calculate total symbols from frequencies derived from lengths
    // for decompression, we need frequencies, but we only have lengths
    // to get frequencies, we need to read them separately
    // Actually, for canonical Huffman, we can't directly decode without knowing
    // the total number of symbols. We need to store this separately.
    // Modify the format: magic (4) + lengths (256) + total_symbols (8) + bitstream

    if (input_size < 4 + 256 + 8)
        return -1;

    uint64_t total_symbols = read_u64_le(input + 4 + 256);
    const unsigned char *bitstream = input + 4 + 256 + 8;
    size_t bitstream_len = input_size - (4 + 256 + 8);

    // handle empty input
    if (total_symbols == 0)
    {
        *output = malloc(1);
        if (!*output)
            return -1;
        *output_size = 0;
        return 0;
    }

    // generate canonical codes from lengths
    Code codes[256];
    memset(codes, 0, sizeof(codes));
    generate_canonical_codes(lengths, codes);

    // build reverse lookup table for fast decoding
    // map from canonical code bits >> symbol
    typedef struct
    {
        uint64_t code;
        int symbol;
        uint8_t len;
    } CodeEntry;

    CodeEntry *code_table = malloc(sizeof(*code_table) * 256);
    if (!code_table)
        return -1;

    int table_size = 0;
    for (int i = 0; i < 256; i++)
    {
        if (codes[i].len > 0)
        {
            code_table[table_size].code = codes[i].bits;
            code_table[table_size].len = codes[i].len;
            code_table[table_size].symbol = i;
            table_size++;
        }
    }

    // allocate output
    if (total_symbols > SIZE_MAX)
    {
        free(code_table);
        return -1;
    }

    unsigned char *out = malloc((size_t)total_symbols);
    if (!out)
    {
        free(code_table);
        return -1;
    }

    // decode bitstream
    BitReader br;
    br_init(&br, bitstream, bitstream_len);
    size_t out_pos = 0;

    while (out_pos < total_symbols)
    {
        uint64_t code_val = 0;
        int found = 0;

        // try lengths from 1 to 64
        for (uint8_t len = 1; len <= 64; len++)
        {
            int bit;
            if (br_read_bit(&br, &bit) != 0)
            {
                free(out);
                free(code_table);
                return -1;
            }
            code_val |= ((uint64_t)bit << (len - 1));

            // check if this matches any code
            for (int i = 0; i < table_size; i++)
            {
                if (code_table[i].len == len && code_table[i].code == code_val)
                {
                    out[out_pos++] = (unsigned char)code_table[i].symbol;
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
            free(code_table);
            return -1;
        }
    }

    *output = out;
    *output_size = out_pos;
    free(code_table);
    return 0;
}

Compressor canonical_huffman_compressor = {
    .name = "canonical_huffman",
    .compress = canonical_huffman_compress,
    .decompress = canonical_huffman_decompress};