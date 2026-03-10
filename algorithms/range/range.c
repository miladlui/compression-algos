#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../../include/compressor.h"
#include "range.h"

static const unsigned char RAN_MAGIC[4] = {'R', 'A', 'N', '1'};

// Renormalization constants
#define PRECISION_BITS 32U
// when range <= TOP, renormalize by emitting/consuming one byte
#define TOP (1ULL << 24) // 0x01000000
// initial full range
#define FULL_RANGE 0xFFFFFFFFULL

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

/* -- byte writer (simple) -- */

typedef struct
{
    unsigned char *buf;
    size_t cap;
    size_t pos;
} ByteWriter;

static int bw_init(ByteWriter *w, size_t expected)
{
    w->cap = expected ? expected : 1024;
    w->buf = malloc(w->cap);
    if (!w->buf)
        return -1;
    w->pos = 0;
    return 0;
}

static void bw_free(ByteWriter *w) { free(w->buf); }

static int bw_ensure(ByteWriter *w, size_t extra)
{
    if (w->pos + extra > w->cap)
    {
        size_t nc = w->cap * 2 + extra;
        unsigned char *tmp = realloc(w->buf, nc);
        if (!tmp)
            return -1;
        w->buf = tmp;
        w->cap = nc;
    }
    return 0;
}

