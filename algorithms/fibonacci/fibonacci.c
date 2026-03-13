#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../../include/compressor.h"
#include "fibonacci.h"

static const unsigned char FIB_MAGIC[4] = {'F', 'I', 'B', '1'};

static void write_u64_le(unsigned char *buf, uint64_t v)
{
    for (int i = 0; i < 8; ++i)
        buf[i] = (unsigned char)((v >> (8 * i)) & 0xFF);
}

static uint64_t read_u64_le(const unsigned char *buf)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= ((uint64_t)buf[i]) << (8 * i);
    return v;
}

/* -- byte buffer (for output) -- */

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

/* -- bit writer -- */

typedef struct
{
    unsigned char *buf;
    size_t cap;
    size_t bitlen;
} BitBuf;

static int bitb_init(BitBuf *b, size_t expected_bits)
{
    b->cap = (expected_bits + 7) / 8;
    if (b->cap == 0)
        b->cap = 1;
    b->buf = calloc(b->cap, 1);
    if (!b->buf)
        return -1;
    b->bitlen = 0;
    return 0;
}

static void bitb_free(BitBuf *b)
{
    free(b->buf);
}

static int bitb_write_bit(BitBuf *b, int bit)
{
    size_t byte_idx = b->bitlen / 8;
    size_t bit_idx = b->bitlen % 8;

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
        b->buf[byte_idx] |= (1u << bit_idx);

    b->bitlen++;
    return 0;
}

/* -- bit reader -- */

typedef struct
{
    const unsigned char *buf;
    size_t bytelength;
    size_t bitpos;
} BitReader;

static void bitr_init(BitReader *r, const unsigned char *buf, size_t len)
{
    r->buf = buf;
    r->bytelength = len;
    r->bitpos = 0;
}

static int bitr_read_bit(BitReader *r)
{
    if (r->bitpos >= r->bytelength * 8)
        return -1;
    size_t byte_idx = r->bitpos / 8;
    size_t bit_idx = r->bitpos % 8;
    int bit = (r->buf[byte_idx] >> bit_idx) & 1;
    r->bitpos++;
    return bit;
}

/* -- fibonacci coding (Zeckendorf) -- */

static void fib_build(uint32_t fib[32], int *count)
{
    fib[0] = 1;
    fib[1] = 2;
    int i = 2;
    while (1)
    {
        uint32_t next = fib[i - 1] + fib[i - 2];
        if (next > UINT32_MAX / 2) // safe guard
            break;
        fib[i++] = next;
        if (next > 1000000) // more than enough for 1..256
            break;
    }
    *count = i;
}

static int fibonacci_encode_uint(uint32_t n, BitBuf *out)
{
    if (n == 0)
        return -1; // invalid (gamma/fib codes are for positive ints)

    uint32_t fib[32];
    int fib_count;
    fib_build(fib, &fib_count);

    int used[32] = {0};

    // Zeckendorf representation
    for (int i = fib_count - 1; i >= 0; --i)
    {
        if (fib[i] <= n)
        {
            used[i] = 1;
            n -= fib[i];
        }
    }

    // Write bits from smallest fib to largest fib used, then terminator '1'
    int highest = -1;
    for (int i = 0; i < fib_count; ++i)
        if (used[i])
            highest = i;
    if (highest < 0)
        return -1;

    for (int i = 0; i <= highest; ++i)
    {
        if (bitb_write_bit(out, used[i]) != 0)
            return -1;
    }

    // terminator bit
    if (bitb_write_bit(out, 1) != 0)
        return -1;

    return 0;
}

static int fibonacci_decode_uint(BitReader *in, uint32_t *out)
{
    uint32_t fib[32];
    int fib_count;
    fib_build(fib, &fib_count);

    uint32_t value = 0;
    int prev_bit = 0;
    int index = 0;

    while (1)
    {
        int bit = bitr_read_bit(in);
        if (bit < 0)
            return -1;

        if (bit)
        {
            // terminator (two consecutive 1s) is only guaranteed after a 1 bit
            if (prev_bit == 1)
            {
                // we consumed the terminator; value is complete
                *out = value;
                return 0;
            }
        }

        if (bit)
        {
            if (index >= fib_count)
                return -1;
            value += fib[index];
        }

        prev_bit = bit;
        index++;
    }
}

/* -- compress/decompress interface -- */

static int fibonacci_compress(
    const unsigned char *input,
    size_t input_size,
    unsigned char **output,
    size_t *output_size)
{
    BitBuf bits;
    if (bitb_init(&bits, input_size * 12) != 0) // estimate
        return -1;

    for (size_t i = 0; i < input_size; ++i)
    {
        uint32_t val = (uint32_t)input[i] + 1; // map 0..255 -> 1..256
        if (fibonacci_encode_uint(val, &bits) != 0)
        {
            bitb_free(&bits);
            return -1;
        }
    }

    size_t bitlen = bits.bitlen;
    size_t bytelen = (bitlen + 7) / 8;
    size_t total = 4 + 8 + bytelen; // magic + original size + bitstream
    unsigned char *out = malloc(total);
    if (!out)
    {
        bitb_free(&bits);
        return -1;
    }

    memcpy(out, FIB_MAGIC, 4);
    write_u64_le(out + 4, (uint64_t)input_size);
    memcpy(out + 12, bits.buf, bytelen);

    *output = out;
    *output_size = total;
    bitb_free(&bits);
    return 0;
}

static int fibonacci_decompress(
    const unsigned char *input,
    size_t input_size,
    unsigned char **output,
    size_t *output_size)
{
    if (input_size < 12 || memcmp(input, FIB_MAGIC, 4) != 0)
        return -1;

    uint64_t orig_sz = read_u64_le(input + 4);
    unsigned char *out = malloc(orig_sz);
    if (!out)
        return -1;

    BitReader br;
    bitr_init(&br, input + 12, input_size - 12);

    for (uint64_t i = 0; i < orig_sz; ++i)
    {
        uint32_t v;
        if (fibonacci_decode_uint(&br, &v) != 0)
        {
            free(out);
            return -1;
        }
        if (v == 0 || v > 256)
        {
            free(out);
            return -1;
        }
        out[i] = (unsigned char)(v - 1);
    }

    *output = out;
    *output_size = (size_t)orig_sz;
    return 0;
}

Compressor fibonacci_compressor = {
    .name = "fibonacci",
    .compress = fibonacci_compress,
    .decompress = fibonacci_decompress};