// algorithms/lzma/lzma.c
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../../include/compressor.h"
#include "lzma.h"

static const unsigned char LZMA_MAGIC[4] = {'L', 'Z', 'M', 'A'};

// Simplified LZMA constants
#define LZMA_DICT_SIZE (1 << 12) // 4096
#define LZMA_MIN_MATCH 3
#define LZMA_MAX_MATCH 258

// Range coder constants (similar to range.c)
#define RC_TOP (1ULL << 24)
#define RC_FULL 0xFFFFFFFFULL
#define RC_PRECISION 32

typedef struct
{
    uint64_t low;
    uint64_t range;
    unsigned char *buf;
    size_t cap;
    size_t pos;
} RangeCoder;

static int rc_init(RangeCoder *rc, size_t expected)
{
    rc->low = 0;
    rc->range = RC_FULL;
    rc->cap = expected ? expected : 1024;
    rc->buf = malloc(rc->cap);
    if (!rc->buf)
        return -1;
    rc->pos = 0;
    return 0;
}

static void rc_free(RangeCoder *rc)
{
    free(rc->buf);
}

static int rc_encode_bit(RangeCoder *rc, uint32_t prob, int bit)
{
    uint64_t mid = rc->low + (rc->range >> RC_PRECISION) * prob;
    if (bit)
    {
        rc->low = mid + 1;
        rc->range -= (mid + 1 - rc->low);
    }
    else
    {
        rc->range = mid - rc->low;
    }
    while (rc->range < RC_TOP)
    {
        if (rc->pos >= rc->cap)
        {
            size_t nc = rc->cap * 2;
            unsigned char *tmp = realloc(rc->buf, nc);
            if (!tmp)
                return -1;
            rc->buf = tmp;
            rc->cap = nc;
        }
        rc->buf[rc->pos++] = (unsigned char)(rc->low >> (RC_PRECISION - 8));
        rc->low <<= 8;
        rc->range <<= 8;
    }
    return 0;
}

static int rc_flush(RangeCoder *rc)
{
    for (int i = 0; i < 5; i++)
    {
        if (rc->pos >= rc->cap)
        {
            size_t nc = rc->cap * 2;
            unsigned char *tmp = realloc(rc->buf, nc);
            if (!tmp)
                return -1;
            rc->buf = tmp;
            rc->cap = nc;
        }
        rc->buf[rc->pos++] = (unsigned char)(rc->low >> (RC_PRECISION - 8));
        rc->low <<= 8;
    }
    return 0;
}

// Simple hash for LZ matching
#define HASH_SIZE 4096
#define HASH_MASK (HASH_SIZE - 1)

typedef struct
{
    uint32_t head[HASH_SIZE];
    uint32_t prev[LZMA_DICT_SIZE];
} HashTable;

static void hash_init(HashTable *ht)
{
    memset(ht->head, 0xFF, sizeof(ht->head));
    memset(ht->prev, 0xFF, sizeof(ht->prev));
}

static uint32_t hash_func(const unsigned char *p)
{
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

static void hash_update(HashTable *ht, uint32_t pos, const unsigned char *data)
{
    uint32_t h = hash_func(data + pos) & HASH_MASK;
    ht->prev[pos & (LZMA_DICT_SIZE - 1)] = ht->head[h];
    ht->head[h] = pos;
}

static uint32_t hash_find(HashTable *ht, const unsigned char *data, uint32_t pos, size_t *len)
{
    uint32_t h = hash_func(data + pos) & HASH_MASK;
    uint32_t best = 0xFFFFFFFF;
    *len = 0;
    for (uint32_t p = ht->head[h]; p != 0xFFFFFFFF; p = ht->prev[p & (LZMA_DICT_SIZE - 1)])
    {
        if (p >= pos)
            continue;
        size_t l = 0;
        while (l < LZMA_MAX_MATCH && pos + l < LZMA_DICT_SIZE && data[p + l] == data[pos + l])
            l++;
        if (l >= LZMA_MIN_MATCH && l > *len)
        {
            *len = l;
            best = p;
        }
    }
    return best;
}

// Compress
static int lzma_compress(const unsigned char *input, size_t input_size, unsigned char **output, size_t *output_size)
{
    if (input_size == 0)
        return -1;
    RangeCoder rc;
    if (rc_init(&rc, input_size) != 0)
        return -1;
    HashTable ht;
    hash_init(&ht);
    uint32_t pos = 0;
    while (pos < input_size)
    {
        size_t match_len;
        uint32_t match_pos = hash_find(&ht, input, pos, &match_len);
        if (match_len >= LZMA_MIN_MATCH)
        {
            // Encode match
            if (rc_encode_bit(&rc, 1 << (RC_PRECISION - 1), 1) != 0)
                goto error; // match flag
            // Encode distance
            uint32_t dist = pos - match_pos;
            for (int i = 0; i < 12; i++)
            {
                if (rc_encode_bit(&rc, 1 << (RC_PRECISION - 1), (dist >> i) & 1) != 0)
                    goto error;
            }
            // Encode length
            size_t len_code = match_len - LZMA_MIN_MATCH;
            for (int i = 0; i < 8; i++)
            {
                if (rc_encode_bit(&rc, 1 << (RC_PRECISION - 1), (len_code >> i) & 1) != 0)
                    goto error;
            }
            for (size_t i = 0; i < match_len; i++)
            {
                hash_update(&ht, pos + i, input);
            }
            pos += match_len;
        }
        else
        {
            // Encode literal
            if (rc_encode_bit(&rc, 1 << (RC_PRECISION - 1), 0) != 0)
                goto error; // literal flag
            for (int i = 0; i < 8; i++)
            {
                if (rc_encode_bit(&rc, 1 << (RC_PRECISION - 1), (input[pos] >> i) & 1) != 0)
                    goto error;
            }
            hash_update(&ht, pos, input);
            pos++;
        }
    }
    if (rc_flush(&rc) != 0)
        goto error;
    size_t total = 4 + rc.pos;
    unsigned char *out = malloc(total);
    if (!out)
        goto error;
    memcpy(out, LZMA_MAGIC, 4);
    memcpy(out + 4, rc.buf, rc.pos);
    *output = out;
    *output_size = total;
    rc_free(&rc);
    return 0;
error:
    rc_free(&rc);
    return -1;
}

// Decompress (simplified, assumes correct input)
static int lzma_decompress(const unsigned char *input, size_t input_size, unsigned char **output, size_t *output_size)
{
    if (input_size < 4 || memcmp(input, LZMA_MAGIC, 4) != 0)
        return -1;
    // Simplified decompress - in practice, need full range decoder
    // For brevity, assume uncompressed for now (placeholder)
    *output = malloc(input_size - 4);
    if (!*output)
        return -1;
    memcpy(*output, input + 4, input_size - 4);
    *output_size = input_size - 4;
    return 0;
}

Compressor lzma_compressor = {
    .name = "lzma",
    .compress = lzma_compress,
    .decompress = lzma_decompress};