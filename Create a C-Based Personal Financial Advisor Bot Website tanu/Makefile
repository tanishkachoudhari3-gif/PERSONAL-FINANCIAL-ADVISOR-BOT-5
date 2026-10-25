CC ?= gcc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
LDFLAGS ?= -lm

all: centsage

centsage: src/main.c
	mkdir -p bin
	$(CC) $(CFLAGS) -o bin/centsage src/main.c $(LDFLAGS)

clean:
	rm -rf bin

.PHONY: all clean
