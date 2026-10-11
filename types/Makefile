CC ?= cc
CFLAGS ?= -O2
CFLAGS += -std=gnu11 -Wall -Wextra -Werror -pthread

test: test.c types.h
	$(CC) $(CFLAGS) -o test_types test.c
	./test_types

clean:
	rm -f test_types

.PHONY: test clean
