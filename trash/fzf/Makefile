# fzf (C) - libc only. `make` builds ./fzf, `make install` copies it to ~/.local/bin, `make test` runs tests/run.
CC ?= cc
CFLAGS ?= -O2
CFLAGS += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -pthread
SRC = main.c util.c algo.c pattern.c opts.c ansi.c term.c theme.c keys.c draw.c ui.c act.c proc.c hints.c
OBJ = $(SRC:.c=.o)

fzf: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) -lm

%.o: %.c fzf.h opts.h ui.h term.h uistate.h
	$(CC) $(CFLAGS) -c -o $@ $<

install: fzf
	install -Dm755 fzf $(HOME)/.local/bin/fzf

test: fzf
	sh tests/run

clean:
	rm -f fzf $(OBJ)

.PHONY: install test clean
