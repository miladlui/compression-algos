#include "../include/compressor.h"
#include "../algorithms/rle/rle.h"
#include "../algorithms/huffman/huffman.h"

Compressor *algorithms[] = {
    &rle_compressor,
    &huffman_compressor,
    NULL};