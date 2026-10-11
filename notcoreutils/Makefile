# notcoreutils - small tools that have no GNU counterpart (or do something
# different from it). One file each, built to bin/NAME.
#
#   make              build everything
#   make cloc         build one tool: bin/cloc
#   make test         run tests/run
#   make install      PREFIX=/usr/local, binaries and man pages (sudo is NOT setuid)
#   make clean
#
# Headers the tools share live in deps/ (a handful of single-file headers).

CC      ?= cc
CFLAGS  ?= -O2
CSTD     = -std=gnu11 -D_GNU_SOURCE -U_FORTIFY_SOURCE   # hardened glibc headers clash with base.h
OUT     ?= bin
PREFIX  ?= /usr/local
INC      = -Ideps/utils -Ideps/libutil -Ideps/singlefile/abstract

TOOLS   := cb cloc colorado dv man netcat sudo tgates tree

LIBS_cb   = -lX11
LIBS_sudo = -lcrypt

all: $(addprefix $(OUT)/,$(TOOLS))

$(OUT):
	mkdir -p $(OUT)

define tool
$(OUT)/$(1): $(1).c | $(OUT)
	$$(CC) $$(CSTD) $$(CFLAGS) -w $$(INC) -o $$@ $(1).c $$(LIBS_$(1))
.PHONY: $(1)
$(1): $(OUT)/$(1)
endef
$(foreach t,$(TOOLS),$(eval $(call tool,$(t))))

test: all
	sh tests/run

install: all
	mkdir -p $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/share/man/man1
	for t in $(TOOLS); do install -m 755 $(OUT)/$$t $(DESTDIR)$(PREFIX)/bin/$$t; done
	for t in $(TOOLS); do [ -f $$t.1 ] && install -m 644 $$t.1 $(DESTDIR)$(PREFIX)/share/man/man1/$$t.1; done; true
	@echo "sudo was installed WITHOUT the setuid bit; add it yourself if you mean to use it"

clean:
	rm -rf $(OUT)

.PHONY: all test install clean
