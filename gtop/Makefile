# gtop - system monitor TUI. C99 + POSIX.1-2008, libc and libm only (no curses).
#   make          build ./gtop
#   make test     run tests/run (needs a built gtop)
#   make install  PREFIX=/usr/local: binary and commands/ under share/gtop
CC ?= cc
CFLAGS ?= -O2
CFLAGS += -std=c99 -Wall -Wextra -Wno-unused-parameter
PREFIX ?= /usr/local

gtop: gtop.c
	$(CC) $(CFLAGS) -o $@ gtop.c -lm

test: gtop
	sh tests/run

install: gtop
	install -Dm755 gtop $(DESTDIR)$(PREFIX)/bin/gtop
	install -d $(DESTDIR)$(PREFIX)/share/gtop/commands
	install -m644 commands/* $(DESTDIR)$(PREFIX)/share/gtop/commands/

clean:
	rm -f gtop

.PHONY: test install clean
