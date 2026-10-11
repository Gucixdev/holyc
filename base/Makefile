# Header-only library: tests and benchmarks compile against src/.
#   make test      build and run every tests/test_*.c (parallel)
#   make bench     build and run the benchmarks in bench/ (each compares against malloc or a naive loop)
CC ?= cc
FLAGS = -std=gnu11 -O2 -w -D_GNU_SOURCE -Isrc -Isrc/hash -Ibench -Itests

test:
	sh tests/run

bench:
	@set -e; for f in bench/bench_*.c; do n=$$(basename $$f .c); \
		$(CC) $(FLAGS) -o /tmp/hc_$$n $$f -lm -lpthread; echo "== $$n"; /tmp/hc_$$n; rm -f /tmp/hc_$$n; done

.PHONY: test bench
