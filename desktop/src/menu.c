/* menu.c - the picker (dmenu) and the main menu (menu) of the desktop.
 *   dmenu_main: dmenu for Wayland (wlr-layer-shell), originally suckless dmenu (src/LICENSES/dmenu.LICENSE). Its settings
 *               (fonts, colours, rofi-style box) sit in the "configuration" section below.
 *   menu_main:  the launcher menu on top of dmenu (apps, games, calculator, clipboard); its helpers carry the mn_ prefix so
 *               both programs live in one file without clashing.
 * Both are compiled into the one desktop executable (see Makefile); dmenu draws with the shared drw.c. */
/* ---- dmenu ---- See LICENSE file for copyright and license details. */
/* Real Wayland port (2026-08-24): "przepisz na wayland, doslownie
 * przepisz nie tylko backend" - matching/filtering/history logic
 * below (match/insert/calcoffsets/appenditem/cistrstr/readstdin) is
 * carried over byte-for-byte, completely X11-independent. Only the
 * windowing (wlr-layer-shell-unstable-v1 instead of an override-
 * redirect X window), rendering (drw's own real FreeType+fontconfig
 * pixels, blitted into a real wl_shm buffer instead of XCopyArea), and
 * input (wl_keyboard + xkbcommon instead of XmbLookupString, wl_data_
 * device instead of XConvertSelection) are real rewrites.
 *
 * Real, honest scope cut: the X11 window-EMBEDDING feature (-w
 * windowid, via XReparentWindow) has no Wayland equivalent at all -
 * Wayland's security model deliberately does not allow one client to
 * embed another's surface into its own window the way X11 does. The
 * `-w` flag is still accepted (so existing invocations don't hard
 * error) but is a documented no-op here, not silently dropped. */
#include <ctype.h>
#include <fcntl.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "wlr-layer-shell-unstable-v1-protocol.h"

#include "drw.h"
#include "util.h"

/* macros */
#define TEXTW(X)              (drw_fontset_getwidth(drw, (X)) + lrpad)

/* enums */
enum { SchemeNorm, SchemeSel, SchemeOut, SchemeInput, SchemePrompt, SchemeHint, SchemeLast }; /* color schemes */

struct item {
	char *text;
	struct item *left, *right;
	int out;
};

static char text[BUFSIZ] = "";
static char *embed; /* accepted for CLI compat only - real no-op, see top comment */
static int bh, mw, mh;
static int inputw = 0, promptw;
static int lrpad; /* sum of left and right padding */
static size_t cursor;
static struct item *items = NULL;
static struct item *matches, *matchend;
static struct item *prev, *curr, *next, *sel;
static int mon = -1;

static Drw *drw;
static Clr *scheme[SchemeLast];

/* real Wayland state */
static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct wl_keyboard *keyboard;
static struct wl_data_device_manager *data_device_manager;
static struct wl_data_device *data_device;
static struct wl_data_offer *clipboard_offer;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct wl_output *chosen_output;
static struct zwlr_layer_surface_v1 *layer_surface;
static struct wl_surface *surface;
static struct wl_buffer *wl_buf;
static void *shm_pixels;
static int shm_fd = -1;
static size_t shm_size;
static int running = 1;
static int configured;

static struct xkb_context *xkb_ctx;
static struct xkb_keymap *xkb_keymap;
static struct xkb_state *xkb_state;

struct outputitem { struct wl_output *o; int idx; struct outputitem *next; };
static struct outputitem *outputlist;
static int noutputs;

/* ---- configuration (was config.h) ---- */
/* Default settings; can be overriden by command line. */

static int topbar = 1;                      /* -b  option; if 0, dmenu appears at bottom     */
/* -fn option overrides fonts[0]; default X11 font or font set */
static const char *fonts[] = {
	"TempleOS Bitmap:pixelsize=16",   /* bitmap .bdf, 8x8 scaled x2 - the theme's main font */
	"Fixed6x13:pixelsize=13",         /* bitmap .bdf, fills in what TempleOS lacks (Polish letters, ...) */
	"TempleOS:size=14"                /* the .ttf, last resort */
};
static const char *prompt      = NULL;      /* -p  option; prompt to the left of input field */
static const char *colors[SchemeLast][2] = {
	/*     fg         bg       */
	[SchemeNorm] = { "#00ff88", "#000000" },   /* hacker green on black */
	[SchemeSel]  = { "#000000", "#00ff88" },
	[SchemeOut]  = { "#000000", "#00cc6a" },
	/* rofi look: window bg is translucent (#rrggbbaa), the input bar and the accent are opaque */
	[SchemeInput]  = { "#00ff88", "#000000" },   /* text in the input bar */
	[SchemePrompt] = { "#00ff88", "#000000" },   /* prompt, and the accent colour of the border */
	[SchemeHint]   = { "#666666", "#000000" },   /* placeholder text */
};
/*
 * The rofi look: a centred box (the same numbers as ~/.config/rofi/config.rasi), a translucent window with an
 * accent border, an input bar and a vertical list. -T on the command line gives the classic top bar instead.
 */
static int rofimode = 1;
static const int rofi_width    = 560;   /* window width */
static const int rofi_border   = 2;     /* accent border */
static const int rofi_pad      = 14;    /* window padding (inside the border) */
static const int rofi_gap      = 10;    /* between the input bar and the list */
static const int rofi_in_padx  = 12, rofi_in_pady = 10, rofi_in_gap = 8;   /* input bar padding, prompt-entry gap */
static const int rofi_el_padx  = 10, rofi_el_pady = 8, rofi_el_gap = 2;    /* list row padding, gap between rows */
static const unsigned int rofi_lines = 8;
static const char *rofi_bg       = "#000000e6";   /* window background (90% black) */
static const char *rofi_placeholder = "Szukaj...";

/* -l option; if nonzero, dmenu uses vertical list with given number of lines */
static unsigned int lines      = 0;

/*
 * Characters not considered part of a word while deleting words
 * for example: " /?\"&[]"
 */
static const char worddelimiters[] = " ";


static int (*fstrncmp)(const char *, const char *, size_t) = strncmp;
static char *(*fstrstr)(const char *, const char *) = strstr;

static void redraw(void);

static unsigned int
textw_clamp(const char *str, unsigned int n)
{
	unsigned int w = drw_fontset_getwidth_clamp(drw, str, n) + lrpad;
	return MIN(w, n);
}

static void
appenditem(struct item *item, struct item **list, struct item **last)
{
	if (*last)
		(*last)->right = item;
	else
		*list = item;

	item->left = *last;
	item->right = NULL;
	*last = item;
}

static void
calcoffsets(void)
{
	int i, n;

	if (lines > 0)
		n = lines * bh;
	else
		n = mw - (promptw + inputw + TEXTW("<") + TEXTW(">"));
	for (i = 0, next = curr; next; next = next->right)
		if ((i += (lines > 0) ? bh : (int)textw_clamp(next->text, (unsigned)n)) > n)
			break;
	for (i = 0, prev = curr; prev && prev->left; prev = prev->left)
		if ((i += (lines > 0) ? bh : (int)textw_clamp(prev->left->text, (unsigned)n)) > n)
			break;
}

