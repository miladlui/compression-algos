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
#include "../algorithms/elias_delta/elias_delta.h"
#include "../algorithms/fibonacci/fibonacci.h"
#include "../algorithms/lz77/lz77.h"
#include "../algorithms/lz78/lz78.h"

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
    &elias_delta_compressor,
    &fibonacci_compressor,
    &lz77_compressor,
    &lz78_compressor,
    NULL};