#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../../include/compressor.h"
#include "lzw.h"

static const unsigned char LZW_MAGIC[4] = {'L', 'Z', 'W', '1'};

#define LZW_MAX_CODE 4095 // 12-bit codes
#define LZW_CODE_BITS 12
#define LZW_DICT_SIZE (LZW_MAX_CODE + 1)

/* -- Byte buffer        -- */

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

/* -- Bit writer / reader -- */

typedef struct
{
    ByteBuf buf;
    uint32_t bit_acc;
    int bits;
} BitWriter;

static int bw_init(BitWriter *w, size_t expected_bytes)
{
    if (bb_init(&w->buf, expected_bytes) != 0)
        return -1;
    w->bit_acc = 0;
    w->bits = 0;
    return 0;
}

static void bw_free(BitWriter *w)
{
    bb_free(&w->buf);
}

static int bw_write_bits(BitWriter *w, uint32_t value, int nbits)
{
    w->bit_acc |= (value << w->bits);
    w->bits += nbits;

    while (w->bits >= 8)
    {
        if (bb_push(&w->buf, (unsigned char)(w->bit_acc & 0xFF)) != 0)
            return -1;
        w->bit_acc >>= 8;
        w->bits -= 8;
    }
    return 0;
}

static int bw_flush(BitWriter *w)
{
    if (w->bits > 0)
    {
        if (bb_push(&w->buf, (unsigned char)(w->bit_acc & 0xFF)) != 0)
            return -1;
        w->bit_acc = 0;
        w->bits = 0;
    }
    return 0;
}

typedef struct
{
    const unsigned char *buf;
    size_t len;
    size_t pos;
    uint32_t bit_acc;
    int bits;
} BitReader;

static void br_init(BitReader *r, const unsigned char *buf, size_t len)
{
    r->buf = buf;
    r->len = len;
    r->pos = 0;
    r->bit_acc = 0;
    r->bits = 0;
}

static int br_read_bits(BitReader *r, uint32_t *out, int nbits)
{
    while (r->bits < nbits)
    {
        if (r->pos >= r->len)
            return -1;
        r->bit_acc |= ((uint32_t)r->buf[r->pos++]) << r->bits;
        r->bits += 8;
    }
    *out = r->bit_acc & (((uint32_t)1 << nbits) - 1);
    r->bit_acc >>= nbits;
    r->bits -= nbits;
    return 0;
}

/* -- LZW encode / decode helpers -- */

typedef struct
{
    uint16_t prefix;
    unsigned char suffix;
} LzwEntry;

static void dict_init(LzwEntry dict[LZW_DICT_SIZE])
{
    for (int i = 0; i < 256; ++i)
    {
        dict[i].prefix = 0xFFFF;
        dict[i].suffix = (unsigned char)i;
    }
}

/*
 * Reconstruct a dictionary sequence into `out` (most recent byte last).
 * Returns length of the sequence, or 0 on error.
 */
static size_t dict_reconstruct(const LzwEntry dict[LZW_DICT_SIZE], uint16_t code, unsigned char *out, size_t out_cap)
{
    size_t idx = 0;
    while (code != 0xFFFF && idx < out_cap)
    {
        out[idx++] = dict[code].suffix;
        code = dict[code].prefix;
    }
    // reverse
    for (size_t i = 0; i < idx / 2; ++i)
    {
        unsigned char t = out[i];
        out[i] = out[idx - 1 - i];
        out[idx - 1 - i] = t;
    }
    return idx;
}

/* -- LZW compress -- */

