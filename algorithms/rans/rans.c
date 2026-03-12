#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include "../../include/compressor.h"
#include "rans.h"

static const unsigned char RANS_MAGIC[4] = {'R', 'A', 'N', 'S'};

#define R 12
#define M (1u << R)
#define RENORM_BASE ((1ULL << 32) / M)
#define RENORM_THRESHOLD(f) ((uint64_t)(f) * RENORM_BASE)

/* -- utilities -- */

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

/* -- frequency scaling -- */

static void scale_frequencies(const uint64_t raw[256], uint32_t scaled[256])
{
    uint64_t total = 0;
    for (int i = 0; i < 256; ++i)
        total += raw[i];

    if (total == 0)
    {
        memset(scaled, 0, 256 * sizeof(uint32_t));
        return;
    }

    uint64_t sum_scaled = 0;

    for (int i = 0; i < 256; ++i)
    {
        if (raw[i] == 0)
        {
            scaled[i] = 0;
            continue;
        }

        uint32_t s = (uint32_t)(((__uint128_t)raw[i] * M) / total);
        if (s == 0)
            s = 1;

        scaled[i] = s;
        sum_scaled += s;
    }

    while (sum_scaled < M)
    {
        for (int i = 0; i < 256 && sum_scaled < M; ++i)
        {
            if (scaled[i] > 0)
            {
                scaled[i]++;
                sum_scaled++;
            }
        }
    }

    while (sum_scaled > M)
    {
        for (int i = 0; i < 256 && sum_scaled > M; ++i)
        {
            if (scaled[i] > 1)
            {
                scaled[i]--;
                sum_scaled--;
            }
        }
    }
}

/* -- byte reader -- */

typedef struct
{
    const unsigned char *buf;
    size_t len;
    size_t pos;
} ByteReader;

static void br_init(ByteReader *r, const unsigned char *buf, size_t len)
{
    r->buf = buf;
    r->len = len;
    r->pos = 0;
}

static int br_get(ByteReader *r, unsigned char *out)
{
    if (r->pos >= r->len)
    {
        *out = 0;
        return -1;
    }
    *out = r->buf[r->pos++];
    return 0;
}

/* -- rANS encode -- */

static int rans_encode_block(
    const unsigned char *input,
    size_t input_size,
    const uint64_t raw_freq[256],
    unsigned char **out_buf,
    size_t *out_size)
{
    uint32_t scaled[256];
    scale_frequencies(raw_freq, scaled);

    uint32_t cum[257];
    cum[0] = 0;

    for (int i = 0; i < 256; ++i)
        cum[i + 1] = cum[i] + scaled[i];

    ByteBuf stack;
    if (bb_init(&stack, input_size + 64) != 0)
        return -1;

    uint64_t state = (1ULL << 32);

    for (ssize_t i = (ssize_t)input_size - 1; i >= 0; --i)
    {

        unsigned sym = input[i];
        uint32_t f = scaled[sym];
        uint32_t c = cum[sym];

        uint64_t threshold = RENORM_THRESHOLD(f);

        while (state >= threshold)
        {
            if (bb_push(&stack, state & 0xFF) != 0)
            {
                bb_free(&stack);
                return -1;
            }
            state >>= 8;
        }

        uint64_t q = state / f;
        uint64_t r = state % f;

        state = (q << R) + r + c;
    }

    while (state > 0)
    {
        if (bb_push(&stack, state & 0xFF) != 0)
        {
            bb_free(&stack);
            return -1;
        }
        state >>= 8;
    }

    /* reverse stream */

    for (size_t i = 0; i < stack.len / 2; ++i)
    {
        unsigned char t = stack.buf[i];
        stack.buf[i] = stack.buf[stack.len - 1 - i];
        stack.buf[stack.len - 1 - i] = t;
    }

    size_t header = 4 + 256 * 8;
    size_t total = header + stack.len;

    unsigned char *out = malloc(total);
    if (!out)
    {
        bb_free(&stack);
        return -1;
    }

    memcpy(out, RANS_MAGIC, 4);

    for (int i = 0; i < 256; ++i)
        write_u64_le(out + 4 + i * 8, raw_freq[i]);

    memcpy(out + header, stack.buf, stack.len);

    *out_buf = out;
    *out_size = total;

    bb_free(&stack);
    return 0;
}

/* -- rANS decode -- */

static int rans_decode_block(
    const unsigned char *input,
    size_t input_size,
    unsigned char **out_buf,
    size_t *out_size)
{
    if (input_size < 4 + 256 * 8)
        return -1;

    if (memcmp(input, RANS_MAGIC, 4) != 0)
        return -1;

    uint64_t raw[256];
    uint64_t total = 0;

    for (int i = 0; i < 256; ++i)
    {
        raw[i] = read_u64_le(input + 4 + i * 8);
        total += raw[i];
    }

    uint32_t scaled[256];
    scale_frequencies(raw, scaled);

    uint32_t cum[257];
    cum[0] = 0;

    for (int i = 0; i < 256; ++i)
        cum[i + 1] = cum[i] + scaled[i];

    uint16_t table[M];
    uint32_t p = 0;

    for (int s = 0; s < 256; ++s)
        for (uint32_t k = 0; k < scaled[s]; ++k)
            table[p++] = s;

    const unsigned char *stream = input + 4 + 256 * 8;
    size_t stream_len = input_size - (4 + 256 * 8);

    ByteReader br;
    br_init(&br, stream, stream_len);

    uint64_t state = 0;

    for (int i = 0; i < 4; ++i)
    {
        unsigned char b;
        br_get(&br, &b);
        state = (state << 8) | b;
    }

    unsigned char *out = malloc(total);
    if (!out)
        return -1;

    for (uint64_t i = 0; i < total; ++i)
    {

        uint32_t idx = state & (M - 1);
        uint16_t sym = table[idx];

        out[i] = sym;

        uint32_t f = scaled[sym];
        uint32_t c = cum[sym];

        state = f * (state >> R) + (idx - c);

        while (state < (1ULL << 32))
        {
            unsigned char b;
            br_get(&br, &b);
            state = (state << 8) | b;
        }
    }

    *out_buf = out;
    *out_size = total;
    return 0;
}

/* -- interface -- */

static int rans_compress(
    const unsigned char *input,
    size_t input_size,
    unsigned char **output,
    size_t *output_size)
{
    uint64_t freq[256];
    memset(freq, 0, sizeof(freq));

    for (size_t i = 0; i < input_size; ++i)
        freq[input[i]]++;

    return rans_encode_block(input, input_size, freq, output, output_size);
}

static int rans_decompress(
    const unsigned char *input,
    size_t input_size,
    unsigned char **output,
    size_t *output_size)
{
    return rans_decode_block(input, input_size, output, output_size);
}

Compressor rans_compressor = {
    .name = "rans",
    .compress = rans_compress,
    .decompress = rans_decompress};