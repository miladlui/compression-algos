#include "../include/compressor.h"
#include "../algorithms/rle/rle.h"
#include "../algorithms/huffman/huffman.h"
#include "../algorithms/shannon/shannon.h"

Compressor *algorithms[] = {
    &rle_compressor,
    &huffman_compressor,
    &shannon_compressor,
    NULL};