#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../../include/compressor.h"

/* -- Constants-- */

static const unsigned char ARC_MAGIC[4] = {'A', 'R', 'C', '1'};

#define PRECISION 32

#define RANGE_MAX ((1ULL << PRECISION) - 1)
#define HALF (1ULL << (PRECISION - 1))
#define QUARTER (1ULL << (PRECISION - 2))
#define THREEQ (QUARTER * 3)

/* -- Utilities-- */

static void write_u64_le(unsigned char *buf, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        buf[i] = (unsigned char)((v >> (8 * i)) & 0xFF);
}

static uint64_t read_u64_le(const unsigned char *buf)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v |= ((uint64_t)buf[i]) << (8 * i);
    return v;
}

/* -- Bit Writer (MSB-first)-- */

typedef struct
{
    unsigned char *buf;
    size_t cap;
    size_t pos;

    uint8_t bitbuf;
    uint8_t bitcount;

    uint32_t pending_bits;

} BitWriter;

static int bw_ensure(BitWriter *bw, size_t extra)
{
    if (bw->pos + extra + 8 > bw->cap)
    {
        size_t nc = bw->cap * 2 + extra + 8;
        unsigned char *tmp = realloc(bw->buf, nc);
        if (!tmp)
            return -1;
        bw->buf = tmp;
        bw->cap = nc;
    }
    return 0;
}

static int bw_init(BitWriter *bw, size_t cap)
{
    bw->cap = cap ? cap : 1024;
    bw->buf = malloc(bw->cap);
    if (!bw->buf)
        return -1;

    bw->pos = 0;
    bw->bitbuf = 0;
    bw->bitcount = 0;
    bw->pending_bits = 0;

    return 0;
}

static void bw_free(BitWriter *bw)
{
    free(bw->buf);
}

static int bw_write_bit(BitWriter *bw, int bit)
{
    bw->bitbuf = (bw->bitbuf << 1) | (bit & 1);
    bw->bitcount++;

    if (bw->bitcount == 8)
    {
        if (bw_ensure(bw, 1) != 0)
            return -1;
        bw->buf[bw->pos++] = bw->bitbuf;
        bw->bitbuf = 0;
        bw->bitcount = 0;
    }

    return 0;
}

static int bw_write_bit_pending(BitWriter *bw, int bit)
{
    if (bw_write_bit(bw, bit) != 0)
        return -1;

    for (uint32_t i = 0; i < bw->pending_bits; i++)
        if (bw_write_bit(bw, !bit) != 0)
            return -1;

    bw->pending_bits = 0;
    return 0;
}

static int bw_flush(BitWriter *bw)
{
    if (bw->bitcount)
    {
        bw->bitbuf <<= (8 - bw->bitcount);
        if (bw_ensure(bw, 1) != 0)
            return -1;
        bw->buf[bw->pos++] = bw->bitbuf;
    }
    return 0;
}

/* -- Bit Reader-- */

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

static int br_read_bit(BitReader *br, int *bit)
{
    if (br->bitcount == 0)
    {
        if (br->pos >= br->len)
            return -1;
        br->bitbuf = br->buf[br->pos++];
        br->bitcount = 8;
    }

    *bit = (br->bitbuf >> 7) & 1;
    br->bitbuf <<= 1;
    br->bitcount--;

    return 0;
}

/* -- Arithmetic State-- */

typedef struct
{
    uint64_t low;
    uint64_t high;
    uint64_t value;

} ArithmeticState;

/* -- Encoding-- */

static void arith_init(ArithmeticState *s)
{
    s->low = 0;
    s->high = RANGE_MAX;
}

static int arith_renorm_encode(ArithmeticState *s, BitWriter *bw)
{
    for (;;)
    {
        if (s->high < HALF)
        {
            if (bw_write_bit_pending(bw, 0) != 0)
                return -1;
        }
        else if (s->low >= HALF)
        {
            if (bw_write_bit_pending(bw, 1) != 0)
                return -1;

            s->low -= HALF;
            s->high -= HALF;
        }
        else if (s->low >= QUARTER && s->high < THREEQ)
        {
            bw->pending_bits++;
            s->low -= QUARTER;
            s->high -= QUARTER;
        }
        else
            break;

        s->low <<= 1;
        s->high = (s->high << 1) | 1;
    }

    return 0;
}

static int arith_finish(ArithmeticState *s, BitWriter *bw)
{
    bw->pending_bits++;

    if (s->low < QUARTER)
        return bw_write_bit_pending(bw, 0);
    else
        return bw_write_bit_pending(bw, 1);
}

/* -- Decoding-- */

