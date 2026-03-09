#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/compressor.h"

extern Compressor *algorithms[];

int main()
{
    const char *text = "AAAAABBBBBCCCCDDDDDD";
    size_t input_size = strlen(text);
    unsigned char *compressed;
    size_t compressed_size;
    algorithms[0]->compress(
        (unsigned char *)text,
        input_size,
        &compressed,
        &compressed_size);
    printf("original size:\t\t%zu\n", input_size);
    printf("compressed size:\t%zu\n", compressed_size);
    printf("compressed:\t\t");
    for (size_t i = 0; i < compressed_size; i += 2)
    {
        printf("(%d,'%c') ",
               compressed[i],
               compressed[i + 1]);
    }
    printf("\n");
    unsigned char *decompressed;
    size_t decompressed_size;
    algorithms[0]->decompress(
        compressed,
        compressed_size,
        &decompressed,
        &decompressed_size);
    printf("decompressed:\t\t%.*s\n", (int)decompressed_size, decompressed);
    free(compressed);
    free(decompressed);
    return 0;
}