/* gens.h - the generator table: name, what it emits, verification status, whether it is in the default apply set. stb-style single header. */
#ifndef COLORADO_GENS_H
#define COLORADO_GENS_H
#include "gen_gtk.h"
#include "gen_qt.h"
#include "gen_term.h"
#include "gen_desktop.h"
#include "gen_web.h"
#include "gen_misc.h"

static const co_gen co_gens[] = {
	{ "gtk3", "gtk-3.0/settings.ini + gtk.css", "verified: matches the installed ~/.config/gtk-3.0 example; css parsed by the real GTK3 css parser in the tests", 1, co_gen_gtk3 },
	{ "gtk4", "gtk-4.0/settings.ini + gtk.css (libadwaita names)", "verified: matches the installed ~/.config/gtk-4.0 example; css parsed by the real GTK4 css parser; libadwaita itself not installed", 1, co_gen_gtk4 },
	{ "gtk2", "~/.gtkrc-2.0", "unverified: no GTK2 here", 0, co_gen_gtk2 },
	{ "qt5ct", "qt5ct/qt5ct.conf + colors/colorado.conf", "verified: identical layout to installed qt5ct files (21 roles x 3 states); not rendered in a Qt app", 1, co_gen_qt5 },
	{ "qt6ct", "qt6ct/qt6ct.conf + colors/colorado.conf", "verified: identical layout to installed qt6ct files; not rendered in a Qt app", 1, co_gen_qt6 },
	{ "kvantum", "Kvantum/colorado/colorado.kvconfig", "unverified: Kvantum not installed", 0, co_gen_kvantum },
	{ "foot", "foot/colorado.ini (include file)", "verified: accepted by foot 1.28 --check-config", 1, co_gen_foot },
	{ "kitty", "kitty/colorado.conf", "unverified: kitty not installed", 0, co_gen_kitty },
	{ "alacritty", "alacritty/colorado.toml", "unverified: alacritty not installed", 0, co_gen_alacritty },
	{ "xresources", "~/.Xresources block", "unverified: xrdb not installed", 0, co_gen_xresources },
	{ "tmux", "tmux/colorado.conf", "verified: loaded by a real tmux server in the tests", 1, co_gen_tmux },
	{ "ansi", "colorado/palette.sh (OSC 4/10/11)", "unverified: not driven through a real terminal", 0, co_gen_ansi },
	{ "desktop", "colorado/desktop-theme.h for desktop/src/{theme,menu,bar}.c", "verified: theme.c part compiles with the desktop's COLOR macro; not rebuilt into the desktop", 1, co_gen_desktop },
	{ "firefox", "colorado/firefox/userChrome.css + userContent.css", "unverified: no Firefox profile touched", 0, co_gen_firefox },
	{ "electron", "note only (chromium flags)", "unverified: note only", 0, co_gen_electron },
	{ "flatpak", "note only (flatpak override commands)", "unverified: printed, never run", 0, co_gen_flatpak },
	{ "xsettingsd", "xsettingsd/xsettingsd.conf block", "unverified: xsettingsd not installed", 0, co_gen_xsettingsd },
	{ "cursor", "~/.icons/default/index.theme", "unverified: per freedesktop spec only", 0, co_gen_cursor },
	{ "fontconfig", "fontconfig/conf.d/60-colorado.conf", "verified: loaded by the real fc-match in the tests", 1, co_gen_fontconfig },
};
#define CO_NGENS ((int)(sizeof co_gens / sizeof *co_gens))
static const co_gen *co_gen_find(const char *n) { for (int i = 0; i < CO_NGENS; i++) if (!strcmp(co_gens[i].name, n)) return &co_gens[i]; return NULL; }
#endif
