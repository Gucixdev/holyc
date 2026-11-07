CC = cc
CFLAGS = -O3 -ffp-contract=off -fno-math-errno -std=c99 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -Wall -Wextra -Iinclude
SRC = src/ecs.c src/ecs_mem.c src/ecs_arena.c src/ecs_event.c src/ecs_sched.c src/ecs_math.c src/ecs_dynamics.c src/ecs_stats.c src/ecs_stream.c src/ecs_gas.c
OBJ = $(SRC:.c=.o)
TESTS = $(patsubst tests/%.c,tests/%,$(wildcard tests/test_*.c))

all: libecs.a

libecs.a: $(OBJ)
	ar rcs $@ $(OBJ)

src/%.o: src/%.c $(wildcard include/*.h)
	$(CC) $(CFLAGS) -c -o $@ $<

tests/%: tests/%.c libecs.a
	$(CC) $(CFLAGS) -o $@ $< libecs.a -lm

test: $(TESTS)
	@for t in $(TESTS); do ./$$t || exit 1; done

asan:
	@for t in tests/test_*.c; do \
		$(CC) -g -O1 -fsanitize=address,undefined,float-cast-overflow -fno-omit-frame-pointer $(CFLAGS) -o /tmp/ecs_asan $$t $(SRC) -lm || exit 1; \
		ASAN_OPTIONS=detect_leaks=1 /tmp/ecs_asan || exit 1; \
	done

clean:
	rm -f $(OBJ) libecs.a $(TESTS)

.PHONY: all test asan clean
