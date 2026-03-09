CC=gcc
CFLAGS=-Wall -O2
SRC=src/main.c src/registry.c algorithms/rle/rle.c
all:
	$(CC) $(CFLAGS) $(SRC) -o compressor