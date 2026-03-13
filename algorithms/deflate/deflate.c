// algorithms/deflate/deflate.c
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../../include/compressor.h"
#include "deflate.h"

static const unsigned char DEFLATE_MAGIC[4] = {'D', 'E', 'F', 'L'};

#define WINDOW_SIZE 32768
#define MIN_MATCH 3
#define MAX_MATCH 258
#define HASH_SIZE 4096

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

static int bb_push_bytes(ByteBuf *b, const unsigned char *data, size_t len)
{
    if (b->len + len > b->cap)
    {
        size_t nc = b->cap * 2 + len;
        unsigned char *tmp = realloc(b->buf, nc);
        if (!tmp)
            return -1;
        b->buf = tmp;
        b->cap = nc;
    }
    memcpy(b->buf + b->len, data, len);
    b->len += len;
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
    b->buf = calloc(b->cap, 1);
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

static int bitb_write_bits(BitBuf *b, uint32_t val, int nbits)
{
    for (int i = 0; i < nbits; i++)
    {
        int bit = (val >> i) & 1;
        if (bitb_write_bit(b, bit) != 0)
            return -1;
    }
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

static uint32_t bitr_read_bits(BitReader *r, int nbits)
{
    uint32_t val = 0;
    for (int i = 0; i < nbits; i++)
    {
        int bit = bitr_read_bit(r);
        if (bit == -1)
            return (uint32_t)-1;
        val |= (bit << i);
    }
    return val;
}

/* -- huffman -- */

typedef struct
{
    uint16_t code;
    uint8_t len;
} HuffCode;

typedef struct
{
    HuffCode codes[288];     // literals/lengths
    HuffCode dist_codes[32]; // distances
} HuffTables;

static void build_huffman(const uint32_t *freq, int n, HuffCode *codes)
{
    // Simple Huffman: sort by freq, assign codes
    typedef struct
    {
        int sym;
        uint32_t freq;
    } SymFreq;
    SymFreq sf[288];
    for (int i = 0; i < n; i++)
    {
        sf[i].sym = i;
        sf[i].freq = freq[i];
    }
    // Sort descending freq
    for (int i = 0; i < n - 1; i++)
        for (int j = i + 1; j < n; j++)
            if (sf[i].freq < sf[j].freq)
            {
                SymFreq tmp = sf[i];
                sf[i] = sf[j];
                sf[j] = tmp;
            }
    // Assign codes
    uint16_t code = 0;
    uint8_t len = 1;
    int count = 0;
    for (int i = 0; i < n; i++)
    {
        if (sf[i].freq == 0)
            continue;
        codes[sf[i].sym].code = code;
        codes[sf[i].sym].len = len;
        code++;
        count++;
        if (count == (1 << (len - 1)))
        {
            len++;
            code = 0;
        }
    }
}

static int write_huffman_table(BitBuf *b, const HuffCode *codes, int n)
{
    for (int i = 0; i < n; i++)
    {
        if (bitb_write_bits(b, codes[i].len, 4) != 0)
            return -1;
        if (codes[i].len > 0 && bitb_write_bits(b, codes[i].code, codes[i].len) != 0)
            return -1;
    }
    return 0;
}

static int read_huffman_table(BitReader *r, HuffCode *codes, int n)
{
    for (int i = 0; i < n; i++)
    {
        uint32_t len = bitr_read_bits(r, 4);
        if (len == (uint32_t)-1)
            return -1;
        codes[i].len = len;
        if (len > 0)
        {
            uint32_t code = bitr_read_bits(r, len);
            if (code == (uint32_t)-1)
                return -1;
            codes[i].code = code;
        }
    }
    return 0;
}

/* -- LZ77 -- */

typedef struct
{
    int is_literal;
    union
    {
        unsigned char literal;
        struct
        {
            uint16_t len;
            uint16_t dist;
        };
    };
} Token;

static int find_match(const unsigned char *data, size_t pos, size_t size, uint16_t *len, uint16_t *dist)
{
    *len = 0;
    *dist = 0;
    if (pos < 1)
        return 0;
    size_t start = pos > WINDOW_SIZE ? pos - WINDOW_SIZE : 0;
    for (size_t i = start; i < pos; i++)
    {
        size_t j = 0;
        while (pos + j < size && data[pos + j] == data[i + j] && j < MAX_MATCH)
            j++;
        if (j >= MIN_MATCH && j > *len)
        {
            *len = j;
            *dist = pos - i;
        }
    }
    return *len >= MIN_MATCH;
}

static Token *lz77_compress(const unsigned char *input, size_t input_size, size_t *num_tokens)
{
    Token *tokens = malloc(sizeof(Token) * input_size);
    if (!tokens)
        return NULL;
    size_t pos = 0;
    size_t count = 0;
    while (pos < input_size)
    {
        uint16_t len, dist;
        if (find_match(input, pos, input_size, &len, &dist))
        {
            tokens[count].is_literal = 0;
            tokens[count].len = len;
            tokens[count].dist = dist;
            pos += len;
        }
        else
        {
            tokens[count].is_literal = 1;
            tokens[count].literal = input[pos];
            pos++;
        }
        count++;
    }
    *num_tokens = count;
    return tokens;
}

/* -- compress -- */

static int deflate_compress(
    const unsigned char *input,
    size_t input_size,
    unsigned char **output,
    size_t *output_size)
{
    // LZ77
    size_t num_tokens;
    Token *tokens = lz77_compress(input, input_size, &num_tokens);
    if (!tokens)
        return -1;

    // Frequencies
    uint32_t lit_freq[288] = {0};
    uint32_t dist_freq[32] = {0};
    for (size_t i = 0; i < num_tokens; i++)
    {
        if (tokens[i].is_literal)
        {
            lit_freq[tokens[i].literal]++;
        }
        else
        {
            // Length codes: simple mapping
            int len_code = tokens[i].len - 3;
            if (len_code < 0)
                len_code = 0;
            if (len_code > 255)
                len_code = 255;
            lit_freq[257 + len_code]++;
            // Distance codes
            int dist_code = 0;
            uint16_t d = tokens[i].dist;
            while (d > 1)
            {
                d >>= 1;
                dist_code++;
            }
            if (dist_code > 29)
                dist_code = 29;
            dist_freq[dist_code]++;
        }
    }

    // Build Huffman
    HuffTables tables;
    build_huffman(lit_freq, 288, tables.codes);
    build_huffman(dist_freq, 32, tables.dist_codes);

    // Output
    BitBuf b;
    if (bitb_init(&b, input_size * 8) != 0)
    {
        free(tokens);
        return -1;
    }

    // Write magic
    for (int i = 0; i < 4; i++)
        if (bb_push(NULL, DEFLATE_MAGIC[i]) != 0) // wait, need ByteBuf for header
            goto error;

    // Actually, since magic is bytes, use ByteBuf for header, then BitBuf for compressed
    ByteBuf header;
    if (bb_init(&header, 4 + 288 * 5 + 32 * 5) != 0)
        goto error;
    bb_push_bytes(&header, DEFLATE_MAGIC, 4);

    // Write Huffman tables as bytes for simplicity
    for (int i = 0; i < 288; i++)
    {
        bb_push(&header, tables.codes[i].len);
        if (tables.codes[i].len > 0)
        {
            bb_push(&header, tables.codes[i].code & 0xFF);
            bb_push(&header, (tables.codes[i].code >> 8) & 0xFF);
        }
    }
    for (int i = 0; i < 32; i++)
    {
        bb_push(&header, tables.dist_codes[i].len);
        if (tables.dist_codes[i].len > 0)
        {
            bb_push(&header, tables.dist_codes[i].code & 0xFF);
            bb_push(&header, (tables.dist_codes[i].code >> 8) & 0xFF);
        }
    }

    // Now bit buffer for data
    for (size_t i = 0; i < num_tokens; i++)
    {
        if (tokens[i].is_literal)
        {
            HuffCode c = tables.codes[tokens[i].literal];
            if (bitb_write_bits(&b, c.code, c.len) != 0)
                goto error;
        }
        else
        {
            int len_code = tokens[i].len - 3;
            if (len_code < 0)
                len_code = 0;
            if (len_code > 255)
                len_code = 255;
            HuffCode c = tables.codes[257 + len_code];
            if (bitb_write_bits(&b, c.code, c.len) != 0)
                goto error;
            // Extra bits for length
            int extra_len = 0;
            if (tokens[i].len >= 11)
                extra_len = (tokens[i].len - 11) / 4 + 1;
            if (extra_len > 5)
                extra_len = 5;
            if (bitb_write_bits(&b, tokens[i].len & ((1 << extra_len) - 1), extra_len) != 0)
                goto error;
            // Distance
            int dist_code = 0;
            uint16_t d = tokens[i].dist;
            while (d > 1)
            {
                d >>= 1;
                dist_code++;
            }
            if (dist_code > 29)
                dist_code = 29;
            HuffCode dc = tables.dist_codes[dist_code];
            if (bitb_write_bits(&b, dc.code, dc.len) != 0)
                goto error;
            // Extra bits for distance
            int extra_dist = dist_code / 2;
            if (extra_dist > 13)
                extra_dist = 13;
            if (bitb_write_bits(&b, tokens[i].dist & ((1 << extra_dist) - 1), extra_dist) != 0)
                goto error;
        }
    }

    // Combine
    size_t total = header.len + (b.len + 7) / 8;
    unsigned char *out = malloc(total);
    if (!out)
        goto error;
    memcpy(out, header.buf, header.len);
    memcpy(out + header.len, b.buf, (b.len + 7) / 8);
    *output = out;
    *output_size = total;

    bb_free(&header);
    bitb_free(&b);
    free(tokens);
    return 0;

error:
    bb_free(&header);
    bitb_free(&b);
    free(tokens);
    return -1;
}

/* -- decompress -- */

static int deflate_decompress(
    const unsigned char *input,
    size_t input_size,
    unsigned char **output,
    size_t *output_size)
{
    if (input_size < 4 || memcmp(input, DEFLATE_MAGIC, 4) != 0)
        return -1;

    // Read Huffman tables
    HuffTables tables;
    size_t pos = 4;
    for (int i = 0; i < 288; i++)
    {
        if (pos + 1 > input_size)
            return -1;
        tables.codes[i].len = input[pos++];
        if (tables.codes[i].len > 0)
        {
            if (pos + 2 > input_size)
                return -1;
            tables.codes[i].code = input[pos] | (input[pos + 1] << 8);
            pos += 2;
        }
    }
    for (int i = 0; i < 32; i++)
    {
        if (pos + 1 > input_size)
            return -1;
        tables.dist_codes[i].len = input[pos++];
        if (tables.dist_codes[i].len > 0)
        {
            if (pos + 2 > input_size)
                return -1;
            tables.dist_codes[i].code = input[pos] | (input[pos + 1] << 8);
            pos += 2;
        }
    }

    // Bit reader for compressed data
    BitReader r;
    bitr_init(&r, input + pos, input_size - pos);

    ByteBuf out;
    if (bb_init(&out, input_size) != 0)
        return -1;

    unsigned char window[WINDOW_SIZE];
    size_t window_pos = 0;

    // Decode tokens
    while (r.pos < (input_size - pos) * 8)
    {
        // Find literal/length code
        int found = -1;
        for (int i = 0; i < 288; i++)
        {
            if (tables.codes[i].len == 0)
                continue;
            uint32_t code = bitr_read_bits(&r, tables.codes[i].len);
            if (code == (uint32_t)-1 || code != tables.codes[i].code)
                continue;
            found = i;
            break;
        }
        if (found == -1)
            break;
        if (found < 256)
        {
            // Literal
            unsigned char byte = found;
            if (bb_push(&out, byte) != 0)
            {
                bb_free(&out);
                return -1;
            }
            window[window_pos] = byte;
            window_pos = (window_pos + 1) % WINDOW_SIZE;
        }
        else
        {
            // Length
            int len_code = found - 257;
            int len = 3 + len_code;
            int extra_len = 0;
            if (len >= 11)
                extra_len = (len - 11) / 4 + 1;
            if (extra_len > 5)
                extra_len = 5;
            uint32_t extra = bitr_read_bits(&r, extra_len);
            if (extra == (uint32_t)-1)
                break;
            len += extra;

            // Distance
            int dist_found = -1;
            for (int i = 0; i < 32; i++)
            {
                if (tables.dist_codes[i].len == 0)
                    continue;
                uint32_t code = bitr_read_bits(&r, tables.dist_codes[i].len);
                if (code == (uint32_t)-1 || code != tables.dist_codes[i].code)
                    continue;
                dist_found = i;
                break;
            }
            if (dist_found == -1)
                break;
            int dist = 1 << dist_found;
            int extra_dist = dist_found / 2;
            if (extra_dist > 13)
                extra_dist = 13;
            uint32_t extra_d = bitr_read_bits(&r, extra_dist);
            if (extra_d == (uint32_t)-1)
                break;
            dist += extra_d;

            // Copy from window
            for (int i = 0; i < len; i++)
            {
                size_t idx = (window_pos - dist + WINDOW_SIZE + i) % WINDOW_SIZE;
                unsigned char c = window[idx];
                if (bb_push(&out, c) != 0)
                {
                    bb_free(&out);
                    return -1;
                }
                window[window_pos] = c;
                window_pos = (window_pos + 1) % WINDOW_SIZE;
            }
        }
    }

    *output = out.buf;
    *output_size = out.len;
    return 0;
}

Compressor deflate_compressor = {
    .name = "deflate",
    .compress = deflate_compress,
    .decompress = deflate_decompress};