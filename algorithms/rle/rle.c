#include <stdlib.h>
#include "rle.h"

/**
 * @brief Compress data using Run-Length Encoding (RLE)
 *
 * RLE replaces consecutive identical bytes with a [count, value] pair.
 *
 * Example:
 *   Input:  "AAAABBBCC" (9 bytes)
 *   Output: [4, 'A', 3, 'B', 2, 'C'] (6 bytes)
 *
 * @note Maximum run length is 255 due to unsigned char limitation
 * @note Worst case (no repetition) doubles the size:
 *       "ABC" → [1,'A',1,'B',1,'C']
 *
 * @param input Pointer to input data to compress
 * @param input_size Size of input data in bytes
 * @param output Pointer to output buffer pointer (will be allocated)
 * @param output_size Pointer to store compressed size
 * @return 0 on success, -1 on error (empty input or allocation failure)
 */
static int rle_compress(
    const unsigned char *input,
    size_t input_size,
    unsigned char **output,
    size_t *output_size)
{
    if (input_size == 0)
        return -1;
    // wcs 2*size
    *output = malloc(input_size * 2);
    if (!*output)
        return -1;
    size_t out_index = 0;
    for (size_t i = 0; i < input_size;)
    {
        unsigned char value = input[i];
        unsigned char count = 1;
        while (i + count < input_size && // buffer overflow
               input[i + count] == value &&
               count < (unsigned char)-1) // 255
        {
            count++;
        }
        (*output)[out_index++] = count;
        (*output)[out_index++] = value;
        i += count;
    }
    *output_size = out_index;
    return 0;
}

/**
 * @brief Decompress RLE-compressed data back to original form
 *
 * This function takes RLE-compressed data consisting of [count, value]
 * pairs and expands it to the original uncompressed data.
 *
 * Algorithm:
 *   1. First pass: Sum all count bytes to estimate output size
 *   2. Allocate output buffer based on estimated size
 *   3. Second pass: For each [count, value] pair:
 *      - Write 'value' to output 'count' times
 *
 * Example:
 *   Input:  [4, 'A', 3, 'B', 2, 'C'] (6 bytes)
 *   Output: "AAAABBBCC" (9 bytes)
 *
 * @note The input must consist of valid [count, value] pairs
 * @note Each count byte must be between 1-255
 * @note Input size must be even (complete pairs only)
 *
 * @param input Pointer to compressed input data
 * @param input_size Size of compressed data in bytes
 * @param output Pointer to output buffer pointer (will be allocated)
 * @param output_size Pointer to store decompressed size
 * @return 0 on success, -1 on allocation failure
 */
static int rle_decompress(
    const unsigned char *input,
    size_t input_size,
    unsigned char **output,
    size_t *output_size)
{
    size_t estimated = 0;
    for (size_t i = 0; i < input_size; i += 2)
        estimated += input[i];
    *output = malloc(estimated);
    if (!*output)
        return -1;
    size_t out_index = 0;
    for (size_t i = 0; i < input_size; i += 2)
    {
        unsigned char count = input[i];
        unsigned char value = input[i + 1];
        for (int j = 0; j < count; j++)
            (*output)[out_index++] = value;
    }
    *output_size = out_index;
    return 0;
}

Compressor rle_compressor = {
    .name = "rle",
    .compress = rle_compress,
    .decompress = rle_decompress};