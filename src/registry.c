#include "../include/compressor.h"
#include "../algorithms/rle/rle.h"
#include "../algorithms/huffman/huffman.h"
#include "../algorithms/shannon/shannon.h"
#include "../algorithms/canonical_huffman/canonical_huffman.h"
#include "../algorithms/adaptive_huffman/adaptive_huffman.h"

Compressor *algorithms[] = {
    &rle_compressor,
    &huffman_compressor,
    &shannon_compressor,
    &canonical_huffman_compressor,
    &adaptive_huffman_compressor,
    NULL};