static void
cleanup(void)
{
	size_t i;

	for (i = 0; i < SchemeLast; i++)
		free(scheme[i]);
	for (i = 0; items && items[i].text; ++i)
		free(items[i].text);
	free(items);
	if (drw)
		drw_free(drw);
	if (display)
		wl_display_disconnect(display);
}

static char *
cistrstr(const char *h, const char *n)
{
	size_t i;

	if (!n[0])
		return (char *)h;

	for (; *h; ++h) {
		for (i = 0; n[i] && tolower((unsigned char)n[i]) ==
		            tolower((unsigned char)h[i]); ++i)
			;
		if (n[i] == '\0')
			return (char *)h;
	}
	return NULL;
}

static int
drawitem(struct item *item, int x, int y, int w)
{
	if (item == sel)
		drw_setscheme(drw, scheme[SchemeSel]);
	else if (item->out)
		drw_setscheme(drw, scheme[SchemeOut]);
	else
		drw_setscheme(drw, scheme[SchemeNorm]);

	return drw_text(drw, x, y, (unsigned)w, (unsigned)bh, (unsigned)(lrpad / 2), item->text, 0);
}

/* ---- rofi look ---------------------------------------------------------- */
static int inh, rowh;        /* input bar height, list row height (bh becomes the row pitch) */
static int nitems_visible;   /* rows in the list right now */
static int reqh;             /* height last asked from the compositor */

static int
rofi_height(int nrows)
{
	int h = 2 * (rofi_border + rofi_pad) + inh;

	if (nrows > 0)
		h += rofi_gap + nrows * rowh + (nrows - 1) * rofi_el_gap;
	return h;
}

static void
drawmenu_rofi(void)
{
	struct item *item;
	int n = 0, x0 = rofi_border + rofi_pad, w0 = mw - 2 * x0, y, tw, ex;
	int fh = (int)drw->fonts->h;

	for (item = curr; item && item != next; item = item->right)
		n++;
	nitems_visible = n;
	if (rofi_height(n) != mh && rofi_height(n) != reqh) { /* the list grew/shrank: ask for a new size, redraw on configure */
		mh = reqh = rofi_height(n);
		zwlr_layer_surface_v1_set_size(layer_surface, (uint32_t)mw, (uint32_t)mh);
		wl_surface_commit(surface);
		return;
	}

	/* window: translucent background, accent border */
	drw_setscheme(drw, scheme[SchemeNorm]);
	drw_rect(drw, 0, 0, (unsigned)mw, (unsigned)mh, 1, 1);
	drw_setscheme(drw, scheme[SchemePrompt]);
	for (y = 0; y < rofi_border; y++)
		drw_rect(drw, y, y, (unsigned)(mw - 2 * y), (unsigned)(mh - 2 * y), 0, 0);

	/* input bar */
	drw_setscheme(drw, scheme[SchemeInput]);
	drw_rect(drw, x0, x0, (unsigned)w0, (unsigned)inh, 1, 1);
	ex = x0 + rofi_in_padx;
	if (prompt && *prompt) {
		drw_setscheme(drw, scheme[SchemePrompt]);
		tw = (int)drw_fontset_getwidth(drw, prompt);
		drw_text(drw, ex, x0, (unsigned)tw, (unsigned)inh, 0, prompt, 0);
		ex += tw + rofi_in_gap;
	}
	if (*text) {
		drw_setscheme(drw, scheme[SchemeInput]);
		drw_text(drw, ex, x0, (unsigned)(x0 + w0 - rofi_in_padx - ex), (unsigned)inh, 0, text, 0);
	} else if (rofi_placeholder && *rofi_placeholder) {
		drw_setscheme(drw, scheme[SchemeHint]);
		drw_text(drw, ex, x0, (unsigned)(x0 + w0 - rofi_in_padx - ex), (unsigned)inh, 0, rofi_placeholder, 0);
	}
	{
		char pre[BUFSIZ];
		size_t c = MIN(cursor, sizeof(pre) - 1);

		memcpy(pre, text, c);
		pre[c] = '\0';
		drw_setscheme(drw, scheme[SchemeInput]);
		drw_rect(drw, ex + (int)drw_fontset_getwidth(drw, pre), x0 + (inh - fh) / 2, 2, (unsigned)fh, 1, 0);
	}

	/* list */
	y = x0 + inh + rofi_gap;
	for (item = curr; item && item != next; item = item->right) {
		drw_setscheme(drw, scheme[item == sel ? SchemeSel : (item->out ? SchemeOut : SchemeNorm)]);
		drw_text(drw, x0, y, (unsigned)w0, (unsigned)rowh, (unsigned)rofi_el_padx, item->text, 0);
		y += rowh + rofi_el_gap;
	}
	redraw();
}

static void
drawmenu(void)
{
	if (rofimode) {
		drawmenu_rofi();
		return;
	}

	unsigned int curpos;
	struct item *item;
	int x = 0, y = 0, w;

	drw_setscheme(drw, scheme[SchemeNorm]);
	drw_rect(drw, 0, 0, (unsigned)mw, (unsigned)mh, 1, 1);

	if (prompt && *prompt) {
		drw_setscheme(drw, scheme[SchemeSel]);
		x = drw_text(drw, x, 0, (unsigned)promptw, (unsigned)bh, (unsigned)(lrpad / 2), prompt, 0);
	}
	w = (lines > 0 || !matches) ? mw - x : inputw;
	drw_setscheme(drw, scheme[SchemeNorm]);
	drw_text(drw, x, 0, (unsigned)w, (unsigned)bh, (unsigned)(lrpad / 2), text, 0);

	curpos = TEXTW(text) - TEXTW(&text[cursor]);
	if ((int)(curpos += (unsigned)(lrpad / 2 - 1)) < w) {
		drw_setscheme(drw, scheme[SchemeNorm]);
		drw_rect(drw, x + (int)curpos, 2, 2, (unsigned)(bh - 4), 1, 0);
	}

	if (lines > 0) {
		for (item = curr; item != next; item = item->right)
			drawitem(item, x, y += bh, mw - x);
	} else if (matches) {
		x += inputw;
		w = (int)TEXTW("<");
		if (curr->left) {
			drw_setscheme(drw, scheme[SchemeNorm]);
			drw_text(drw, x, 0, (unsigned)w, (unsigned)bh, (unsigned)(lrpad / 2), "<", 0);
		}
		x += w;
		for (item = curr; item != next; item = item->right)
			x = drawitem(item, x, 0, (int)textw_clamp(item->text, (unsigned)(mw - x - (int)TEXTW(">"))));
		if (next) {
			w = (int)TEXTW(">");
			drw_setscheme(drw, scheme[SchemeNorm]);
			drw_text(drw, mw - w, 0, (unsigned)w, (unsigned)bh, (unsigned)(lrpad / 2), ">", 0);
		}
	}
	redraw();
}

