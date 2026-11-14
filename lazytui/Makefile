# lazytui - C library on plain ANSI escapes.
#   make            liblazytui.a + examples/demo + examples/TUItemplate
#   make test       every tests/test_*.c, built with ASAN+UBSAN, run
#   make install    headers + library under PREFIX (default /usr/local)
CC ?= cc
CFLAGS ?= -O2
CFLAGS += -std=c99 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -Wall -Wextra -Wno-unused-parameter -Iinclude
PREFIX ?= /usr/local
# every .c under src/ (core/{fx,io}, widgets/<group>/), found recursively
SRC := $(shell find src -name '*.c' | sort)
HDR := $(shell find src -name '*.h') include/lazytui.h
# every private header dir is on the include path, so widgets say #include "internal.h" wherever they live
INC := $(addprefix -I,$(sort $(dir $(shell find src -name '*.h'))))
OBJ := $(SRC:.c=.o)
TESTS := $(patsubst tests/%.c,tests/%,$(wildcard tests/test_*.c))
SAN := -g -O1 -fsanitize=address,undefined,float-cast-overflow -fno-omit-frame-pointer

all: liblazytui.a examples/demo examples/TUItemplate

liblazytui.a: $(OBJ)
	$(AR) rcs $@ $(OBJ)

src/%.o: src/%.c $(HDR)
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<

tests/%: tests/%.c liblazytui.a
	$(CC) $(CFLAGS) -o $@ $< liblazytui.a -lm

examples/demo: examples/demo.c liblazytui.a
	$(CC) $(CFLAGS) -o $@ $< liblazytui.a -lm

examples/TUItemplate: examples/TUItemplate.c liblazytui.a
	$(CC) $(CFLAGS) -o $@ $< liblazytui.a -lm

# sanitized copy of the library (built once into build/san), then every test linked against it
SANOBJ := $(patsubst src/%.c,build/san/%.o,$(SRC))
build/san/%.o: src/%.c $(HDR)
	@mkdir -p $(dir $@)
	$(CC) $(SAN) $(CFLAGS) $(INC) -c -o $@ $<

# every test under AddressSanitizer + UBSan (LT_FUZZ=1 for the exhaustive robust run); leak check only for test_robust
test: $(SANOBJ)
	@for t in tests/test_*.c; do \
		echo "== $$t"; \
		$(CC) $(SAN) $(CFLAGS) -o build/san/t $$t $(SANOBJ) -lm || exit 1; \
		case $$t in *robust*) l=1;; *) l=0;; esac; \
		ASAN_OPTIONS=detect_leaks=$$l build/san/t || exit 1; \
	done

asan: test

install: liblazytui.a
	install -d $(DESTDIR)$(PREFIX)/include $(DESTDIR)$(PREFIX)/lib
	install -m 644 include/lazytui.h $(DESTDIR)$(PREFIX)/include/
	install -m 644 liblazytui.a $(DESTDIR)$(PREFIX)/lib/

clean:
	rm -rf build; rm -f $(OBJ) liblazytui.a $(TESTS) examples/demo examples/TUItemplate

.PHONY: all test asan install clean
