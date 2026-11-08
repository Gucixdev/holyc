CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter
FT = $(shell pkg-config --cflags --libs freetype2)
WEBP = $(shell pkg-config --libs libwebp libwebpmux)
autodemo: src/autodemo.c
	nice -n 19 $(CC) $(CFLAGS) -o $@ $< $(FT) $(WEBP) -lm
test: autodemo
	sh test/run
clean:
	mv -f autodemo /data/dev/trash/ 2>/dev/null || true
.PHONY: test clean
