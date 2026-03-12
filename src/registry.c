#include "../include/compressor.h"
#include "../algorithms/rle/rle.h"
#include "../algorithms/huffman/huffman.h"
#include "../algorithms/shannon/shannon.h"
#include "../algorithms/canonical_huffman/canonical_huffman.h"
#include "../algorithms/adaptive_huffman/adaptive_huffman.h"
#include "../algorithms/arithmetic/arithmetic.h"
#include "../algorithms/range/range.h"
#include "../algorithms/rans/rans.h"
#include "../algorithms/elias_gamma/elias_gamma.h"

Compressor *algorithms[] = {
    &rle_compressor,
    &huffman_compressor,
    &shannon_compressor,
    &canonical_huffman_compressor,
    &adaptive_huffman_compressor,
    &arithmetic_compressor,
    &range_compressor,
    &rans_compressor,
    &elias_gamma_compressor,
    NULL};