#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

#include "../include/compressor.h"
extern Compressor *algorithms[];

/* -- helpers -- */
static double diff_ms(const struct timespec *a, const struct timespec *b)
{
    double s = (double)(b->tv_sec - a->tv_sec);
    double ns = (double)(b->tv_nsec - a->tv_nsec);
    return s * 1000.0 + ns / 1e6;
}

// print a short hex sample of the compressed buffer (up to n bytes)
static void print_hex_sample(const unsigned char *buf, size_t len, size_t n)
{
    size_t upto = len < n ? len : n;
    for (size_t i = 0; i < upto; ++i)
    {
        printf("%02x", buf[i]);
        if (i + 1 < upto)
            putchar(' ');
    }
    if (len > upto)
        printf(" ...");
}

// create deterministic pseudo-random data (caller must free)
static unsigned char *make_random(size_t len)
{
    unsigned char *b = malloc(len);
    if (!b)
        return NULL;
    // deterministic seed so results are reproducible
    unsigned int seed = 123456789u;
    for (size_t i = 0; i < len; ++i)
    {
        // simple xorshift-ish
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        b[i] = (unsigned char)(seed & 0xFF);
    }
    return b;
}

// human-friendly size
static void fmt_size(char *out, size_t n)
{
    if (n >= 1 << 30)
        sprintf(out, "%.2fG", n / (double)(1 << 30));
    else if (n >= 1 << 20)
        sprintf(out, "%.2fM", n / (double)(1 << 20));
    else if (n >= 1 << 10)
        sprintf(out, "%.2fk", n / (double)(1 << 10));
    else
        sprintf(out, "%zub", n);
}

/* -- test harness -- */

int main(void)
{
    // test cases
    const char *t1 = "AAAAABBBBBCCCCDDDDDD"; // very compressible for RLE
    const char *t2 = "This is an example sentence to exercise Huffman coding. "
                     "Huffman is good for natural-language text.";
    size_t rnd_len = 4096;
    unsigned char *rnd = make_random(rnd_len);
    if (!rnd)
    {
        fprintf(stderr, "failed to allocate random buffer\n");
        return 2;
    }
    struct TestCase
    {
        const char *name;
        const unsigned char *data;
        size_t len;
        int should_free; // if we should free data after use
    } tests[] = {
        {"repetitive", (const unsigned char *)t1, strlen(t1), 0},
        {"english", (const unsigned char *)t2, strlen(t2), 0},
        {"random", rnd, rnd_len, 1},
    };
    const size_t test_count = sizeof(tests) / sizeof(tests[0]);
    // table header
    puts("-------------------------------------------------------------------------------");
    printf("%-12s | %-10s | %-10s | %-6s | %-9s | %-4s | %s\n",
           "algorithm", "test", "orig", "comped", "ratio", "ms", "ok");
    puts("-------------------------------------------------------------------------------");
    // iterate algorithms[]
    for (size_t a = 0; algorithms[a] != NULL; ++a)
    {
        Compressor *c = algorithms[a];
        if (!c || !c->compress || !c->decompress)
            continue;

        for (size_t ti = 0; ti < test_count; ++ti)
        {
            const unsigned char *in = tests[ti].data;
            size_t in_sz = tests[ti].len;
            unsigned char *comp = NULL;
            size_t comp_sz = 0;
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            int rc = c->compress(in, in_sz, &comp, &comp_sz);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            double ms = diff_ms(&t0, &t1);
            int ok = 0;
            if (rc == 0 && comp != NULL)
            {
                unsigned char *dec = NULL;
                size_t dec_sz = 0;
                int rc2 = c->decompress(comp, comp_sz, &dec, &dec_sz);
                if (rc2 == 0 && dec != NULL && dec_sz == in_sz && memcmp(dec, in, in_sz) == 0)
                    ok = 1;
                free(dec);
            }
            char orig_s[32], comp_s[32];
            fmt_size(orig_s, in_sz);
            fmt_size(comp_s, comp_sz);
            double ratio = (comp_sz == 0) ? 0.0 : (double)in_sz / (double)comp_sz;
            printf("%-12s | %-10s | %-10s | %-6s | %6.2f | %6.2f | %s\n",
                   c->name,
                   tests[ti].name,
                   orig_s,
                   comp_s,
                   ratio,
                   ms,
                   ok ? "yes" : "no");
            // show small compressed sample for quick eyeballing (only for first test of each algorithm)
            if (ti == 0 && comp && comp_sz > 0)
            {
                printf("    -> sample: ");
                print_hex_sample(comp, comp_sz, 16);
                printf("\n");
            }
            free(comp);
        }
        puts("-------------------------------------------------------------------------------");
    }

    // cleanup
    for (size_t i = 0; i < test_count; ++i)
        if (tests[i].should_free)
            free((void *)tests[i].data);

    return 0;
}