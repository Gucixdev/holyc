# alloc: bump arena and fixed-size slab on a replaceable allocator hook.
# make test builds and runs tests/test_arena.c
CC ?= cc
CFLAGS ?= -O2
CFLAGS += -std=gnu11 -Wall -Wextra

test: tests/test_arena
	./tests/test_arena

tests/test_arena: tests/test_arena.c arena.c mem.c arena.h mem.h
	$(CC) $(CFLAGS) -I. -o $@ tests/test_arena.c arena.c mem.c -lm

clean:
	rm -f tests/test_arena

.PHONY: test clean