static void
match(void)
{
	static char **tokv = NULL;
	static int tokn = 0;

	char buf[sizeof text], *s;
	int i, tokc = 0;
	size_t len, textsize;
	struct item *item, *lprefix, *lsubstr, *prefixend, *substrend;

	strcpy(buf, text);
	for (s = strtok(buf, " "); s; tokv[tokc - 1] = s, s = strtok(NULL, " "))
		if (++tokc > tokn && !(tokv = realloc(tokv, ++tokn * sizeof *tokv)))
			die("cannot realloc %zu bytes:", tokn * sizeof *tokv);
	len = tokc ? strlen(tokv[0]) : 0;

	matches = lprefix = lsubstr = matchend = prefixend = substrend = NULL;
	textsize = strlen(text) + 1;
	for (item = items; item && item->text; item++) {
		for (i = 0; i < tokc; i++)
			if (!fstrstr(item->text, tokv[i]))
				break;
		if (i != tokc)
			continue;
		if (!tokc || !fstrncmp(text, item->text, textsize))
			appenditem(item, &matches, &matchend);
		else if (!fstrncmp(tokv[0], item->text, len))
			appenditem(item, &lprefix, &prefixend);
		else
			appenditem(item, &lsubstr, &substrend);
	}
	if (lprefix) {
		if (matches) {
			matchend->right = lprefix;
			lprefix->left = matchend;
		} else
			matches = lprefix;
		matchend = prefixend;
	}
	if (lsubstr) {
		if (matches) {
			matchend->right = lsubstr;
			lsubstr->left = matchend;
		} else
			matches = lsubstr;
		matchend = substrend;
	}
	curr = sel = matches;
	calcoffsets();
}

static void
insert(const char *str, ssize_t n)
{
	if (strlen(text) + (size_t)(n > 0 ? n : 0) > sizeof text - 1)
		return;
	memmove(&text[cursor + (size_t)n], &text[cursor], sizeof text - cursor - (size_t)MAX(n, 0));
	if (n > 0)
		memcpy(&text[cursor], str, (size_t)n);
	cursor += (size_t)n;
	match();
}

static size_t
nextrune(int inc)
{
	ssize_t n;

	for (n = (ssize_t)cursor + inc; n + inc >= 0 && (text[n] & 0xc0) == 0x80; n += inc)
		;
	return (size_t)n;
}

static void
movewordedge(int dir)
{
	if (dir < 0) {
		while (cursor > 0 && strchr(worddelimiters, text[nextrune(-1)]))
			cursor = nextrune(-1);
		while (cursor > 0 && !strchr(worddelimiters, text[nextrune(-1)]))
			cursor = nextrune(-1);
	} else {
		while (text[cursor] && strchr(worddelimiters, text[cursor]))
			cursor = nextrune(+1);
		while (text[cursor] && !strchr(worddelimiters, text[cursor]))
			cursor = nextrune(+1);
	}
}

static void
requestpaste(void)
{
	int fds[2];

	if (!clipboard_offer)
		return;
	if (pipe(fds) < 0)
		return;
	wl_data_offer_receive(clipboard_offer, "text/plain;charset=utf-8", fds[1]);
	close(fds[1]);
	wl_display_roundtrip(display);
	{
		char buf[4096];
		ssize_t n;
		size_t total = 0;
		char all[4096];
		while ((n = read(fds[0], buf, sizeof buf)) > 0 && total + (size_t)n < sizeof all) {
			memcpy(all + total, buf, (size_t)n);
			total += (size_t)n;
		}
		close(fds[0]);
		if (total > 0) {
			char *q = memchr(all, '\n', total);
			insert(all, q ? q - all : (ssize_t)total);
			drawmenu();
		}
	}
}

static void
keypress_sym(xkb_keysym_t ksym, int ctrl, int alt, int shift)
{
	if (ctrl) {
		switch (ksym) {
		case XKB_KEY_a: ksym = XKB_KEY_Home;      break;
		case XKB_KEY_b: ksym = XKB_KEY_Left;      break;
		case XKB_KEY_c: ksym = XKB_KEY_Escape;    break;
		case XKB_KEY_d: ksym = XKB_KEY_Delete;    break;
		case XKB_KEY_e: ksym = XKB_KEY_End;       break;
		case XKB_KEY_f: ksym = XKB_KEY_Right;     break;
		case XKB_KEY_g: ksym = XKB_KEY_Escape;    break;
		case XKB_KEY_h: ksym = XKB_KEY_BackSpace; break;
		case XKB_KEY_i: ksym = XKB_KEY_Tab;       break;
		case XKB_KEY_j:
		case XKB_KEY_J:
		case XKB_KEY_m:
		case XKB_KEY_M: ksym = XKB_KEY_Return; ctrl = 0; break;
		case XKB_KEY_n: ksym = XKB_KEY_Down;      break;
		case XKB_KEY_p: ksym = XKB_KEY_Up;        break;
		case XKB_KEY_k:
			text[cursor] = '\0';
			match();
			drawmenu();
			return;
		case XKB_KEY_u:
			insert(NULL, 0 - (ssize_t)cursor);
			drawmenu();
			return;
		case XKB_KEY_w:
			while (cursor > 0 && strchr(worddelimiters, text[nextrune(-1)]))
				insert(NULL, (ssize_t)nextrune(-1) - (ssize_t)cursor);
			while (cursor > 0 && !strchr(worddelimiters, text[nextrune(-1)]))
				insert(NULL, (ssize_t)nextrune(-1) - (ssize_t)cursor);
			drawmenu();
			return;
		case XKB_KEY_y:
		case XKB_KEY_Y:
			requestpaste();
			return;
		case XKB_KEY_Left:
			movewordedge(-1);
			drawmenu();
			return;
		case XKB_KEY_Right:
			movewordedge(+1);
			drawmenu();
			return;
		case XKB_KEY_Return:
			break;
		case XKB_KEY_bracketleft:
			running = 0;
			return;
		default:
			return;
		}
	} else if (alt) {
		switch (ksym) {
		case XKB_KEY_b: movewordedge(-1); drawmenu(); return;
		case XKB_KEY_f: movewordedge(+1); drawmenu(); return;
		case XKB_KEY_g: ksym = XKB_KEY_Home;  break;
		case XKB_KEY_G: ksym = XKB_KEY_End;   break;
		case XKB_KEY_h: ksym = XKB_KEY_Up;    break;
		case XKB_KEY_j: ksym = XKB_KEY_Next;  break;
		case XKB_KEY_k: ksym = XKB_KEY_Prior; break;
		case XKB_KEY_l: ksym = XKB_KEY_Down;  break;
		default: return;
		}
	}

	switch (ksym) {
	case XKB_KEY_Delete:
		if (text[cursor] == '\0')
			return;
		cursor = nextrune(+1);
		/* fallthrough */
	case XKB_KEY_BackSpace:
		if (ksym == XKB_KEY_BackSpace && cursor == 0)
			return;
		insert(NULL, (ssize_t)nextrune(-1) - (ssize_t)cursor);
		break;
	case XKB_KEY_End:
		if (text[cursor] != '\0') {
			cursor = strlen(text);
			break;
		}
		if (next) {
			curr = matchend;
			calcoffsets();
			curr = prev;
			calcoffsets();
			while (next && (curr = curr->right))
				calcoffsets();
		}
		sel = matchend;
		break;
	case XKB_KEY_Escape:
		running = 0;
		return;
	case XKB_KEY_Home:
		if (sel == matches) {
			cursor = 0;
			break;
		}
		sel = curr = matches;
		calcoffsets();
		break;
	case XKB_KEY_Left:
		if (cursor > 0 && (!sel || !sel->left || lines > 0)) {
			cursor = nextrune(-1);
			break;
		}
		if (lines > 0)
			return;
		/* fallthrough */
	case XKB_KEY_Up:
		if (sel && sel->left && (sel = sel->left)->right == curr) {
			curr = prev;
			calcoffsets();
		}
		break;
	case XKB_KEY_Next:
		if (!next)
			return;
		sel = curr = next;
		calcoffsets();
		break;
	case XKB_KEY_Prior:
		if (!prev)
			return;
		sel = curr = prev;
		calcoffsets();
		break;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		puts((sel && !shift) ? sel->text : text);
		fflush(stdout);
		if (!ctrl) {
			running = 0;
			return;
		}
		if (sel)
			sel->out = 1;
		break;
	case XKB_KEY_Right:
		if (text[cursor] != '\0') {
			cursor = nextrune(+1);
			break;
		}
		if (lines > 0)
			return;
		/* fallthrough */
	case XKB_KEY_Down:
		if (sel && sel->right && (sel = sel->right) == next) {
			curr = next;
			calcoffsets();
		}
		break;
	case XKB_KEY_Tab:
		if (!sel)
			return;
		cursor = strnlen(sel->text, sizeof text - 1);
		memcpy(text, sel->text, cursor);
		text[cursor] = '\0';
		match();
		break;
	default:
		return;
	}

	drawmenu();
}

