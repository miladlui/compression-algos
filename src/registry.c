#include "../include/compressor.h"
#include "../algorithms/rle/rle.h"

Compressor *algorithms[] = {
    &rle_compressor,
    NULL};