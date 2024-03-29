BIN_DIR = bin
CC = gcc
CFLAGS = -std=c99 -O3 -Wall -Wpedantic

all: mkbin http-server

%: %.c
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $<

.PHONY: clean mkbin

clean:
	rm -rf $(BIN_DIR)

mkbin:
	mkdir -p bin
