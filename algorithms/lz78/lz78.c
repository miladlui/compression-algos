#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../../include/compressor.h"
#include "lz78.h"

static const unsigned char LZ78_MAGIC[4] = {'L', 'Z', '7', '8'};

/* --- Byte buffer helpers --- */

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

static void write_u32_le(unsigned char *buf, uint32_t v)
{
    buf[0] = (unsigned char)(v & 0xFF);
    buf[1] = (unsigned char)((v >> 8) & 0xFF);
    buf[2] = (unsigned char)((v >> 16) & 0xFF);
    buf[3] = (unsigned char)((v >> 24) & 0xFF);
}

static uint32_t read_u32_le(const unsigned char *buf)
{
    return (uint32_t)buf[0] |
           ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) |
           ((uint32_t)buf[3] << 24);
}

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

/* --- Dictionary --- */

typedef struct
{
    uint32_t prefix;
    uint16_t symbol; // 0 = sentinel (no symbol), otherwise 1..256
} DictEntry;

typedef struct
{
    DictEntry *entries;
    size_t len;
    size_t cap;
} Dictionary;

static int dict_init(Dictionary *d, size_t expected)
{
    d->cap = expected ? expected : 1024;
    d->entries = malloc(d->cap * sizeof(DictEntry));
    if (!d->entries)
        return -1;
    d->len = 0;
    return 0;
}

static void dict_free(Dictionary *d)
{
    free(d->entries);
}

static int dict_push(Dictionary *d, uint32_t prefix, uint16_t symbol)
{
    if (d->len + 1 > d->cap)
    {
        size_t nc = d->cap * 2 + 1;
        DictEntry *tmp = realloc(d->entries, nc * sizeof(DictEntry));
        if (!tmp)
            return -1;
        d->entries = tmp;
        d->cap = nc;
    }
    d->entries[d->len].prefix = prefix;
    d->entries[d->len].symbol = symbol;
    d->len++;
    return 0;
}

/* Find entry index where (prefix, symbol) matches. Returns 0..len-1 or -1 if missing. */
static int32_t dict_find(const Dictionary *d, uint32_t prefix, uint16_t symbol)
{
    for (size_t i = 0; i < d->len; ++i)
    {
        if (d->entries[i].prefix == prefix && d->entries[i].symbol == symbol)
            return (int32_t)i + 1; // store 1-based so 0 remains "no match"
    }
    return -1;
}

/* Reconstruct phrase for a given dictionary index (1-based). */
static int dict_reconstruct(const Dictionary *d, uint32_t idx, ByteBuf *out)
{
    if (idx == 0)
        return 0;

    // Collect bytes in reverse order:
    unsigned char stack[1024];
    size_t stack_len = 0;

    while (idx != 0)
    {
        if (idx > d->len)
            return -1;
        const DictEntry *e = &d->entries[idx - 1];
        if (stack_len >= sizeof(stack))
            return -1;
        stack[stack_len++] = (unsigned char)(e->symbol - 1); // restore original byte
        idx = e->prefix;
    }

    // write in correct order
    for (size_t i = 0; i < stack_len; ++i)
    {
        if (bb_push(out, stack[stack_len - 1 - i]) != 0)
            return -1;
    }
    return 0;
}

/* --- LZ78 compress / decompress --- */

static int lz78_compress(
    const unsigned char *input,
    size_t input_size,
    unsigned char **output,
    size_t *output_size)
{
    Dictionary dict;
    if (dict_init(&dict, 1024) != 0)
        return -1;

    ByteBuf out;
    if (bb_init(&out, input_size + 16) != 0)
    {
        dict_free(&dict);
        return -1;
    }

    // Header: magic + original size
    if (bb_push_bytes(&out, LZ78_MAGIC, 4) != 0)
        goto err;
    unsigned char size_buf[8];
    write_u64_le(size_buf, (uint64_t)input_size);
    if (bb_push_bytes(&out, size_buf, 8) != 0)
        goto err;

    uint32_t current = 0; // 0 means empty
    for (size_t i = 0; i < input_size; ++i)
    {
        uint16_t sym = (uint16_t)input[i] + 1; // map 0..255 -> 1..256
        int32_t found = dict_find(&dict, current, sym);
        if (found > 0)
        {
            current = (uint32_t)found;
            continue;
        }

        // emit (current, sym)
        unsigned char token[6];
        write_u32_le(token, current);
        write_u16_le(token + 4, sym);
        if (bb_push_bytes(&out, token, sizeof(token)) != 0)
            goto err;

        if (dict_push(&dict, current, sym) != 0)
            goto err;

        current = 0;
    }

    // If input ended in the middle of a phrase, flush it (symbol=0).
    if (current != 0)
    {
        unsigned char token[6];
        write_u32_le(token, current);
        write_u16_le(token + 4, 0);
        if (bb_push_bytes(&out, token, sizeof(token)) != 0)
            goto err;
    }

    *output = out.buf;
    *output_size = out.len;
    dict_free(&dict);
    return 0;

err:
    bb_free(&out);
    dict_free(&dict);
    return -1;
}

static int lz78_decompress(
    const unsigned char *input,
    size_t input_size,
    unsigned char **output,
    size_t *output_size)
{
    if (input_size < 12 || memcmp(input, LZ78_MAGIC, 4) != 0)
        return -1;

    uint64_t orig_size = read_u64_le(input + 4);
    const unsigned char *ptr = input + 12;
    size_t remaining = input_size - 12;

    Dictionary dict;
    if (dict_init(&dict, 1024) != 0)
        return -1;

    ByteBuf out;
    if (bb_init(&out, (size_t)orig_size) != 0)
    {
        dict_free(&dict);
        return -1;
    }

    while (remaining >= 6 && out.len < orig_size)
    {
        uint32_t idx = read_u32_le(ptr);
        uint16_t sym = read_u16_le(ptr + 4);
        ptr += 6;
        remaining -= 6;

        // output phrase from dict
        if (dict_reconstruct(&dict, idx, &out) != 0)
            goto err;

        // if sym != 0, append symbol and add new dict entry
        if (sym != 0)
        {
            if (bb_push(&out, (unsigned char)(sym - 1)) != 0)
                goto err;

            if (dict_push(&dict, idx, sym) != 0)
                goto err;
        }
    }

    // truncate to original size if needed
    if (out.len > orig_size)
        out.len = (size_t)orig_size;

    *output = out.buf;
    *output_size = out.len;
    dict_free(&dict);
    return 0;

err:
    bb_free(&out);
    dict_free(&dict);
    return -1;
}

Compressor lz78_compressor = {
    .name = "lz78",
    .compress = lz78_compress,
    .decompress = lz78_decompress};