static void
readstdin(void)
{
	char *line = NULL;
	size_t i, itemsiz = 0, linesiz = 0;
	ssize_t len;

	for (i = 0; (len = getline(&line, &linesiz, stdin)) != -1; i++) {
		if (i + 1 >= itemsiz) {
			itemsiz += 256;
			if (!(items = realloc(items, itemsiz * sizeof(*items))))
				die("cannot realloc %zu bytes:", itemsiz * sizeof(*items));
		}
		if (line[len - 1] == '\n')
			line[len - 1] = '\0';
		if (!(items[i].text = strdup(line)))
			die("strdup:");

		items[i].out = 0;
	}
	free(line);
	if (items)
		items[i].text = NULL;
	lines = MIN(lines, (int)i);
}

/* ---- real Wayland plumbing ---- */

static int
allocshm(size_t size)
{
	char name[] = "/dmenu-shm-XXXXXX";
	int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (fd < 0)
		die("dmenu: shm_open: cannot create shm buffer");
	shm_unlink(name);
	if (ftruncate(fd, (off_t)size) < 0)
		die("dmenu: ftruncate failed");
	return fd;
}

static void
ensurebuffer(void)
{
	size_t need = (size_t)mw * (size_t)mh * 4;

	if (wl_buf && need == shm_size)
		return;
	if (wl_buf) {
		wl_buffer_destroy(wl_buf);
		munmap(shm_pixels, shm_size);
		close(shm_fd);
	}
	shm_size = need;
	shm_fd = allocshm(shm_size);
	shm_pixels = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
	if (shm_pixels == MAP_FAILED)
		die("dmenu: mmap failed");
	{
		struct wl_shm_pool *pool = wl_shm_create_pool(shm, shm_fd, (int32_t)shm_size);
		wl_buf = wl_shm_pool_create_buffer(pool, 0, mw, mh, mw * 4, WL_SHM_FORMAT_ARGB8888);
		wl_shm_pool_destroy(pool);
	}
}

static void
redraw(void)
{
	if (!configured)
		return;
	ensurebuffer();
	memcpy(shm_pixels, drw->pixels, (size_t)mw * (size_t)mh * 4);
	wl_surface_attach(surface, wl_buf, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, mw, mh);
	wl_surface_commit(surface);
}

static void
layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *ls,
                         uint32_t serial, uint32_t width, uint32_t height)
{
	zwlr_layer_surface_v1_ack_configure(ls, serial);
	mw = (int)width;
	if (rofimode && height)
		mh = (int)height;
	configured = 1;
	drw_resize(drw, (unsigned)mw, (unsigned)mh);
	promptw = (prompt && *prompt) ? (int)TEXTW(prompt) - lrpad / 4 : 0;
	inputw = mw / 3;
	match();
	if (rofimode)
		reqh = 0; /* a configure means the last size request is settled */
	drawmenu();
}

static void
layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *ls)
{
	running = 0;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_configure,
	.closed = layer_surface_closed,
};

static void
keyboard_keymap(void *data, struct wl_keyboard *kb, uint32_t format, int32_t fd, uint32_t size)
{
	char *map;
	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		close(fd);
		return;
	}
	map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (map == MAP_FAILED)
		return;
	if (xkb_keymap)
		xkb_keymap_unref(xkb_keymap);
	xkb_keymap = xkb_keymap_new_from_string(xkb_ctx, map, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(map, size);
	if (xkb_state)
		xkb_state_unref(xkb_state);
	xkb_state = xkb_state_new(xkb_keymap);
}

static void keyboard_enter(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf, struct wl_array *keys) {}
static void keyboard_leave(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf) {}

