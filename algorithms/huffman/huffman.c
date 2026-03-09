/*
 * Huffman Compressor and Decompressor
 * ====================================
 *
 * Overview:
 * ---------
 * This module implements Huffman coding, a lossless data compression algorithm
 * that uses variable-length codes to represent symbols based on their frequency
 * of occurrence. More frequent symbols get shorter codes, achieving compression.
 *
 * File Format:
 * ------------
 * Compressed files have the following structure:
 *   - Magic number (4 bytes): "HUF1"
 *   - Frequency table (256 × 8 = 2048 bytes): 64-bit little-endian frequencies
 *     for each possible byte value (0-255)
 *   - Bitstream: Variable-length Huffman codes for each input byte, packed
 *     LSB-first into bytes
 *
 * Compression Process:
 * --------------------
 * 1. Frequency Analysis: Count occurrences of each byte value (0-255) in input
 * 2. Tree Construction: Build Huffman tree using a min-heap:
 *    - Create leaf nodes for each symbol with non-zero frequency
 *    - Repeatedly merge two lowest-frequency nodes into a parent node
 *    - Continue until one node remains (the tree root)
 * 3. Code Generation: Traverse tree to generate binary codes:
 *    - Left edge = 0, Right edge = 1
 *    - Store codes with their lengths for each symbol
 * 4. Encoding: Write header (magic + frequencies) followed by bitstream:
 *    - For each input byte, write its Huffman code using BitWriter
 *    - Codes are written LSB-first and packed into bytes
 *
 * Decompression Process:
 * ----------------------
 * 1. Header Parsing: Read magic number and frequency table
 * 2. Tree Reconstruction: Rebuild exact same Huffman tree from frequencies
 * 3. Decoding: Read bits one at a time using BitReader:
 *    - Start at root, follow bits (0=left, 1=right) until reaching a leaf
 *    - Output the leaf's symbol
 *    - Repeat until all original symbols are recovered
 *
 * Data Structures:
 * ----------------
 * - HNode: Huffman tree node with frequency, symbol (or -1 for internal),
 *          and left/right children
 * - MinHeap: Binary min-heap for building tree (ordered by frequency)
 * - Code: Stores Huffman code (bits and length) for a symbol
 * - BitWriter: Handles bit-level output with buffering
 * - BitReader: Handles bit-level input with buffering
 *
 * Special Cases:
 * --------------
 * - Empty input: Only header is written (all frequencies zero)
 * - Single symbol: Artificial parent node created so codes have length 1
 *
 * Bit I/O Details:
 * ----------------
 * - All bit operations use LSB-first ordering (least significant bit first)
 * - Bits are accumulated in a buffer and written when full
 * - This ensures consistent encoding/decoding across platforms
 *
 * Complexity:
 * -----------
 * - Time: O(n + k log k) where n is input size, k is alphabet size (256)
 * - Space: O(k) for tree and heap, plus output buffer
 *
 * Limitations:
 * ------------
 * - Maximum file size: Limited by 64-bit frequency counters (~16 exabytes)
 * - Compression ratio: Depends on data entropy (worst case: slightly larger)
 */

#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../../include/compressor.h"
#include "huffman.h"

/* -- constants & data structs -- */

static const unsigned char HUF_MAGIC[4] = {'H', 'U', 'F', '1'};
typedef struct HNode
{
    uint64_t freq;
    int symbol; // 0..255 for leaves and -1 for internal nodes
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

/* -- min heap -- */

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
    if (h->size >= h->capacity) // if need more space
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
    while (i > 0) // sift up
    {
        size_t p = (i - 1) / 2; // parent index
        if (h->data[p]->freq <= h->data[i]->freq)
            break;
        // swap with parent
        HNode *t = h->data[p];
        h->data[p] = h->data[i];
        h->data[i] = t;
        i = p; // move up
    }
    return 0;
}

static HNode *heap_pop(MinHeap *h)
{
    if (h->size == 0)
        return NULL;
    HNode *ret = h->data[0];         // root = min
    h->data[0] = h->data[--h->size]; // move last to root
    size_t i = 0;
    while (1) // sift down
    {
        size_t l = 2 * i + 1, r = l + 1, smallest = i;
        if (l < h->size && h->data[l]->freq < h->data[smallest]->freq)
            smallest = l;
        if (r < h->size && h->data[r]->freq < h->data[smallest]->freq)
            smallest = r;
        if (smallest == i)
            break;
        // swap with smaller child
        HNode *t = h->data[i];
        h->data[i] = h->data[smallest];
        h->data[smallest] = t;
        i = smallest; // move down
    }
    return ret;
}

/* -- Huffman tree construction from freq table -- */

