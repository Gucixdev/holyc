# desktop - build ONE executable from src/: the dwl compositor, the bar, the menu/dmenu picker, lemonbar, stest and small tools.
# make                       -> ./desktop plus bin/<applet> symlinks
# make install               -> $(PREFIX)/bin/desktop and one symlink per applet
# make BUILD=build-new OUT=build-new/desktop LINKDIR=build-new/bin     (build somewhere else; the default paths stay untouched)
# wlroots (src/resources/wlroots) is linked statically (built once into build/wlroots by ./wlroots-static); wtype and wvkbd in
# src/resources are separate programs. Everything else comes from pkg-config.
#
# How the programs share one binary: every program is a group of objects joined with `ld -r`, and objcopy keeps only the group's
# <name>_main entry points global - every other symbol becomes local, so two programs may both have a `drw` or a `main loop`
# static-ish global without clashing. Shared code (src/util.c) stays global; src/drw.c and the client protocol glue are joined
# into the dmenu/menu/lemonbar group ONCE and used by all three.
CC       ?= cc
WLROOTS  ?= $(CURDIR)/build/wlroots
BUILD    ?= build
OUT      ?= desktop
LINKDIR  ?= bin
OPT      ?= -O2
PREFIX   ?= /usr/local
APPLETS   = dwl dmenu stest menu lemonbar bar autostart wsw settings

WLPC  = PKG_CONFIG_PATH=$(WLROOTS)/lib/pkgconfig pkg-config
DWLCF = `pkg-config --cflags wayland-server xkbcommon libinput xcb xcb-icccm` `$(WLPC) --cflags wlroots-0.20` \
        -Isrc -Isrc/protocols/server -DWLR_USE_UNSTABLE -D_POSIX_C_SOURCE=200809L -DVERSION=\"desktop\" -DXWAYLAND -Wno-unused-parameter
CLICF = `pkg-config --cflags wayland-client xkbcommon fontconfig freetype2` -Isrc -Isrc/protocols/client \
        -D_DEFAULT_SOURCE -D_GNU_SOURCE -D_XOPEN_SOURCE=700 -DVERSION=\"desktop\"
LIBS  = `pkg-config --libs wayland-server xkbcommon libinput xcb xcb-icccm wayland-client fontconfig freetype2` \
        `$(WLPC) --static --libs wlroots-0.20` -lm

DWL_SRC    = compositor client keys settings fx ime config theme api
CLIENT_SRC = menu lemonbar drw panel
CLIENT_PROTO = xdg-shell-protocol wlr-layer-shell-unstable-v1-protocol

all: $(OUT) links

$(WLROOTS)/lib/libwlroots-0.20.a:
	./wlroots-static

# bin/<applet> -> the executable, so programs that run "dmenu" or "menu" through PATH find the applets
links: $(OUT)
	mkdir -p $(LINKDIR)
	for a in $(APPLETS); do ln -sf `realpath -m --relative-to=$(LINKDIR) $(OUT)` $(LINKDIR)/$$a; done

$(BUILD):
	mkdir -p $(BUILD)

# --- compositor: src/{compositor,client,keys,settings,fx,ime,config,theme}.c share src/dwl.h
$(BUILD)/dwl.%.o: src/%.c src/dwl.h src/client.h src/util.h $(WLROOTS)/lib/libwlroots-0.20.a | $(BUILD)
	$(CC) -c $(OPT) $(DWLCF) $< -o $@

$(BUILD)/dwl.o: $(addprefix $(BUILD)/dwl.,$(addsuffix .o,$(DWL_SRC)))
	ld -r -o $(BUILD)/dwl.raw.o $^
	objcopy --keep-global-symbol=dwl_main --keep-global-symbol=dwl_dumpapi $(BUILD)/dwl.raw.o $@

# --- dmenu + menu + lemonbar: one copy of drw.c and of the protocol glue
$(BUILD)/cli.%.o: src/%.c src/drw.h src/util.h | $(BUILD)
	$(CC) -c $(OPT) -std=gnu11 $(CLICF) $< -o $@

$(BUILD)/cli.%.o: src/protocols/client/%.c | $(BUILD)
	$(CC) -c $(OPT) -std=gnu11 $(CLICF) $< -o $@

$(BUILD)/clients.o: $(addprefix $(BUILD)/cli.,$(addsuffix .o,$(CLIENT_SRC) $(CLIENT_PROTO)))
	ld -r -o $(BUILD)/clients.raw.o $^
	objcopy --keep-global-symbol=dmenu_main --keep-global-symbol=menu_main --keep-global-symbol=lemonbar_main --keep-global-symbol=settings_main $(BUILD)/clients.raw.o $@

$(BUILD)/stest.o: src/stest.c src/arg.h | $(BUILD)
	$(CC) -c $(OPT) -std=gnu11 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -Isrc $< -o $(BUILD)/stest.raw.o
	objcopy --keep-global-symbol=stest_main $(BUILD)/stest.raw.o $@

$(BUILD)/util.o: src/util.c src/util.h | $(BUILD)
	$(CC) -c $(OPT) -std=gnu11 -Wall -Wextra -Isrc $< -o $@

$(BUILD)/bar.o: src/bar.c | $(BUILD)
	$(CC) -c $(OPT) -std=gnu11 -Wall -Wextra -Dmain=bar_main_unused src/bar.c -o $(BUILD)/bar.raw.o
	objcopy --keep-global-symbol=bar_main $(BUILD)/bar.raw.o $@

$(BUILD)/autostart.o: src/autostart.c | $(BUILD)
	$(CC) -c $(OPT) -std=gnu11 -Wall -Wextra -Dmain=autostart_main src/autostart.c -o $(BUILD)/autostart.raw.o
	objcopy --keep-global-symbol=autostart_main $(BUILD)/autostart.raw.o $@

$(BUILD)/wsw.o: src/wsw.c | $(BUILD)
	$(CC) -c $(OPT) -std=gnu11 -D_DEFAULT_SOURCE -Wall -Wextra -Dmain=wsw_main src/wsw.c -o $(BUILD)/wsw.raw.o
	objcopy --keep-global-symbol=wsw_main $(BUILD)/wsw.raw.o $@

$(BUILD)/main.o: src/main.c | $(BUILD)
	$(CC) -c $(OPT) -Wall -Wextra src/main.c -o $@

$(OUT): $(BUILD)/main.o $(BUILD)/util.o $(BUILD)/dwl.o $(BUILD)/clients.o $(BUILD)/stest.o $(BUILD)/bar.o $(BUILD)/autostart.o $(BUILD)/wsw.o
	$(CC) -o $@ $^ $(LIBS)

install: $(OUT)
	install -Dm755 $(OUT) $(DESTDIR)$(PREFIX)/bin/desktop
	for a in $(APPLETS); do ln -sf desktop $(DESTDIR)$(PREFIX)/bin/$$a; done

clean:
	rm -rf $(BUILD) $(OUT)
.PHONY: all links install clean
