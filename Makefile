CC=gcc
CFLAGS=-Wall -O2
SRC=src/main.c src/registry.c algorithms/rle/rle.c algorithms/huffman/huffman.c algorithms/shannon/shannon.c algorithms/canonical_huffman/canonical_huffman.c algorithms/adaptive_huffman/adaptive_huffman.c algorithms/arithmetic/arithmetic.c algorithms/range/range.c algorithms/rans/rans.c algorithms/elias_gamma/elias_gamma.c algorithms/elias_delta/elias_delta.c algorithms/fibonacci/fibonacci.c algorithms/lz77/lz77.c
all:
	$(CC) $(CFLAGS) $(SRC) -o compressor