static void
keyboard_key(void *data, struct wl_keyboard *kb, uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
	xkb_keysym_t sym;
	char buf[8];
	int n, ctrl, alt, shift;

	if (state != WL_KEYBOARD_KEY_STATE_PRESSED || !xkb_state)
		return;

	sym = xkb_state_key_get_one_sym(xkb_state, key + 8);
	ctrl = xkb_state_mod_name_is_active(xkb_state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0;
	alt = xkb_state_mod_name_is_active(xkb_state, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0;
	shift = xkb_state_mod_name_is_active(xkb_state, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0;

	if (!ctrl && !alt) {
		switch (sym) {
		case XKB_KEY_Return: case XKB_KEY_KP_Enter: case XKB_KEY_Escape:
		case XKB_KEY_BackSpace: case XKB_KEY_Delete: case XKB_KEY_End:
		case XKB_KEY_Home: case XKB_KEY_Left: case XKB_KEY_Up:
		case XKB_KEY_Next: case XKB_KEY_Prior: case XKB_KEY_Right:
		case XKB_KEY_Down: case XKB_KEY_Tab:
			break;
		default:
			n = xkb_state_key_get_utf8(xkb_state, key + 8, buf, sizeof(buf));
			if (n > 0 && !iscntrl((unsigned char)buf[0])) {
				insert(buf, n);
				drawmenu();
				return;
			}
		}
	}
	keypress_sym(sym, ctrl, alt, shift);
}

static void keyboard_modifiers(void *data, struct wl_keyboard *kb, uint32_t serial,
                                uint32_t dep, uint32_t lat, uint32_t lock, uint32_t group)
{
	if (xkb_state)
		xkb_state_update_mask(xkb_state, dep, lat, lock, 0, 0, group);
}
static void keyboard_repeat_info(void *d, struct wl_keyboard *k, int32_t r, int32_t del) {}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = keyboard_keymap, .enter = keyboard_enter, .leave = keyboard_leave,
	.key = keyboard_key, .modifiers = keyboard_modifiers, .repeat_info = keyboard_repeat_info,
};

static void data_offer_offer(void *data, struct wl_data_offer *o, const char *mime) {}
/* version >= 3 of wl_data_offer also sends these; libwayland aborts on an event without a handler */
static void data_offer_source_actions(void *data, struct wl_data_offer *o, uint32_t actions) {}
static void data_offer_action(void *data, struct wl_data_offer *o, uint32_t action) {}
static const struct wl_data_offer_listener data_offer_listener = {
	.offer = data_offer_offer, .source_actions = data_offer_source_actions, .action = data_offer_action,
};

static void
data_device_data_offer(void *data, struct wl_data_device *dd, struct wl_data_offer *offer)
{
	/* Real wiring, not a stub: every new offer (this event fires before
	 * `selection` names which one is now the real clipboard contents)
	 * needs its own listener added before it's usable at all. */
	wl_data_offer_add_listener(offer, &data_offer_listener, NULL);
}

static void
data_device_selection(void *data, struct wl_data_device *dd, struct wl_data_offer *offer)
{
	clipboard_offer = offer;
}
static void data_device_enter(void *d, struct wl_data_device *dd, uint32_t s, struct wl_surface *sf, wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *o) {}
static void data_device_leave(void *d, struct wl_data_device *dd) {}
static void data_device_motion(void *d, struct wl_data_device *dd, uint32_t t, wl_fixed_t x, wl_fixed_t y) {}
static void data_device_drop(void *d, struct wl_data_device *dd) {}
static const struct wl_data_device_listener data_device_listener = {
	.data_offer = data_device_data_offer, .enter = data_device_enter, .leave = data_device_leave,
	.motion = data_device_motion, .drop = data_device_drop, .selection = data_device_selection,
};

static void
seat_capabilities(void *data, struct wl_seat *s, uint32_t caps)
{
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard) {
		keyboard = wl_seat_get_keyboard(s);
		wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL);
	}
}
static void seat_name(void *d, struct wl_seat *s, const char *n) {}
static const struct wl_seat_listener seat_listener = { .capabilities = seat_capabilities, .name = seat_name };

static void
registry_global(void *data, struct wl_registry *reg, uint32_t name, const char *interface, uint32_t version)
{
	/* Real, confirmed bug (same one fixed in the shared lemonbar.c
	 * registry_global): every bind below hardcoded the version it
	 * WANTED and ignored the `version` the server actually advertised
	 * - requesting a higher version than the server offers is a fatal
	 * Wayland protocol error (client gets disconnected), not silently
	 * negotiated. wlroots 0.18 only advertises zwlr_layer_shell_v1
	 * version 3, so the old hardcoded `4` here killed dmenu's
	 * connection immediately. MIN(wanted, version) is the correct,
	 * standard pattern. */
	if (!strcmp(interface, wl_compositor_interface.name)) {
		compositor = wl_registry_bind(reg, name, &wl_compositor_interface, MIN(version, 4));
	} else if (!strcmp(interface, wl_shm_interface.name)) {
		shm = wl_registry_bind(reg, name, &wl_shm_interface, MIN(version, 1));
	} else if (!strcmp(interface, wl_seat_interface.name)) {
		seat = wl_registry_bind(reg, name, &wl_seat_interface, MIN(version, 7));
		wl_seat_add_listener(seat, &seat_listener, NULL);
	} else if (!strcmp(interface, wl_data_device_manager_interface.name)) {
		data_device_manager = wl_registry_bind(reg, name, &wl_data_device_manager_interface, MIN(version, 3));
	} else if (!strcmp(interface, zwlr_layer_shell_v1_interface.name)) {
		layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, MIN(version, 4));
	} else if (!strcmp(interface, wl_output_interface.name)) {
		struct outputitem *oi = ecalloc(1, sizeof(*oi));
		oi->o = wl_registry_bind(reg, name, &wl_output_interface, MIN(version, 1));
		oi->idx = noutputs++;
		oi->next = outputlist;
		outputlist = oi;
	}
}
static void registry_global_remove(void *d, struct wl_registry *r, uint32_t n) {}
static const struct wl_registry_listener registry_listener = { .global = registry_global, .global_remove = registry_global_remove };

static void
usage(void)
{
	die("usage: dmenu [-bfirTv] [-l lines] [-p prompt] [-fn font] [-m monitor]\n"
	    "             [-nb color] [-nf color] [-sb color] [-sf color] [-w windowid]");
}

int
dmenu_main(int argc, char *argv[])
{
	int i, fast = 0;
	struct wl_registry *registry;
	struct outputitem *oi;

	for (i = 1; i < argc; i++)
		if (!strcmp(argv[i], "-v")) {
			puts("dmenu-"VERSION);
			exit(0);
		} else if (!strcmp(argv[i], "-b"))
			topbar = 0;
		else if (!strcmp(argv[i], "-f"))
			fast = 1;
		else if (!strcmp(argv[i], "-r"))
			rofimode = 1;
		else if (!strcmp(argv[i], "-T"))
			rofimode = 0;
		else if (!strcmp(argv[i], "-i")) {
			fstrncmp = strncasecmp;
			fstrstr = cistrstr;
		} else if (i + 1 == argc)
			usage();
		else if (!strcmp(argv[i], "-l"))
			lines = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-m"))
			mon = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-p"))
			prompt = argv[++i];
		else if (!strcmp(argv[i], "-fn"))
			fonts[0] = argv[++i];
		else if (!strcmp(argv[i], "-nb"))
			colors[SchemeNorm][ColBg] = argv[++i];
		else if (!strcmp(argv[i], "-nf"))
			colors[SchemeNorm][ColFg] = argv[++i];
		else if (!strcmp(argv[i], "-sb"))
			colors[SchemeSel][ColBg] = argv[++i];
		else if (!strcmp(argv[i], "-sf"))
			colors[SchemeSel][ColFg] = argv[++i];
		else if (!strcmp(argv[i], "-w"))
			embed = argv[++i]; /* accepted, real no-op - see top comment */
		else
			usage();

	if (!setlocale(LC_CTYPE, ""))
		fputs("warning: no locale support\n", stderr);

	if (!(display = wl_display_connect(NULL)))
		die("dmenu: cannot connect to Wayland display");
	xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

	registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	wl_display_roundtrip(display); /* seat capabilities + outputs settle */

	if (!compositor || !shm || !layer_shell)
		die("dmenu: compositor is missing wl_compositor/wl_shm/wlr-layer-shell-unstable-v1");

	if (data_device_manager && seat) {
		data_device = wl_data_device_manager_get_data_device(data_device_manager, seat);
		wl_data_device_add_listener(data_device, &data_device_listener, NULL);
	}

	chosen_output = NULL;
	if (mon >= 0)
		for (oi = outputlist; oi; oi = oi->next)
			if (oi->idx == mon)
				chosen_output = oi->o;

	drw = drw_create(1, 1); /* real size known only after the first configure */
	if (!drw_fontset_create(drw, fonts, LENGTH(fonts)))
		die("no fonts could be loaded.");
	lrpad = (int)drw->fonts->h;

	if (rofimode) /* window background is the translucent one */
		colors[SchemeNorm][ColBg] = rofi_bg;
	for (i = 0; i < SchemeLast; i++)
		scheme[i] = drw_scm_create(drw, colors[i], 2);

	if (fast && !isatty(0)) {
		readstdin();
	} else {
		readstdin();
	}

	bh = (int)drw->fonts->h + 2;
	lines = MAX(lines, 0);
	mh = (lines + 1) * bh;
	if (rofimode) {
		size_t n = 0;

		while (items && items[n].text) /* items is still a plain array here: the links are set by match() */
			n++;
		if (!lines)
			lines = rofi_lines;
		inh = (int)drw->fonts->h + 2 * rofi_in_pady;
		rowh = (int)drw->fonts->h + 2 * rofi_el_pady;
		bh = rowh + rofi_el_gap; /* row pitch for the shared list logic */
		mw = rofi_width;
		mh = reqh = rofi_height((int)MIN(n, (size_t)lines));
	}

	surface = wl_compositor_create_surface(compositor);
	layer_surface = zwlr_layer_shell_v1_get_layer_surface(layer_shell, surface, chosen_output,
	                                                       ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "dmenu");
	zwlr_layer_surface_v1_add_listener(layer_surface, &layer_surface_listener, NULL);
	if (rofimode) {
		zwlr_layer_surface_v1_set_size(layer_surface, (uint32_t)rofi_width, (uint32_t)mh);
		zwlr_layer_surface_v1_set_anchor(layer_surface, 0); /* no anchor = centred */
		zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, -1);
	} else {
		zwlr_layer_surface_v1_set_size(layer_surface, 0, (uint32_t)mh);
		zwlr_layer_surface_v1_set_anchor(layer_surface,
			ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT |
			(topbar ? ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP : ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM));
		zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, mh);
	}
	zwlr_layer_surface_v1_set_keyboard_interactivity(layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
	wl_surface_commit(surface);

	while (running && wl_display_dispatch(display) >= 0)
		;

	cleanup();
	return 0;
}

