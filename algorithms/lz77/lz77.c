#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../../include/compressor.h"
#include "lz77.h"

static const unsigned char LZ77_MAGIC[4] = {'L', 'Z', '7', '7'};

#define LZ77_WINDOW_SIZE (1 << 12) // 4096 bytes
#define LZ77_LOOKAHEAD 18          // typical LZ77 lookahead
#define LZ77_MIN_MATCH 3           // minimum match length

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

static int bb_push_bytes(ByteBuf *b, const unsigned char *data, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        if (bb_push(b, data[i]) != 0)
            return -1;
    return 0;
}

static void write_u16_le(unsigned char *buf, uint16_t v)
{
    buf[0] = (unsigned char)(v & 0xFF);
    buf[1] = (unsigned char)((v >> 8) & 0xFF);
}

static uint16_t read_u16_le(const unsigned char *buf)
{
    return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
}

/* -- LZ77 encoding helpers -- */

// Find longest match in sliding window (naive O(n^2) scan).
// Returns match length and sets match_offset.
static void lz77_find_longest_match(const unsigned char *input,
                                    size_t input_size,
                                    size_t pos,
                                    size_t *out_offset,
                                    size_t *out_len)
{
    size_t end = input_size;
    size_t max_len = 0;
    size_t best_off = 0;

    size_t window_start = (pos > LZ77_WINDOW_SIZE) ? (pos - LZ77_WINDOW_SIZE) : 0;
    size_t max_lookahead = LZ77_LOOKAHEAD;
    if (pos + max_lookahead > end)
        max_lookahead = end - pos;

    for (size_t w = window_start; w < pos; ++w)
    {
        size_t match_len = 0;
        while (match_len < max_lookahead &&
               input[w + match_len] == input[pos + match_len])
        {
            match_len++;
        }

        if (match_len > max_len)
        {
            max_len = match_len;
            best_off = pos - w;
            if (max_len == max_lookahead)
                break;
        }
    }

    if (max_len < LZ77_MIN_MATCH)
        max_len = 0;

    *out_offset = best_off;
    *out_len = max_len;
}

/* -- compress -- */

static int lz77_compress(
    const unsigned char *input,
    size_t input_size,
    unsigned char **output,
    size_t *output_size)
{
    ByteBuf out;
    if (bb_init(&out, input_size + 16) != 0)
        return -1;

    // magic header
    if (bb_push_bytes(&out, LZ77_MAGIC, 4) != 0)
    {
        bb_free(&out);
        return -1;
    }

    size_t pos = 0;
    while (pos < input_size)
    {
        unsigned char control = 0;
        size_t control_pos = out.len;
        if (bb_push(&out, 0) != 0) // placeholder for control byte
        {
            bb_free(&out);
            return -1;
        }

        // encode up to 8 tokens under this control byte
        for (int bit = 0; bit < 8 && pos < input_size; ++bit)
        {
            size_t match_off = 0;
            size_t match_len = 0;
            lz77_find_longest_match(input, input_size, pos, &match_off, &match_len);

            if (match_len > 0)
            {
                // mark a match token (bit=1)
                control |= (1u << bit);

                // store offset (2 bytes) and length (1 byte)
                uint16_t off16 = (uint16_t)match_off;
                unsigned char len8 = (unsigned char)match_len;
                unsigned char token[3];
                write_u16_le(token, off16);
                token[2] = len8;
                if (bb_push_bytes(&out, token, sizeof(token)) != 0)
                {
                    bb_free(&out);
                    return -1;
                }
                pos += match_len;
            }
            else
            {
                // literal token (bit=0)
                if (bb_push(&out, input[pos]) != 0)
                {
                    bb_free(&out);
                    return -1;
                }
                pos++;
            }
        }

        // write control byte back
        out.buf[control_pos] = control;
    }

    *output = out.buf;
    *output_size = out.len;
    return 0;
}

/* -- decompress -- */

static int lz77_decompress(
    const unsigned char *input,
    size_t input_size,
    unsigned char **output,
    size_t *output_size)
{
    if (input_size < 4 || memcmp(input, LZ77_MAGIC, 4) != 0)
        return -1;

    const unsigned char *ptr = input + 4;
    size_t remaining = input_size - 4;

    ByteBuf out;
    if (bb_init(&out, remaining * 2) != 0)
        return -1;

    while (remaining > 0)
    {
        if (remaining < 1)
            break;

        unsigned char control = *ptr++;
        remaining--;

        for (int bit = 0; bit < 8 && remaining > 0; ++bit)
        {
            if (control & (1u << bit))
            {
                // match token
                if (remaining < 3)
                {
                    bb_free(&out);
                    return -1;
                }
                uint16_t off = read_u16_le(ptr);
                unsigned char len = ptr[2];
                ptr += 3;
                remaining -= 3;

                if (off == 0 || len == 0)
                {
                    bb_free(&out);
                    return -1;
                }

                size_t start = out.len - off;
                for (size_t i = 0; i < len; ++i)
                {
                    unsigned char c = out.buf[start + i];
                    if (bb_push(&out, c) != 0)
                    {
                        bb_free(&out);
                        return -1;
                    }
                }
            }
            else
            {
                // literal token
                if (remaining < 1)
                {
                    bb_free(&out);
                    return -1;
                }
                if (bb_push(&out, *ptr++) != 0)
                {
                    bb_free(&out);
                    return -1;
                }
                remaining--;
            }
        }
    }

    *output = out.buf;
    *output_size = out.len;
    return 0;
}

Compressor lz77_compressor = {
    .name = "lz77",
    .compress = lz77_compress,
    .decompress = lz77_decompress};