static int bw_put_byte(ByteWriter *w, unsigned char b)
{
    if (bw_ensure(w, 1) != 0)
        return -1;
    w->buf[w->pos++] = b;
    return 0;
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

static int br_get_byte(ByteReader *r, unsigned char *out)
{
    if (r->pos >= r->len)
        return -1;
    *out = r->buf[r->pos++];
    return 0;
}

/* -- range coder encode/decode -- */

/*
 Encoder state:
   low   : 64-bit accumulator (we keep it in high precision)
   range : current range (uint64_t)
 After narrowing by symbol, while range <= TOP:
   emit top byte of low >> 24, then low <<= 8, range <<= 8
 Finish: emit 4 more bytes of low >> 24 (to flush)
*/

static int range_encode_block(
    const unsigned char *input, size_t input_size,
    const uint64_t freq[256],
    unsigned char **out_buf, size_t *out_size)
{
    if (!out_buf || !out_size)
        return -1;
    *out_buf = NULL;
    *out_size = 0;

    // build cumulative
    uint64_t cum[257];
    cum[0] = 0;
    for (int i = 0; i < 256; ++i)
        cum[i + 1] = cum[i] + freq[i];
    uint64_t total = cum[256];
    if (total == 0)
    {
        // nothing to encode
        *out_buf = malloc(4 + 256 * 8);
        if (!*out_buf)
            return -1;
        memcpy(*out_buf, RAN_MAGIC, 4);
        memset(*out_buf + 4, 0, 256 * 8);
        *out_size = 4 + 256 * 8;
        return 0;
    }

    ByteWriter bw;
    if (bw_init(&bw, input_size + 512) != 0)
        return -1;

    // write header: magic + 256 u64 frequencies (little-endian)
    if (bw_ensure(&bw, 4 + 256 * 8) != 0)
    {
        bw_free(&bw);
        return -1;
    }
    memcpy(bw.buf + bw.pos, RAN_MAGIC, 4);
    bw.pos += 4;
    for (int i = 0; i < 256; ++i)
    {
        write_u64_le(bw.buf + bw.pos, freq[i]);
        bw.pos += 8;
    }

    uint64_t low = 0;
    uint64_t range = FULL_RANGE;

    for (size_t i = 0; i < input_size; ++i)
    {
        unsigned sym = input[i];
        // compute scaled range
        if (freq[sym] == 0)
        {
            bw_free(&bw);
            return -1;
        } // should not happen
        uint64_t r = range / total;
        uint64_t start = r * cum[sym];
        uint64_t size = r * freq[sym];

        low += start;
        range = size;

        // renormalize: emit bytes while range <= TOP
        while (range <= TOP)
        {
            unsigned char outb = (unsigned char)(low >> 24);
            if (bw_put_byte(&bw, outb) != 0)
            {
                bw_free(&bw);
                return -1;
            }
            low = (low & 0xFFFFFFULL) << 8; // keep low's low 24 bits, shift left 8
            range <<= 8;
        }
    }

    // flush: emit 4 bytes to ensure decoder can finish
    for (int i = 0; i < 4; ++i)
    {
        unsigned char outb = (unsigned char)(low >> 24);
        if (bw_put_byte(&bw, outb) != 0)
        {
            bw_free(&bw);
            return -1;
        }
        low = (low & 0xFFFFFFULL) << 8;
    }

    // return buffer (shrink to fit)
    unsigned char *ret = malloc(bw.pos);
    if (!ret)
    {
        bw_free(&bw);
        return -1;
    }
    memcpy(ret, bw.buf, bw.pos);
    *out_buf = ret;
    *out_size = bw.pos;
    bw_free(&bw);
    return 0;
}

/*
 Decoder:
   read header frequencies -> cum[] and total
   init: read first 4 bytes into 'code' (uint64)
   set range = FULL_RANGE
   loop total times:
     r = range / total
     idx = code / r
     find sym with cum[sym] <= idx < cum[sym+1]
     low = r * cum[sym]
     range = r * freq[sym]
     while(range <= TOP) { code = (code << 8) | next_byte; range <<= 8; }
*/

static int range_decode_block(
    const unsigned char *input, size_t input_size,
    unsigned char **out_buf, size_t *out_size)
{
    if (!out_buf || !out_size)
        return -1;
    *out_buf = NULL;
    *out_size = 0;

    if (input_size < 4 + 256 * 8)
        return -1;
    if (memcmp(input, RAN_MAGIC, 4) != 0)
        return -1;

    // read frequencies
    uint64_t freq[256];
    uint64_t cum[257];
    cum[0] = 0;
    uint64_t read_total = 0;
    for (int i = 0; i < 256; ++i)
    {
        freq[i] = read_u64_le(input + 4 + i * 8);
        read_total += freq[i];
        cum[i + 1] = read_total;
    }

    if (read_total == 0)
    {
        // empty
        *out_buf = malloc(0);
        *out_size = 0;
        return 0;
    }

    const unsigned char *stream = input + 4 + 256 * 8;
    size_t stream_len = input_size - (4 + 256 * 8);
    ByteReader br;
    br_init(&br, stream, stream_len);

    // init code with first 4 bytes (pad with zeros if short)
    uint64_t code = 0;
    for (int i = 0; i < 4; ++i)
    {
        unsigned char b = 0;
        if (br_get_byte(&br, &b) != 0)
            b = 0;
        code = (code << 8) | b;
    }

    uint64_t range = FULL_RANGE;

    // allocate output buffer sized to total count
    if (read_total > SIZE_MAX)
        return -1;
    unsigned char *out = malloc((size_t)read_total);
    if (!out)
        return -1;

    for (uint64_t out_i = 0; out_i < read_total; ++out_i)
    {
        uint64_t r = range / read_total;
        if (r == 0)
        {
            free(out);
            return -1;
        } // bad model

        uint64_t idx = code / r; // in [0, total-1]

        // binary/search for symbol (linear scan is fine for 256)
        int sym = 0;
        // find sym such that cum[sym] <= idx < cum[sym+1]
        int lo = 0, hi = 255;
        // simple linear is OK; binary search faster
        while (lo <= hi)
        {
            int mid = (lo + hi) >> 1;
            if (cum[mid + 1] <= idx)
                lo = mid + 1;
            else
            {
                if (cum[mid] > idx)
                    hi = mid - 1;
                else
                {
                    sym = mid;
                    break;
                }
            }
        }
        // if not set by binary, fall back linear scan (safe)
        if (cum[sym] > idx || cum[sym + 1] <= idx)
        {
            for (sym = 0; sym < 256; ++sym)
            {
                if (cum[sym] <= idx && idx < cum[sym + 1])
                    break;
            }
            if (sym >= 256)
            {
                free(out);
                return -1;
            }
        }

        out[out_i] = (unsigned char)sym;

        // narrow interval
        uint64_t start = r * cum[sym];
        uint64_t size = r * freq[sym];
        range = size;
        code -= start;

        // renormalize: bring range back above TOP by consuming bytes
        while (range <= TOP)
        {
            unsigned char b = 0;
            if (br_get_byte(&br, &b) != 0)
                b = 0;
            code = (code << 8) | b;
            range <<= 8;
        }
    }

    *out_buf = out;
    *out_size = (size_t)read_total;
    return 0;
}

/* -- public compress/decompress functions -- */

static int range_compress(
    const unsigned char *input,
    size_t input_size,
    unsigned char **output,
    size_t *output_size)
{
    if (!output || !output_size)
        return -1;
    *output = NULL;
    *output_size = 0;

    // count frequencies
    uint64_t freq[256];
    memset(freq, 0, sizeof(freq));
    for (size_t i = 0; i < input_size; ++i)
        freq[input[i]]++;

    return range_encode_block(input, input_size, freq, output, output_size);
}

static int range_decompress(
    const unsigned char *input,
    size_t input_size,
    unsigned char **output,
    size_t *output_size)
{
    return range_decode_block(input, input_size, output, output_size);
}

Compressor range_compressor = {
    .name = "range",
    .compress = range_compress,
    .decompress = range_decompress};