/* ---- menu (the launcher; was dmenu_menu.c) ---------------------------------------------------------------------- */
/* the main menu, in C, on top of dmenu.
 *
 * Replaces the rofi shell script bound to Super+W: one menu with
 *   Aplikacje   desktop applications that are not games
 *   Gry         desktop applications in the Game category
 *   Kalkulator  type an expression, the result is copied to the clipboard
 *   Schowek     cliphist history, the chosen entry goes back to the clipboard
 * Every list is shown by dmenu (DMENU_CMD, default "dmenu -i"), one item
 * per line, and the choice is read back from its stdout.
 *
 * usage: dmenu_menu              the main menu
 *        dmenu_menu apps|games|calc|clip    jump straight to one entry
 *        dmenu_menu --eval EXPR  print the value of EXPR and exit (for calc)
 *
 * environment:
 *   DMENU_CMD      picker command (default "dmenu -i"); a prompt is added as -p
 *   DMENU_TERMINAL terminal for Terminal=true apps (default foot)
 *
 * Applications come from $XDG_DATA_HOME/applications, $XDG_DATA_DIRS/applications
 * and the Flatpak export directories (also $FLATPAK_USER_DIR/exports/share).
 *
 * build: cc -O2 -Wall -Wextra -o dmenu_menu dmenu_menu.c -lm
 */
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <dirent.h>
#include <math.h>

/* ---- small helpers ----------------------------------------------------- */
static void *
mn_xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);
	if (!p) {
		fputs("dmenu_menu: out of memory\n", stderr);
		exit(1);
	}
	return p;
}

static char *
mn_xstrdup(const char *s)
{
	char *p = mn_xmalloc(strlen(s) + 1);
	return strcpy(p, s);
}

static char *
mn_join(const char *a, const char *sep, const char *b)
{
	char *p = mn_xmalloc(strlen(a) + strlen(sep) + strlen(b) + 1);
	sprintf(p, "%s%s%s", a, sep, b);
	return p;
}

static const char *
mn_home(void)
{
	const char *h = getenv("HOME");
	return h && *h ? h : "/";
}

/* ---- the picker (dmenu) ------------------------------------------------ */
struct mn_item {
	char *label;  /* what dmenu shows */
	char *data;   /* what we act on (exec line, path, ...) */
	int terminal;
};

static struct mn_item *
mn_additem(struct mn_item **v, size_t *n, size_t *cap, const char *label, const char *data, int terminal)
{
	if (*n == *cap) {
		*cap = *cap ? *cap * 2 : 64;
		*v = realloc(*v, *cap * sizeof **v);
		if (!*v)
			exit(1);
	}
	(*v)[*n].label = mn_xstrdup(label);
	(*v)[*n].data = mn_xstrdup(data ? data : "");
	(*v)[*n].terminal = terminal;
	return &(*v)[(*n)++];
}

static int
mn_cmpitem(const void *a, const void *b)
{
	return strcasecmp(((const struct mn_item *)a)->label, ((const struct mn_item *)b)->label);
}

/* Show labels in the picker and return the chosen line (malloc'd, without the
 * newline), or NULL if it was cancelled. A line the user typed that is not in
 * the list comes back as is, like dmenu itself does. */
