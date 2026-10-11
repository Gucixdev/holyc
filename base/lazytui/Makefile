# lazytui - C library on plain ANSI escapes. `make` builds liblazytui.a and the demo, `make test` runs the tests.
CC ?= cc
CFLAGS ?= -O2
CFLAGS += -std=c99 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -Wall -Wextra -Wno-unused-parameter -Iinclude
SRC = $(wildcard src/*.c src/widgets/*.c)
OBJ = $(SRC:.c=.o)
TESTS = $(patsubst tests/%.c,tests/%,$(wildcard tests/test_*.c))

all: liblazytui.a examples/demo

liblazytui.a: $(OBJ)
	$(AR) rcs $@ $(OBJ)

src/%.o: src/%.c include/lazytui.h src/internal.h src/appint.h src/places.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/widgets/%.o: src/widgets/%.c include/lazytui.h src/internal.h src/places.h $(wildcard src/widgets/*.h)
	$(CC) $(CFLAGS) -Isrc -c -o $@ $<

tests/%: tests/%.c liblazytui.a
	$(CC) $(CFLAGS) -o $@ $< liblazytui.a -lm

examples/demo: examples/demo.c liblazytui.a
	$(CC) $(CFLAGS) -o $@ $< liblazytui.a -lm

test: $(TESTS)
	@for t in $(TESTS); do ./$$t || exit 1; done

clean:
	rm -f $(OBJ) liblazytui.a $(TESTS) examples/demo

.PHONY: all test clean

# every test again under AddressSanitizer (LT_FUZZ=1 make asan for the exhaustive run) + UndefinedBehaviorSanitizer (leak checking only for test_robust, which frees all it makes)
asan:
	@for t in tests/test_*.c; do \
		$(CC) -g -O1 -fsanitize=address,undefined,float-cast-overflow -fno-omit-frame-pointer $(CFLAGS) -Isrc -o /tmp/lt_asan $$t $(SRC) -lm || exit 1; \
		case $$t in *robust*) ASAN_OPTIONS=detect_leaks=1 /tmp/lt_asan || exit 1;; *) ASAN_OPTIONS=detect_leaks=0 /tmp/lt_asan || exit 1;; esac; \
	done
.PHONY: asan
