/* See LICENSE file for copyright and license details. */
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

#include "config.h"

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
main(int argc, char *argv[])
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