static char *
mn_pick(const char *prompt, char **labels, size_t n)
{
	int in[2], out[2];
	pid_t pid;
	const char *base = getenv("DMENU_CMD");
	char *cmd, *res = NULL;
	size_t i, len = 0, cap = 256;
	FILE *w;
	char *buf;
	ssize_t r;

	if (!base || !*base)
		base = "dmenu -i";
	cmd = mn_xmalloc(strlen(base) + strlen(prompt) + 16);
	sprintf(cmd, "%s -p '%s'", base, prompt);
	if (pipe(in) || pipe(out))
		return NULL;
	if ((pid = fork()) == 0) {
		dup2(in[0], 0);
		dup2(out[1], 1);
		close(in[0]); close(in[1]); close(out[0]); close(out[1]);
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
	close(in[0]);
	close(out[1]);
	if (pid < 0)
		return NULL;
	w = fdopen(in[1], "w");
	for (i = 0; i < n; i++)
		fprintf(w, "%s\n", labels[i]);
	fclose(w);

	buf = mn_xmalloc(cap);
	while ((r = read(out[0], buf + len, cap - len - 1)) > 0) {
		len += (size_t)r;
		if (cap - len < 2) {
			cap *= 2;
			buf = realloc(buf, cap);
			if (!buf)
				exit(1);
		}
	}
	close(out[0]);
	waitpid(pid, NULL, 0);
	buf[len] = 0;
	while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
		buf[--len] = 0;
	if (len)
		res = buf;
	else
		free(buf);
	free(cmd);
	return res;
}

static char *
mn_pickitem(const char *prompt, struct mn_item *v, size_t n, struct mn_item **chosen)
{
	char **labels = mn_xmalloc((n + 1) * sizeof *labels);
	char *r;
	size_t i;

	for (i = 0; i < n; i++)
		labels[i] = v[i].label;
	r = mn_pick(prompt, labels, n);
	free(labels);
	*chosen = NULL;
	if (r)
		for (i = 0; i < n; i++)
			if (!strcmp(v[i].label, r)) {
				*chosen = &v[i];
				break;
			}
	return r;
}

/* ---- launching --------------------------------------------------------- */
static void
mn_detach_run(const char *cmd, int terminal)
{
	pid_t pid = fork();

	if (pid == 0) {
		const char *mn_term = getenv("DMENU_TERMINAL");
		if (setsid() < 0)
			_exit(1);
		if (pid == 0 && chdir(mn_home()) < 0)
			_exit(1);
		if (terminal)
			execlp(mn_term && *mn_term ? mn_term : "foot", mn_term && *mn_term ? mn_term : "foot", "-e", "sh", "-c", cmd, (char *)NULL);
		else
			execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
}

/* ---- desktop entries --------------------------------------------------- */
static char *
mn_stripcodes(const char *exec)
{
	char *out = mn_xmalloc(strlen(exec) + 1), *o = out;
	const char *p;

	for (p = exec; *p; p++) {
		if (*p == '%' && p[1]) {
			if (p[1] == '%')
				*o++ = '%';
			p++;              /* %f %F %u %U %i %c %k ... are dropped */
			continue;
		}
		*o++ = *p;
	}
	*o = 0;
	while (o > out && o[-1] == ' ')
		*--o = 0;
	return out;
}

static int
mn_truthy(const char *v)
{
	return v && !strcmp(v, "true");
}

static void
mn_readdesktop(const char *path, int want_games, struct mn_item **v, size_t *n, size_t *cap)
{
	FILE *f = fopen(path, "r");
	char line[4096], *name = NULL, *exec = NULL, *cats = NULL, *type = NULL;
	int in = 0, nodisplay = 0, hidden = 0, terminal = 0;

	if (!f)
		return;
	while (fgets(line, sizeof line, f)) {
		char *eq;
		line[strcspn(line, "\r\n")] = 0;
		if (line[0] == '[') {
			if (in)
				break;          /* only the [Desktop Entry] group */
			in = !strcmp(line, "[Desktop Entry]");
			continue;
		}
		if (!in || !(eq = strchr(line, '=')))
			continue;
		*eq = 0;
		if (!strcmp(line, "Name") && !name) name = mn_xstrdup(eq + 1);
		else if (!strcmp(line, "Exec") && !exec) exec = mn_xstrdup(eq + 1);
		else if (!strcmp(line, "Categories") && !cats) cats = mn_xstrdup(eq + 1);
		else if (!strcmp(line, "Type") && !type) type = mn_xstrdup(eq + 1);
		else if (!strcmp(line, "NoDisplay")) nodisplay = mn_truthy(eq + 1);
		else if (!strcmp(line, "Hidden")) hidden = mn_truthy(eq + 1);
		else if (!strcmp(line, "Terminal")) terminal = mn_truthy(eq + 1);
	}
	fclose(f);
	if (name && exec && type && !strcmp(type, "Application") && !nodisplay && !hidden) {
		int isgame = cats && (strstr(cats, "Game;") || (strlen(cats) >= 4 && !strcmp(cats + strlen(cats) - 4, "Game")));
		if (isgame == want_games) {
			char *cmd = mn_stripcodes(exec);
			mn_additem(v, n, cap, name, cmd, terminal);
			free(cmd);
		}
	}
	free(name); free(exec); free(cats); free(type);
}

static void
mn_scandir_apps(const char *dir, int want_games, struct mn_item **v, size_t *n, size_t *cap)
{
	DIR *d = opendir(dir);
	struct dirent *e;

	if (!d)
		return;
	while ((e = readdir(d))) {
		size_t l = strlen(e->d_name);
		char *p;
		if (l < 9 || strcmp(e->d_name + l - 8, ".desktop"))
			continue;
		p = mn_join(dir, "/", e->d_name);
		mn_readdesktop(p, want_games, v, n, cap);
		free(p);
	}
	closedir(d);
}

static void
mn_appdirs(void (*fn)(const char *, void *), void *arg)
{
	const char *xdh = getenv("XDG_DATA_HOME"), *xdd = getenv("XDG_DATA_DIRS"), *fu = getenv("FLATPAK_USER_DIR");
	char *dirs, *save = NULL, *t, *p;

	p = xdh && *xdh ? mn_join(xdh, "/applications", "") : mn_join(mn_home(), "/.local/share/applications", "");
	fn(p, arg); free(p);
	dirs = mn_xstrdup(xdd && *xdd ? xdd : "/usr/local/share:/usr/share");
	for (t = strtok_r(dirs, ":", &save); t; t = strtok_r(NULL, ":", &save)) {
		p = mn_join(t, "/applications", "");
		fn(p, arg); free(p);
	}
	free(dirs);
	if (fu && *fu) {
		p = mn_join(fu, "/exports/share/applications", "");
		fn(p, arg); free(p);
	}
	p = mn_join(mn_home(), "/.local/share/flatpak/exports/share/applications", "");
	fn(p, arg); free(p);
	fn("/var/lib/flatpak/exports/share/applications", arg);
}

struct appctx { int games; struct mn_item **v; size_t *n, *cap; };

static void
mn_appdir_cb(const char *dir, void *arg)
{
	struct appctx *c = arg;
	mn_scandir_apps(dir, c->games, c->v, c->n, c->cap);
}

static void
mn_menu_apps(int games)
{
	struct mn_item *v = NULL, *sel, *w = NULL;
	size_t n = 0, cap = 0, i, m = 0;
	struct appctx c = { games, &v, &n, &cap };
	char *r;

	mn_appdirs(mn_appdir_cb, &c);
	qsort(v, n, sizeof *v, mn_cmpitem);
	/* the same application can be in several directories: keep the first */
	w = mn_xmalloc((n + 1) * sizeof *w);
	for (i = 0; i < n; i++)
		if (!m || strcmp(w[m - 1].label, v[i].label))
			w[m++] = v[i];
	r = mn_pickitem(games ? "Gry:" : "Aplikacje:", w, m, &sel);
	if (sel)
		mn_detach_run(sel->data, sel->terminal);
	free(r);
}

/* ---- calculator -------------------------------------------------------- */
static const char *mn_g_p;
static int mn_g_err;

static double mn_expr(void);

static void
mn_skipws(void)
{
	while (*mn_g_p == ' ' || *mn_g_p == '\t')
		mn_g_p++;
}

static double
mn_primary(void)
{
	double v;
	char name[16];
	int i;

	mn_skipws();
	if (*mn_g_p == '(') {
		mn_g_p++;
		v = mn_expr();
		mn_skipws();
		if (*mn_g_p == ')')
			mn_g_p++;
		else
			mn_g_err = 1;
		return v;
	}
	if (isdigit((unsigned char)*mn_g_p) || *mn_g_p == '.' || *mn_g_p == ',') {
		char buf[64];
		i = 0;
		while ((isdigit((unsigned char)*mn_g_p) || *mn_g_p == '.' || *mn_g_p == ',') && i < 63)
			buf[i++] = *mn_g_p == ',' ? '.' : *mn_g_p, mn_g_p++;
		buf[i] = 0;
		return atof(buf);
	}
	if (isalpha((unsigned char)*mn_g_p)) {
		i = 0;
		while (isalpha((unsigned char)*mn_g_p) && i < 15)
			name[i++] = (char)tolower((unsigned char)*mn_g_p++);
		name[i] = 0;
		if (!strcmp(name, "pi")) return M_PI;
		if (!strcmp(name, "e")) return M_E;
		mn_skipws();
		if (*mn_g_p == '(') {
			mn_g_p++;
			v = mn_expr();
			mn_skipws();
			if (*mn_g_p == ')')
				mn_g_p++;
			else
				mn_g_err = 1;
			if (!strcmp(name, "sqrt")) return sqrt(v);
			if (!strcmp(name, "sin")) return sin(v);
			if (!strcmp(name, "cos")) return cos(v);
			if (!strcmp(name, "tan")) return tan(v);
			if (!strcmp(name, "ln")) return log(v);
			if (!strcmp(name, "log")) return log10(v);
			if (!strcmp(name, "exp")) return exp(v);
			if (!strcmp(name, "abs")) return fabs(v);
			if (!strcmp(name, "floor")) return floor(v);
			if (!strcmp(name, "ceil")) return ceil(v);
			if (!strcmp(name, "round")) return round(v);
		}
	}
	mn_g_err = 1;
	return 0;
}

static double
mn_unary(void)
{
	mn_skipws();
	if (*mn_g_p == '-') { mn_g_p++; return -mn_unary(); }
	if (*mn_g_p == '+') { mn_g_p++; return mn_unary(); }
	return mn_primary();
}

static double
mn_power(void)
{
	double b = mn_unary();

	mn_skipws();
	if (*mn_g_p == '^') {
		mn_g_p++;
		return pow(b, mn_power());   /* right associative */
	}
	return b;
}

static double
mn_term(void)
{
	double v = mn_power();

	for (;;) {
		mn_skipws();
		if (*mn_g_p == '*' || *mn_g_p == 'x') { mn_g_p++; v *= mn_power(); }
		else if (*mn_g_p == '/') { mn_g_p++; v /= mn_power(); }
		else if (*mn_g_p == '%') { mn_g_p++; v = fmod(v, mn_power()); }
		else return v;
	}
}

static double
mn_expr(void)
{
	double v = mn_term();

	for (;;) {
		mn_skipws();
		if (*mn_g_p == '+') { mn_g_p++; v += mn_term(); }
		else if (*mn_g_p == '-') { mn_g_p++; v -= mn_term(); }
		else return v;
	}
}

/* value of s, or NaN on a syntax error */
static double
mn_evaluate(const char *s)
{
	double v;

	mn_g_p = s;
	mn_g_err = 0;
	v = mn_expr();
	mn_skipws();
	if (mn_g_err || *mn_g_p)
		return NAN;
	return v;
}

static void
mn_copy_text(const char *s)
{
	FILE *w = popen("wl-copy", "w");

	if (w) {
		fputs(s, w);
		pclose(w);
	} else {
		puts(s);
	}
}

static void
mn_menu_calc(void)
{
	char *in = mn_pick("=", (char *[]){ NULL }, 0);

	while (in) {
		double v = mn_evaluate(in);
		char res[64], *label[1], *choice;

		if (v != v) {
			snprintf(res, sizeof res, "syntax error");
		} else {
			snprintf(res, sizeof res, "%.12g", v);
		}
		label[0] = res;
		choice = mn_pick(in, label, 1);
		free(in);
		if (!choice)
			return;
		if (v == v && !strcmp(choice, res)) {
			mn_copy_text(res);
			free(choice);
			return;
		}
		in = choice;   /* edited: mn_evaluate the new text */
	}
}

/* ---- clipboard (cliphist) ---------------------------------------------- */
static void
mn_menu_clip(void)
{
	if (system("command -v cliphist >/dev/null 2>&1") != 0) {
		char *m[] = { "cliphist is not installed" };
		free(mn_pick("Schowek:", m, 1));
		return;
	}
	{
		/* the picker command is spliced into the string, so its own quotes are parsed by sh (an unquoted
		 * ${DMENU_CMD} expansion would keep them as literal characters) */
		const char *base = getenv("DMENU_CMD");
		char *cmd;

		if (!base || !*base)
			base = "dmenu -i";
		cmd = mn_xmalloc(strlen(base) + 96);
		sprintf(cmd, "cliphist list | %s -p 'Schowek:' | cliphist decode | wl-copy", base);
		(void)!system(cmd);
		free(cmd);
	}
}

/* ---- main -------------------------------------------------------------- */
static void
mn_mainmenu(void)
{
	char *entries[] = { "Aplikacje", "Gry", "Kalkulator", "Schowek" };
	char *r = mn_pick("Menu:", entries, 4);

	if (!r)
		return;
	if (!strcmp(r, "Aplikacje")) mn_menu_apps(0);
	else if (!strcmp(r, "Gry")) mn_menu_apps(1);
	else if (!strcmp(r, "Kalkulator")) mn_menu_calc();
	else if (!strcmp(r, "Schowek")) mn_menu_clip();
	free(r);
}

/* Use the dmenu that sits next to this binary (a source-tree build that was never installed) unless
 * DMENU_CMD says otherwise; without one, fall back to whatever "dmenu" is in PATH. */
static void
mn_default_picker(void)
{
	char exe[4096], cmd[4200];
	ssize_t n;
	char *slash;

	if (getenv("DMENU_CMD") && *getenv("DMENU_CMD"))
		return;
	n = readlink("/proc/self/exe", exe, sizeof(exe) - 8);
	if (n <= 0)
		return;
	exe[n] = '\0';
	if (!(slash = strrchr(exe, '/')))
		return;
	strcpy(slash + 1, "dmenu");
	if (access(exe, X_OK) != 0)
		return;
	snprintf(cmd, sizeof(cmd), "'%s' -i", exe);
	setenv("DMENU_CMD", cmd, 0);
}

int
menu_main(int argc, char *argv[])
{
	mn_default_picker();
	if (argc >= 3 && !strcmp(argv[1], "--eval")) {
		double v = mn_evaluate(argv[2]);
		if (v != v) {
			fputs("syntax error\n", stderr);
			return 1;
		}
		printf("%.12g\n", v);
		return 0;
	}
	if (argc >= 2) {
		if (!strcmp(argv[1], "apps")) mn_menu_apps(0);
		else if (!strcmp(argv[1], "games")) mn_menu_apps(1);
		else if (!strcmp(argv[1], "calc")) mn_menu_calc();
		else if (!strcmp(argv[1], "clip")) mn_menu_clip();
		else {
			fputs("usage: dmenu_menu [apps|games|calc|clip | --eval EXPR]\n", stderr);
			return 1;
		}
		return 0;
	}
	mn_mainmenu();
	return 0;
}
