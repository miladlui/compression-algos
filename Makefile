CC=gcc
CFLAGS=-Wall -O2
SRC=src/main.c src/registry.c algorithms/rle/rle.c algorithms/huffman/huffman.c algorithms/shannon/shannon.c algorithms/canonical_huffman/canonical_huffman.c
all:
	$(CC) $(CFLAGS) $(SRC) -o compressor