static int arith_decode_init(ArithmeticState *s, BitReader *br)
{
    s->low = 0;
    s->high = RANGE_MAX;

    uint64_t v = 0;

    for (int i = 0; i < PRECISION; i++)
    {
        int bit = 0;
        if (br_read_bit(br, &bit) != 0)
            bit = 0;
        v = (v << 1) | bit;
    }

    s->value = v;
    return 0;
}

static int arith_renorm_decode(ArithmeticState *s, BitReader *br)
{
    for (;;)
    {
        if (s->high < HALF)
        {
        }
        else if (s->low >= HALF)
        {
            s->value -= HALF;
            s->low -= HALF;
            s->high -= HALF;
        }
        else if (s->low >= QUARTER && s->high < THREEQ)
        {
            s->value -= QUARTER;
            s->low -= QUARTER;
            s->high -= QUARTER;
        }
        else
            break;

        s->low <<= 1;
        s->high = (s->high << 1) | 1;

        int bit = 0;
        if (br_read_bit(br, &bit) != 0)
            bit = 0;

        s->value = (s->value << 1) | bit;
    }

    return 0;
}

/* -- Compression-- */

static int arithmetic_compress(
    const unsigned char *input,
    size_t input_size,
    unsigned char **output,
    size_t *output_size)
{
    *output = NULL;
    *output_size = 0;

    uint64_t freq[256] = {0};

    for (size_t i = 0; i < input_size; i++)
        freq[input[i]]++;

    uint64_t cum[257];
    cum[0] = 0;

    for (int i = 0; i < 256; i++)
        cum[i + 1] = cum[i] + freq[i];

    uint64_t total = cum[256];
    if (total == 0)
        return -1;

    BitWriter bw;
    if (bw_init(&bw, input_size / 2 + 4096) != 0)
        return -1;

    if (bw_ensure(&bw, 4 + 256 * 8) != 0)
    {
        bw_free(&bw);
        return -1;
    }

    memcpy(bw.buf, ARC_MAGIC, 4);
    bw.pos = 4;

    for (int i = 0; i < 256; i++)
    {
        write_u64_le(bw.buf + bw.pos, freq[i]);
        bw.pos += 8;
    }

    ArithmeticState s;
    arith_init(&s);

    for (size_t i = 0; i < input_size; i++)
    {
        unsigned sym = input[i];

        uint64_t range = s.high - s.low + 1;

        s.high = s.low + (range * cum[sym + 1]) / total - 1;
        s.low = s.low + (range * cum[sym]) / total;

        if (arith_renorm_encode(&s, &bw) != 0)
        {
            bw_free(&bw);
            return -1;
        }
    }

    if (arith_finish(&s, &bw) != 0)
    {
        bw_free(&bw);
        return -1;
    }

    if (bw_flush(&bw) != 0)
    {
        bw_free(&bw);
        return -1;
    }

    unsigned char *out = malloc(bw.pos);
    if (!out)
    {
        bw_free(&bw);
        return -1;
    }

    memcpy(out, bw.buf, bw.pos);

    *output = out;
    *output_size = bw.pos;

    bw_free(&bw);
    return 0;
}

/* -- Decompression-- */

static int arithmetic_decompress(
    const unsigned char *input,
    size_t input_size,
    unsigned char **output,
    size_t *output_size)
{
    *output = NULL;
    *output_size = 0;

    if (input_size < 4 + 256 * 8)
        return -1;
    if (memcmp(input, ARC_MAGIC, 4) != 0)
        return -1;

    uint64_t freq[256];
    uint64_t cum[257];

    for (int i = 0; i < 256; i++)
        freq[i] = read_u64_le(input + 4 + i * 8);

    cum[0] = 0;
    for (int i = 0; i < 256; i++)
        cum[i + 1] = cum[i] + freq[i];

    uint64_t total = cum[256];
    if (total == 0)
        return -1;

    BitReader br;
    br_init(&br, input + 4 + 256 * 8, input_size - (4 + 256 * 8));

    ArithmeticState s;
    if (arith_decode_init(&s, &br) != 0)
        return -1;

    unsigned char *out = malloc(total);
    if (!out)
        return -1;

    for (uint64_t i = 0; i < total; i++)
    {
        uint64_t range = s.high - s.low + 1;
        uint64_t scaled = ((s.value - s.low + 1) * total - 1) / range;

        int sym = 0;
        while (cum[sym + 1] <= scaled)
            sym++;

        out[i] = sym;

        s.high = s.low + (range * cum[sym + 1]) / total - 1;
        s.low = s.low + (range * cum[sym]) / total;

        if (arith_renorm_decode(&s, &br) != 0)
        {
            free(out);
            return -1;
        }
    }

    *output = out;
    *output_size = total;

    return 0;
}

/* -- Registration-- */

Compressor arithmetic_compressor = {
    .name = "arithmetic",
    .compress = arithmetic_compress,
    .decompress = arithmetic_decompress};