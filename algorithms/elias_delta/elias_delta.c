#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../../include/compressor.h"
#include "elias_delta.h"

static const unsigned char ELIAS_DELTA_MAGIC[4] = {'E', 'D', 'E', 'L'};

/* -- byte buffer -- */

typedef struct
{
    unsigned char *buf;
    size_t cap;
    size_t len;
} ByteBuf;

static int bb_init(ByteBuf *b, size_t expected)
{
    b->cap = expected ? expected : 1024;
    b->buf = malloc(b->cap);
    if (!b->buf)
        return -1;
    b->len = 0;
    return 0;
}

static void bb_free(ByteBuf *b)
{
    free(b->buf);
}

static int bb_push(ByteBuf *b, unsigned char c)
{
    if (b->len + 1 > b->cap)
    {
        size_t nc = b->cap * 2 + 1;
        unsigned char *tmp = realloc(b->buf, nc);
        if (!tmp)
            return -1;
        b->buf = tmp;
        b->cap = nc;
    }
    b->buf[b->len++] = c;
    return 0;
}

/* -- bit buffer -- */

typedef struct
{
    unsigned char *buf;
    size_t cap;
    size_t len; // in bits
} BitBuf;

static int bitb_init(BitBuf *b, size_t expected_bits)
{
    b->cap = (expected_bits + 7) / 8;
    if (b->cap == 0)
        b->cap = 1;
    b->buf = calloc(b->cap, 1); // calloc to zero
    if (!b->buf)
        return -1;
    b->len = 0;
    return 0;
}

static void bitb_free(BitBuf *b)
{
    free(b->buf);
}

static int bitb_write_bit(BitBuf *b, int bit)
{
    size_t byte_idx = b->len / 8;
    size_t bit_idx = b->len % 8;
    if (byte_idx >= b->cap)
    {
        size_t nc = b->cap * 2 + 1;
        unsigned char *tmp = realloc(b->buf, nc);
        if (!tmp)
            return -1;
        memset(tmp + b->cap, 0, nc - b->cap);
        b->buf = tmp;
        b->cap = nc;
    }
    if (bit)
    {
        b->buf[byte_idx] |= (1 << bit_idx);
    }
    b->len++;
    return 0;
}

/* -- bit reader -- */

typedef struct
{
    const unsigned char *buf;
    size_t len; // in bytes
    size_t pos; // in bits
} BitReader;

static void bitr_init(BitReader *r, const unsigned char *buf, size_t len)
{
    r->buf = buf;
    r->len = len;
    r->pos = 0;
}

static int bitr_read_bit(BitReader *r)
{
    if (r->pos >= r->len * 8)
        return -1;
    size_t byte_idx = r->pos / 8;
    size_t bit_idx = r->pos % 8;
    int bit = (r->buf[byte_idx] >> bit_idx) & 1;
    r->pos++;
    return bit;
}

/* -- elias gamma encode (helper for delta) -- */

static int elias_gamma_encode(uint32_t n, BitBuf *b)
{
    if (n == 0)
        return -1; // not defined for 0
    int k = 0;
    uint32_t temp = n;
    while (temp > 1)
    {
        k++;
        temp >>= 1;
    }
    // write k zeros
    for (int i = 0; i < k; i++)
    {
        if (bitb_write_bit(b, 0) != 0)
            return -1;
    }
    // write 1
    if (bitb_write_bit(b, 1) != 0)
        return -1;
    // write the lower k bits of n
    for (int i = k - 1; i >= 0; i--)
    {
        int bit = (n >> i) & 1;
        if (bitb_write_bit(b, bit) != 0)
            return -1;
    }
    return 0;
}

/* -- elias gamma decode (helper for delta) -- */

static int elias_gamma_decode(BitReader *r, uint32_t *out)
{
    int k = 0;
    while (1)
    {
        int bit = bitr_read_bit(r);
        if (bit == -1)
            return -1;
        if (bit == 1)
            break;
        k++;
    }
    uint32_t n = 1 << k; // the leading 1
    for (int i = k - 1; i >= 0; i--)
    {
        int bit = bitr_read_bit(r);
        if (bit == -1)
            return -1;
        if (bit)
            n |= (1 << i);
    }
    *out = n;
    return 0;
}

/* -- elias delta encode -- */

static int elias_delta_encode(uint32_t n, BitBuf *b)
{
    if (n == 0)
        return -1; // not defined for 0
    // find k = floor(log2(n))
    int k = 0;
    uint32_t temp = n;
    while (temp > 1)
    {
        k++;
        temp >>= 1;
    }
    // encode (k+1) using gamma
    if (elias_gamma_encode(k + 1, b) != 0)
        return -1;
    // write the lower k bits of n
    for (int i = k - 1; i >= 0; i--)
    {
        int bit = (n >> i) & 1;
        if (bitb_write_bit(b, bit) != 0)
            return -1;
    }
    return 0;
}

/* -- elias delta decode -- */

static int elias_delta_decode(BitReader *r, uint32_t *out)
{
    uint32_t m;
    if (elias_gamma_decode(r, &m) != 0)
        return -1;
    int k = m - 1;
    uint32_t n = 1 << k; // leading 1
    for (int i = k - 1; i >= 0; i--)
    {
        int bit = bitr_read_bit(r);
        if (bit == -1)
            return -1;
        if (bit)
            n |= (1 << i);
    }
    *out = n;
    return 0;
}

/* -- compress -- */

static int elias_delta_compress(
    const unsigned char *input,
    size_t input_size,
    unsigned char **output,
    size_t *output_size)
{
    BitBuf b;
    if (bitb_init(&b, input_size * 8) != 0)
        return -1;
    for (size_t i = 0; i < input_size; i++)
    {
        uint32_t val = input[i] + 1; // map 0-255 to 1-256
        if (elias_delta_encode(val, &b) != 0)
        {
            bitb_free(&b);
            return -1;
        }
    }
    size_t bit_len = b.len;
    size_t byte_len = (bit_len + 7) / 8;
    size_t total = 4 + byte_len;
    unsigned char *out = malloc(total);
    if (!out)
    {
        bitb_free(&b);
        return -1;
    }
    memcpy(out, ELIAS_DELTA_MAGIC, 4);
    memcpy(out + 4, b.buf, byte_len);
    *output = out;
    *output_size = total;
    bitb_free(&b);
    return 0;
}

/* -- decompress -- */

static int elias_delta_decompress(
    const unsigned char *input,
    size_t input_size,
    unsigned char **output,
    size_t *output_size)
{
    if (input_size < 4 || memcmp(input, ELIAS_DELTA_MAGIC, 4) != 0)
        return -1;
    BitReader r;
    bitr_init(&r, input + 4, input_size - 4);
    ByteBuf out;
    if (bb_init(&out, input_size) != 0)
        return -1;
    while (r.pos < (input_size - 4) * 8)
    {
        uint32_t val;
        if (elias_delta_decode(&r, &val) != 0)
            break;
        unsigned char byte = val - 1;
        if (bb_push(&out, byte) != 0)
        {
            bb_free(&out);
            return -1;
        }
    }
    *output = out.buf;
    *output_size = out.len;
    return 0;
}

Compressor elias_delta_compressor = {
    .name = "elias_delta",
    .compress = elias_delta_compress,
    .decompress = elias_delta_decompress};