static int lzw_compress(
    const unsigned char *input,
    size_t input_size,
    unsigned char **output,
    size_t *output_size)
{
    if (input_size == 0)
        return -1;

    BitWriter bw;
    if (bw_init(&bw, input_size) != 0)
        return -1;

    if (bb_push(&bw.buf, LZW_MAGIC[0]) != 0 ||
        bb_push(&bw.buf, LZW_MAGIC[1]) != 0 ||
        bb_push(&bw.buf, LZW_MAGIC[2]) != 0 ||
        bb_push(&bw.buf, LZW_MAGIC[3]) != 0)
    {
        bw_free(&bw);
        return -1;
    }

    LzwEntry dict[LZW_DICT_SIZE];
    dict_init(dict);

    uint16_t next_code = 256;
    uint16_t w = input[0];

    for (size_t i = 1; i < input_size; ++i)
    {
        unsigned char k = input[i];
        uint16_t match = 0xFFFF;

        // find in dictionary: prefix=w, suffix=k
        for (uint16_t c = 256; c < next_code; ++c)
        {
            if (dict[c].prefix == w && dict[c].suffix == k)
            {
                match = c;
                break;
            }
        }

        if (match != 0xFFFF)
        {
            w = match;
        }
        else
        {
            if (bw_write_bits(&bw, w, LZW_CODE_BITS) != 0)
            {
                bw_free(&bw);
                return -1;
            }
            if (next_code <= LZW_MAX_CODE)
            {
                dict[next_code].prefix = w;
                dict[next_code].suffix = k;
                next_code++;
            }
            w = k;
        }
    }

    if (bw_write_bits(&bw, w, LZW_CODE_BITS) != 0)
    {
        bw_free(&bw);
        return -1;
    }

    if (bw_flush(&bw) != 0)
    {
        bw_free(&bw);
        return -1;
    }

    *output = bw.buf.buf;
    *output_size = bw.buf.len;
    return 0;
}

/* -- LZW decompress -- */

static int lzw_decompress(
    const unsigned char *input,
    size_t input_size,
    unsigned char **output,
    size_t *output_size)
{
    if (input_size < 4 || memcmp(input, LZW_MAGIC, 4) != 0)
        return -1;

    BitReader br;
    br_init(&br, input + 4, input_size - 4);

    ByteBuf out;
    if (bb_init(&out, input_size * 2) != 0)
        return -1;

    LzwEntry dict[LZW_DICT_SIZE];
    dict_init(dict);
    uint16_t next_code = 256;

    uint32_t prev_code;
    if (br_read_bits(&br, &prev_code, LZW_CODE_BITS) != 0)
        return -1;

    // output first code (always a single byte)
    if (bb_push(&out, (unsigned char)prev_code) != 0)
    {
        bb_free(&out);
        return -1;
    }

    unsigned char seq_buf[4096];

    while (1)
    {
        uint32_t cur_code;
        if (br_read_bits(&br, &cur_code, LZW_CODE_BITS) != 0)
            break;

        size_t seq_len;
        if (cur_code < next_code)
        {
            seq_len = dict_reconstruct(dict, (uint16_t)cur_code, seq_buf, sizeof(seq_buf));
        }
        else if (cur_code == next_code)
        {
            // special case: KWCW+K
            seq_len = dict_reconstruct(dict, prev_code, seq_buf, sizeof(seq_buf));
            if (seq_len == 0)
                return -1;
            if (seq_len + 1 > sizeof(seq_buf))
                return -1;
            seq_buf[seq_len] = seq_buf[0];
            seq_len += 1;
        }
        else
        {
            return -1;
        }

        for (size_t i = 0; i < seq_len; ++i)
        {
            if (bb_push(&out, seq_buf[i]) != 0)
            {
                bb_free(&out);
                return -1;
            }
        }

        if (next_code <= LZW_MAX_CODE)
        {
            // add new dictionary entry
            uint16_t first_char = seq_buf[0];
            dict[next_code].prefix = (uint16_t)prev_code;
            dict[next_code].suffix = (unsigned char)first_char;
            next_code++;
        }

        prev_code = (uint16_t)cur_code;
    }

    *output = out.buf;
    *output_size = out.len;
    return 0;
}

Compressor lzw_compressor = {
    .name = "lzw",
    .compress = lzw_compress,
    .decompress = lzw_decompress};