static HNode *node_create(uint64_t freq, int symbol)
{
    HNode *n = malloc(sizeof(*n)); // new Huffman tree node
    if (!n)
        return NULL;
    n->freq = freq;
    n->symbol = symbol;        // sym or -1 for internal
    n->left = n->right = NULL; // init with no children
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

/* -- code gen -- */

static void build_codes_from_tree(HNode *root, Code codes[256])
{
    struct StackItem
    {
        HNode *node;
        uint64_t bits;
        uint8_t len;
    };
    struct StackItem *stack = malloc(sizeof(*stack) * 512);
    size_t sp = 0;
    if (!stack)
        return;
    for (int i = 0; i < 256; ++i) // init all codes to zero
    {
        codes[i].bits = 0;
        codes[i].len = 0;
    }
    if (!root)
    {
        free(stack);
        return;
    }
    // push root onto stack
    stack[sp++] = (struct StackItem){.node = root, .bits = 0, .len = 0};
    while (sp > 0) // traverse depth-first
    {
        struct StackItem it = stack[--sp];
        HNode *n = it.node;
        if (!n->left && !n->right) // leaf node
        {
            if (n->symbol >= 0 && n->symbol < 256)
            {
                codes[(unsigned char)n->symbol].bits = it.bits;
                codes[(unsigned char)n->symbol].len = it.len;
            }
        }
        else // internal node: push children (right first so left processed first)
        {
            if (n->right)
            {
                // right branch: append 1 bit
                stack[sp++] = (struct StackItem){
                    .node = n->right,
                    .bits = (it.bits | ((uint64_t)1 << it.len)),
                    .len = (uint8_t)(it.len + 1)};
            }
            if (n->left)
            {
                // Left branch: append 0 bit (bits unchanged)
                stack[sp++] = (struct StackItem){
                    .node = n->left,
                    .bits = it.bits,
                    .len = (uint8_t)(it.len + 1)};
            }
        }
    }
    free(stack);
}

/* -- bit writer -- */

typedef struct
{
    unsigned char *buf; // output buffer
    size_t cap;         // buffer capacity
    size_t pos;         // next byte position for completed bytes
    uint8_t bitbuf;     // buffer for partial bits (lsb-first)
    uint8_t bitcount;   // number of bits currently in bitbuf
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

// ensure buffer has space for 'extra' more bytes
static int bw_ensure(BitWriter *bw, size_t extra)
{
    if (bw->pos + extra + 1 > bw->cap)
    {
        size_t nc = (bw->cap * 2) + extra; // double and add extra
        unsigned char *tmp = realloc(bw->buf, nc);
        if (!tmp)
            return -1;
        bw->buf = tmp;
        bw->cap = nc;
    }
    return 0;
}

// write 'len' bits from 'bits' (lsb-first)
static int bw_write_bits(BitWriter *bw, uint64_t bits, uint8_t len)
{
    for (uint8_t i = 0; i < len; ++i)
    {
        uint8_t bit = (bits >> i) & 1;       // extract bit
        bw->bitbuf |= (bit << bw->bitcount); // append to buffer
        bw->bitcount++;
        if (bw->bitcount == 8) // byte is full
        {
            if (bw_ensure(bw, 1) != 0)
                return -1;
            bw->buf[bw->pos++] = bw->bitbuf; // write completed byte
            bw->bitbuf = 0;                  // reset buffer
            bw->bitcount = 0;
        }
    }
    return 0;
}

// flush any remaining bits to output
static int bw_flush(BitWriter *bw)
{
    if (bw->bitcount > 0)
    {
        if (bw_ensure(bw, 1) != 0)
            return -1;
        bw->buf[bw->pos++] = bw->bitbuf; // write last partial byte
        bw->bitbuf = 0;
        bw->bitcount = 0;
    }
    return 0;
}

/* -- bit reader -- */

typedef struct
{
    const unsigned char *buf; // init buffer
    size_t len;               // total avail bytes
    size_t pos;               // current byte position
    uint8_t bitbuf;           // current byte being read bit-by-bit
    uint8_t bitcount;         // bits remaining in bitbuf
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
    if (br->bitcount == 0) // need a new byte
    {
        if (br->pos >= br->len)
            return -1;                   // EOF
        br->bitbuf = br->buf[br->pos++]; // read next byte
        br->bitcount = 8;                // 8 bits available
    }
    *out_bit = br->bitbuf & 1; // extract lsb
    br->bitbuf >>= 1;          // shift out the bit
    br->bitcount--;
    return 0;
}

/* -- compression function -- */

static int huffman_compress(
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
        size_t hdr = 4 + 256 * 8; // header size: magic + frequencies
        unsigned char *out = malloc(hdr);
        if (!out)
            return -1;
        memcpy(out, HUF_MAGIC, 4);   // write magic
        memset(out + 4, 0, 256 * 8); // zero frequencies
        *output = out;
        *output_size = hdr;
        return 0;
    }
    // count frequencies
    uint64_t freq[256];
    memset(freq, 0, sizeof(freq));
    for (size_t i = 0; i < input_size; ++i)
        freq[input[i]]++;
    // build heap from frequencies
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
            heap_push(heap, n);
        }
    }
    // special case: only one distinct symbol
    if (heap->size == 1)
    {
        HNode *only = heap_pop(heap);
        HNode *root = node_create(only->freq, -1); // create artificial parent
        root->left = only;                         // so codes have length 1
        heap_push(heap, root);
    }
    // build huffman tree by repeatedly merging smallest nodes
    while (heap->size > 1)
    {
        HNode *a = heap_pop(heap);                     // smallest
        HNode *b = heap_pop(heap);                     // second smallest
        HNode *p = node_create(a->freq + b->freq, -1); // parent node
        if (!p)
        {
            free_tree(a);
            free_tree(b);
            heap_destroy(heap);
            return -1;
        }
        p->left = a;
        p->right = b;
        heap_push(heap, p);
    }
    HNode *root = heap_pop(heap); // final tree root
    heap_destroy(heap);
    // generate codes from tree
    Code codes[256];
    memset(codes, 0, sizeof(codes));
    build_codes_from_tree(root, codes);
    // initialize bit writer with estimated size
    BitWriter bw;
    if (bw_init(&bw, input_size / 2 + 1024) != 0)
    {
        free_tree(root);
        return -1;
    }
    // write encoded symbols to bitstream
    for (size_t i = 0; i < input_size; ++i)
    {
        unsigned char sym = input[i];
        Code c = codes[sym];
        if (c.len == 0)
        {
            bw_free(&bw);
            free_tree(root);
            return -1;
        }
        if (bw_write_bits(&bw, c.bits, c.len) != 0)
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
    // build final output: magic + frequencies + bitstream
    size_t header_size = 4 + 256 * 8;
    size_t total = header_size + bw.pos;
    unsigned char *out = malloc(total);
    if (!out)
    {
        bw_free(&bw);
        free_tree(root);
        return -1;
    }
    memcpy(out, HUF_MAGIC, 4);    // write magic
    for (int i = 0; i < 256; ++i) // write frequencies
    {
        unsigned char tmp[8];
        write_u64_le(tmp, freq[i]);
        memcpy(out + 4 + i * 8, tmp, 8);
    }
    memcpy(out + header_size, bw.buf, bw.pos); // write bitstream

    *output = out;
    *output_size = total;

    bw_free(&bw);
    free_tree(root);

    return 0;
}

