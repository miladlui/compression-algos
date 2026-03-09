#ifndef COMPRESSOR_H
#define COMPRESSOR_H

#include <stddef.h>

typedef struct
{
    const char *name;
    int (*compress)(
        const unsigned char *input,
        size_t input_size,
        unsigned char **output,
        size_t *output_size);
    int (*decompress)(
        const unsigned char *input,
        size_t input_size,
        unsigned char **output,
        size_t *output_size);
} Compressor;

#endif