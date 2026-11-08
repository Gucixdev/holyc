# desktop - build ONE executable from src/: dwl + dmenu + stest + menu + lemonbar (+ bar, tools as they are ported to C).
# make            -> ./desktop
# make install    -> $(PREFIX)/bin/desktop and one symlink per applet (dwl, dmenu, ...)
# wlroots is vendored in src/wlroots and linked statically (built once into build/wlroots by ./wlroots-static); everything else comes from pkg-config.
# How the programs share one binary: each is compiled with -Dmain=<name>_main, its objects are joined with `ld -r`, and objcopy
# keeps only <name>_main global - every other symbol becomes local, so drw.c, util.c and the generated protocol code can exist
# once per program without clashing, and no program is changed to fit.
CC       ?= cc
WLROOTS  ?= $(CURDIR)/build/wlroots
OPT      ?= -O2
PREFIX   ?= /usr/local
APPLETS   = dwl dmenu stest menu lemonbar bar autostart

WLPC  = PKG_CONFIG_PATH=$(WLROOTS)/lib/pkgconfig pkg-config
DWLCF = `pkg-config --cflags wayland-server xkbcommon libinput xcb xcb-icccm` `$(WLPC) --cflags wlroots-0.20` \
        -Isrc/dwl -DWLR_USE_UNSTABLE -D_POSIX_C_SOURCE=200809L -DVERSION=\"desktop\" -DXWAYLAND -Wno-unused-parameter
CLICF = `pkg-config --cflags wayland-client xkbcommon fontconfig freetype2` -D_DEFAULT_SOURCE -D_GNU_SOURCE -D_XOPEN_SOURCE=700 -DVERSION=\"desktop\"
LIBS  = `pkg-config --libs wayland-server xkbcommon libinput xcb xcb-icccm wayland-client fontconfig freetype2` \
        `$(WLPC) --static --libs wlroots-0.20` -lm

DMENU_SRC    = dmenu.c drw.c util.c wlr-layer-shell-unstable-v1-protocol.c xdg-shell-protocol.c
LEMONBAR_SRC = lemonbar.c utils.c util.c drw.c wlr-layer-shell-unstable-v1-protocol.c xdg-shell-protocol.c

all: desktop links

$(WLROOTS)/lib/libwlroots-0.20.a:
	./wlroots-static


# bin/<applet> -> ../desktop, so programs that run "dmenu" or "menu" through PATH find the applets
links: desktop
	mkdir -p bin
	for a in $(APPLETS); do ln -sf ../desktop bin/$$a; done

build:
	mkdir -p build

# program object: compile (renaming main), join, hide everything but <name>_main
build/dwl.o: src/dwl/dwl.c src/dwl/config.h $(WLROOTS)/lib/libwlroots-0.20.a | build
	$(CC) -c $(OPT) $(DWLCF) -Dmain=dwl_main src/dwl/dwl.c -o build/dwl.raw.o
	objcopy --keep-global-symbol=dwl_main build/dwl.raw.o $@

build/dmenu.o: $(addprefix src/dmenu/,$(DMENU_SRC)) | build
	for f in $(DMENU_SRC); do $(CC) -c $(OPT) -std=gnu11 $(CLICF) -Isrc/dmenu -Dmain=dmenu_main src/dmenu/$$f -o build/dmenu.$$f.o || exit 1; done
	ld -r -o build/dmenu.raw.o $(addprefix build/dmenu.,$(addsuffix .o,$(DMENU_SRC)))
	objcopy --keep-global-symbol=dmenu_main build/dmenu.raw.o $@

build/stest.o: src/dmenu/stest.c | build
	$(CC) -c $(OPT) -std=gnu11 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -Isrc/dmenu -Dmain=stest_main src/dmenu/stest.c -o build/stest.raw.o
	objcopy --keep-global-symbol=stest_main build/stest.raw.o $@

build/menu.o: src/dmenu/dmenu_menu.c | build
	$(CC) -c $(OPT) -std=gnu11 -D_DEFAULT_SOURCE -Dmain=menu_main src/dmenu/dmenu_menu.c -o build/menu.raw.o
	objcopy --keep-global-symbol=menu_main build/menu.raw.o $@

build/lemonbar.o: $(addprefix src/lemonbar/,$(LEMONBAR_SRC)) | build
	for f in $(LEMONBAR_SRC); do $(CC) -c $(OPT) -std=gnu11 $(CLICF) -Isrc/lemonbar -Dmain=lemonbar_main src/lemonbar/$$f -o build/lemonbar.$$f.o || exit 1; done
	ld -r -o build/lemonbar.raw.o $(addprefix build/lemonbar.,$(addsuffix .o,$(LEMONBAR_SRC)))
	objcopy --keep-global-symbol=lemonbar_main build/lemonbar.raw.o $@

build/bar.o: src/bar.c | build
	$(CC) -c $(OPT) -std=gnu11 -Wall -Wextra -Dmain=bar_main_unused src/bar.c -o build/bar.raw.o
	objcopy --keep-global-symbol=bar_main build/bar.raw.o $@

build/autostart.o: src/autostart.c | build
	$(CC) -c $(OPT) -std=gnu11 -Wall -Wextra -Dmain=autostart_main src/autostart.c -o build/autostart.raw.o
	objcopy --keep-global-symbol=autostart_main build/autostart.raw.o $@

build/main.o: src/main.c | build
	$(CC) -c $(OPT) -Wall -Wextra src/main.c -o $@

desktop: build/main.o $(addprefix build/,$(addsuffix .o,$(APPLETS)))
	$(CC) -o $@ $^ $(LIBS)

install: desktop
	install -Dm755 desktop $(DESTDIR)$(PREFIX)/bin/desktop
	for a in $(APPLETS); do ln -sf desktop $(DESTDIR)$(PREFIX)/bin/$$a; done

clean:
	rm -rf build desktop
.PHONY: all links install clean