/* -- decompression function -- */

static int huffman_decompress(
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
    if (memcmp(input, HUF_MAGIC, 4) != 0)
        return -1;
    // read frequencies from header
    const unsigned char *p = input + 4;
    uint64_t freq[256];
    uint64_t total_symbols = 0;
    for (int i = 0; i < 256; ++i)
    {
        freq[i] = read_u64_le(p + i * 8);
        total_symbols += freq[i];
    }
    size_t header_size = 4 + 256 * 8;
    const unsigned char *bitstream = input + header_size;
    size_t bitstream_len = input_size - header_size;
    // handle empty input
    if (total_symbols == 0)
    {
        *output = malloc(0);
        *output_size = 0;
        return 0;
    }
    // rebuild tree from frequencies (same algorithm as compression)
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
            heap_push(heap, n);
        }
    }
    // handle single symbol case
    if (heap->size == 1)
    {
        HNode *only = heap_pop(heap);
        HNode *root = node_create(only->freq, -1);
        root->left = only;
        heap_push(heap, root);
    }
    // build tree by merging
    while (heap->size > 1)
    {
        HNode *a = heap_pop(heap);
        HNode *b = heap_pop(heap);
        HNode *pnode = node_create(a->freq + b->freq, -1);
        if (!pnode)
        {
            free_tree(a);
            free_tree(b);
            heap_destroy(heap);
            return -1;
        }
        pnode->left = a;
        pnode->right = b;
        heap_push(heap, pnode);
    }
    HNode *root = heap_pop(heap);
    heap_destroy(heap);
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
    // walk tree according to bits until all symbols decoded
    while (out_pos < total_symbols)
    {
        HNode *cur = root;
        while (cur->left || cur->right)
        { // traverse to leaf
            int bit;
            if (br_read_bit(&br, &bit) != 0)
            {
                free(out);
                free_tree(root);
                return -1; // unexpected EOF
            }
            if (bit)
                cur = cur->right; // 1 = right branch
            else
                cur = cur->left; // 0 = left branch
            if (!cur)            // invalid tree
            {
                free(out);
                free_tree(root);
                return -1;
            }
        }
        // leaf reached - output symbol
        out[out_pos++] = (unsigned char)cur->symbol;
    }

    *output = out;
    *output_size = out_pos;
    free_tree(root);
    return 0;
}

Compressor huffman_compressor = {
    .name = "huffman",
    .compress = huffman_compress,
    .decompress = huffman_decompress};