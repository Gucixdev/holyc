# dmenu version
VERSION = 5.4

# paths
PREFIX = /usr/local
MANPREFIX = $(PREFIX)/share/man

# real Wayland port (2026-08-24): wayland-client + xkbcommon (windowing/
# input) and fontconfig + freetype2 (real font resolution/rasterization
# - drw.c kept both, only the X-specific compositing step changed, see
# drw.h's own top comment), all via pkg-config.
WLPKGS = wayland-client xkbcommon fontconfig freetype2

# includes and libs
INCS = `pkg-config --cflags $(WLPKGS)`
LIBS = `pkg-config --libs $(WLPKGS)`

# flags
CPPFLAGS = -D_DEFAULT_SOURCE -D_BSD_SOURCE -D_XOPEN_SOURCE=700 -D_POSIX_C_SOURCE=200809L -DVERSION=\"$(VERSION)\"
CFLAGS   = -std=c99 -pedantic -Wall -Os $(INCS) $(CPPFLAGS)
LDFLAGS  = $(LIBS)

# compiler and linker
CC = cc
