/* panel.c - `desktop settings`: the settings panel. A dmenu-style layer-shell window (same drw.c, same colours as dmenu) that
 * lists every setting the compositor has, scrolls, and edits them. Nothing is hardcoded per setting: the rows come from the
 * compositor's `schema` command (api.c: its registry plus ~/.config/desktop/settings.schema), one line each:
 *     key|type|extra|apply|default|current|label        type: bool number text choice color
 * A change goes to the compositor as `set key value` (live where apply says live), and the values that differ from their
 * default are written to ~/.config/desktop/settings.conf, which the compositor reads at start and on `reload`.
 * Keys: Tab / Down next field, Shift-Tab / Up previous, PgUp PgDn Home End, Space or Enter toggle (bool) / cycle (choice) /
 * edit (number, text, colour), Left Right step a number or cycle a choice, Esc closes (cancels an edit first). Mouse: wheel
 * scrolls, click selects, click on a value acts on it.
 * `desktop settings volume` is the small volume panel (speaker + microphone bars), `... volume --osd` the same as a keyboardless
 * on-screen display that closes itself after the volume_osd_ms setting; both talk to the compositor's `volume` command (audio.c).
 * Test hooks (no real keyboard needed): DESKTOP_PANEL_KEYS="Tab space Down ..." feeds key names after the first draw,
 * DESKTOP_PANEL_DUMP=1 prints the state to stdout after that. */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <math.h>
#include <poll.h>
#include <stdarg.h>
#include <time.h>
#include <sys/file.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "wlr-layer-shell-unstable-v1-protocol.h"

#include "drw.h"
#include "util.h"

enum { SNorm, SSel, SDim, SErr, SAcc, SLast };

struct field {
	char key[48], type[12], extra[96], apply[12], def[160], cur[160], label[96];
	double lo, hi, step;
};

#define MAXF 256
static struct field F[MAXF];
static int nf, sel, top, editing, ncfg;
static char ebuf[160], status[200];
static int status_err;

static Drw *drw;
static Clr *scheme[SLast];
static int pw = 640, ph = 520, lrpad, configured, running = 1, fh;
static int px, py; /* pointer position */

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct wl_keyboard *keyboard;
static struct wl_pointer *pointer;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct zwlr_layer_surface_v1 *layer_surface;
static struct wl_surface *surface;
static struct wl_buffer *wl_buf;
static void *shm_pixels;
static int shm_fd = -1;
static size_t shm_size;
static struct xkb_context *xkb_ctx;
static struct xkb_keymap *xkb_keymap;
static struct xkb_state *xkb_state;

/* ---- look (the dmenu rofi look) */
static const char *colors[SLast][2] = {
	[SNorm] = { "#00ff88", "#000000e6" },
	[SSel]  = { "#000000", "#00ff88" },
	[SDim]  = { "#778877", "#000000e6" },
	[SErr]  = { "#ff5555", "#000000e6" },
	[SAcc]  = { "#00ff88", "#000000" },
};
static const char *fonts[] = { "TempleOS Bitmap:pixelsize=16", "Fixed6x13:pixelsize=13", "TempleOS:size=14" };
static const int border = 2, pad = 14, rowpadx = 10, rowpady = 6;

/* ---- compositor socket */
static char *
ctl(const char *line)
{
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	const char *rt = getenv("XDG_RUNTIME_DIR");
	char *out = NULL, buf[4096];
	size_t len = 0;
	ssize_t r;
	int fd;
	if (!rt) return NULL;
	snprintf(a.sun_path, sizeof a.sun_path, "%s/dwl.sock", rt);
	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) return NULL;
	if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return NULL; }
	if (write(fd, line, strlen(line)) < 0 || write(fd, "\n", 1) < 0) { close(fd); return NULL; }
	while ((r = read(fd, buf, sizeof buf)) > 0) {
		out = realloc(out, len + (size_t)r + 1);
		memcpy(out + len, buf, (size_t)r);
		len += (size_t)r;
		out[len] = 0;
	}
	close(fd);
	return out ? out : strdup("");
}

static void
parse_range(struct field *f)
{
	char *e;
	f->lo = -1e9; f->hi = 1e9; f->step = 1;
	if (!f->extra[0]) return;
	f->lo = strtod(f->extra, &e);
	if (*e == ':') {
		f->hi = strtod(e + 1, &e);
		if (*e == ':') f->step = strtod(e + 1, NULL);
		else if (f->lo != floor(f->lo) || f->hi != floor(f->hi)) f->step = 0.05;
	}
}

static int
load_schema(void)
{
	char *r = ctl("schema"), *p, *nl;
	if (!r) return -1;
	for (p = r; *p && nf < MAXF; p = nl ? nl + 1 : p + strlen(p)) {
		char *fld[7] = {0};
		int n = 0;
		nl = strchr(p, '\n');
		if (nl) *nl = 0;
		if (!*p) continue;
		if (!strncmp(p, "error:", 6)) break;
		for (fld[n++] = p; n < 7 && (p = strchr(p, '|')); fld[n++] = ++p) *p = 0;
		if (n < 7) continue;
		snprintf(F[nf].key, sizeof F[nf].key, "%s", fld[0]);
		snprintf(F[nf].type, sizeof F[nf].type, "%s", fld[1]);
		snprintf(F[nf].extra, sizeof F[nf].extra, "%s", fld[2]);
		snprintf(F[nf].apply, sizeof F[nf].apply, "%s", fld[3]);
		snprintf(F[nf].def, sizeof F[nf].def, "%s", fld[4]);
		snprintf(F[nf].cur, sizeof F[nf].cur, "%s", fld[5]);
		snprintf(F[nf].label, sizeof F[nf].label, "%s", fld[6]);
		parse_range(&F[nf]);
		nf++;
	}
	free(r);
	return nf ? 0 : -1;
}

/* ---- persistence: only the values that differ from their default */
static void
persist(void)
{
	char dir[512], path[600], tmp[620];
	const char *e = getenv("DESKTOP_CONFIG_DIR"), *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
	FILE *f;
	if (e && *e) snprintf(dir, sizeof dir, "%s", e);
	else if (x && *x) snprintf(dir, sizeof dir, "%s/desktop", x);
	else snprintf(dir, sizeof dir, "%s/.config/desktop", h ? h : "");
	{ char d[520]; snprintf(d, sizeof d, "%s", dir); for (char *p = d + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(d, 0755); *p = '/'; } mkdir(d, 0755); }
	snprintf(path, sizeof path, "%s/settings.conf", dir);
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	if (!(f = fopen(tmp, "w"))) { snprintf(status, sizeof status, "cannot write %s", path); status_err = 1; return; }
	fputs("# written by `desktop settings`; key = value, only what differs from the default. Read at start and on `reload`.\n", f);
	for (int i = 0; i < nf; i++)
		if (strcmp(F[i].cur, F[i].def)) fprintf(f, "%s = %s\n", F[i].key, F[i].cur);
	fclose(f);
	if (rename(tmp, path)) { snprintf(status, sizeof status, "cannot write %s", path); status_err = 1; }
}

static void
commit(int i, const char *val)
{
	char req[400], *r;
	struct field *f = &F[i];
	snprintf(req, sizeof req, "set %s %s", f->key, val);
	r = ctl(req);
	if (!r) { snprintf(status, sizeof status, "compositor not reachable"); status_err = 1; return; }
	if (strncmp(r, "ok", 2)) {
		r[strcspn(r, "\n")] = 0;
		snprintf(status, sizeof status, "%s", r + (!strncmp(r, "error: ", 7) ? 7 : 0));
		status_err = 1;
	} else {
		snprintf(f->cur, sizeof f->cur, "%s", val);
		status_err = 0;
		snprintf(status, sizeof status, "%s = %s%s", f->key, val,
			!strcmp(f->apply, "restart") ? "   (needs restart)" : !strcmp(f->apply, "new") ? "   (new outputs only)" : "");
		persist();
	}
	free(r);
}

/* ---- field actions */
static void
step_number(int i, int dir)
{
	struct field *f = &F[i];
	double v = strtod(f->cur, NULL) + dir * (f->step > 0 ? f->step : 1);
	char b[48];
	if (v < f->lo) v = f->lo;
	if (v > f->hi) v = f->hi;
	snprintf(b, sizeof b, "%.6g", v);
	if (strcmp(b, f->cur)) commit(i, b);
}

static void
cycle_choice(int i, int dir)
{
	struct field *f = &F[i];
	char *opts[32], x[96], *p;
	int n = 0, cur = 0;
	snprintf(x, sizeof x, "%s", f->extra);
	for (p = strtok(x, ","); p && n < 32; p = strtok(NULL, ",")) { if (!strcmp(p, f->cur)) cur = n; opts[n++] = p; }
	if (n) commit(i, opts[(cur + dir + n) % n]);
}

static void
toggle_bool(int i)
{
	commit(i, !strcmp(F[i].cur, "1") ? "0" : "1");
}

static void
begin_edit(int i)
{
	snprintf(ebuf, sizeof ebuf, "%s", F[i].cur);
	editing = 1;
}

static void
activate(int i)
{
	const char *t = F[i].type;
	if (!strcmp(t, "bool")) toggle_bool(i);
	else if (!strcmp(t, "choice")) cycle_choice(i, 1);
	else begin_edit(i);
}

static int
visible_rows(void)
{
	int listh = ph - 2 * (border + pad) - 2 * (fh + 2 * rowpady) - 8;
	return MAX(1, listh / (fh + 2 * rowpady));
}

static void
scroll_to_sel(void)
{
	int v = visible_rows();
	if (sel < top) top = sel;
	if (sel >= top + v) top = sel - v + 1;
	if (top > nf - v) top = MAX(0, nf - v);
}

static void
draw(void);
static void blit(void);
static void vol_paint(void);
static int volmode, osdmode;

/* one key, from the real keyboard or from the test hook */
static void
handle(xkb_keysym_t sym, const char *utf8, int ctrl)
{
	if (editing) {
		size_t l = strlen(ebuf);
		if (sym == XKB_KEY_Escape) editing = 0;
		else if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) { editing = 0; commit(sel, ebuf); }
		else if (sym == XKB_KEY_BackSpace) { while (l && (ebuf[l - 1] & 0xC0) == 0x80) l--; if (l) ebuf[l - 1] = 0; }
		else if (ctrl && (sym == XKB_KEY_u || sym == XKB_KEY_U)) ebuf[0] = 0;
		else if (utf8 && utf8[0] && !iscntrl((unsigned char)utf8[0]) && l + strlen(utf8) < sizeof ebuf) strcat(ebuf, utf8);
		return;
	}
	switch (sym) {
	case XKB_KEY_Escape: case XKB_KEY_q: running = 0; return;
	case XKB_KEY_Tab: case XKB_KEY_Down: case XKB_KEY_j: sel = (sel + 1) % nf; break;
	case XKB_KEY_ISO_Left_Tab: case XKB_KEY_Up: case XKB_KEY_k: sel = (sel + nf - 1) % nf; break;
	case XKB_KEY_Next: sel = MIN(nf - 1, sel + visible_rows()); break;
	case XKB_KEY_Prior: sel = MAX(0, sel - visible_rows()); break;
	case XKB_KEY_Home: sel = 0; break;
	case XKB_KEY_End: sel = nf - 1; break;
	case XKB_KEY_space: case XKB_KEY_Return: case XKB_KEY_KP_Enter: activate(sel); break;
	case XKB_KEY_Left: case XKB_KEY_Right: {
		int d = sym == XKB_KEY_Right ? 1 : -1;
		if (!strcmp(F[sel].type, "number")) step_number(sel, d);
		else if (!strcmp(F[sel].type, "choice")) cycle_choice(sel, d);
		else if (!strcmp(F[sel].type, "bool")) toggle_bool(sel);
		break;
	}
	default: return;
	}
	scroll_to_sel();
}

/* ---- drawing */
static void
draw(void)
{
	int x0 = border + pad, w0 = pw - 2 * x0, v = visible_rows(), rowh = fh + 2 * rowpady, y, labw = w0 * 55 / 100, i;
	char buf[300];
	if (!configured) return;
	if (volmode) { vol_paint(); blit(); return; }
	drw_setscheme(drw, scheme[SNorm]);
	drw_rect(drw, 0, 0, (unsigned)pw, (unsigned)ph, 1, 1);
	drw_setscheme(drw, scheme[SAcc]);
	for (y = 0; y < border; y++) drw_rect(drw, y, y, (unsigned)(pw - 2 * y), (unsigned)(ph - 2 * y), 0, 0);

	snprintf(buf, sizeof buf, "Settings  %d/%d", sel + 1, nf);
	drw_setscheme(drw, scheme[SAcc]);
	drw_text(drw, x0, x0, (unsigned)w0, (unsigned)rowh, (unsigned)rowpadx, buf, 0);
	y = x0 + rowh + 4;
	for (i = top; i < nf && i < top + v; i++, y += rowh) {
		struct field *f = &F[i];
		int issel = i == sel, vx = x0 + labw, vw = w0 - labw - 6;
		drw_setscheme(drw, scheme[issel ? SSel : SNorm]);
		drw_text(drw, x0, y, (unsigned)w0, (unsigned)rowh, (unsigned)rowpadx, f->label[0] ? f->label : f->key, 0);
		if (!strcmp(f->type, "bool")) snprintf(buf, sizeof buf, "[%c]", !strcmp(f->cur, "1") ? 'x' : ' ');
		else if (!strcmp(f->type, "number") || !strcmp(f->type, "choice")) snprintf(buf, sizeof buf, "< %s >", f->cur);
		else snprintf(buf, sizeof buf, "%s", f->cur);
		if (issel && editing) snprintf(buf, sizeof buf, "%s_", ebuf);
		if (!strcmp(f->type, "color") && !(issel && editing)) {
			unsigned long c = f->cur[0] == '#' ? strtoul(f->cur + 1, NULL, 16) : 0;
			unsigned rgb = strlen(f->cur) == 9 ? (unsigned)(c >> 8) : (unsigned)c;
			Clr sw[2] = { 0xff000000u | rgb, 0xff000000u | rgb };
			Clr *old = drw->scheme;
			drw_setscheme(drw, sw);
			drw_rect(drw, vx, y + 3, (unsigned)(rowh - 6), (unsigned)(rowh - 6), 1, 0);
			drw_setscheme(drw, old);
			drw_text(drw, vx + rowh, y, (unsigned)(vw - rowh), (unsigned)rowh, 0, buf, 0);
		} else
			drw_text(drw, vx, y, (unsigned)vw, (unsigned)rowh, 4, buf, 0);
		if (strcmp(f->apply, "live")) { /* tell the user it will not apply now */
			drw_setscheme(drw, scheme[issel ? SSel : SDim]);
			const char *tag = !strcmp(f->apply, "restart") ? "restart" : "new";
			int tw = (int)drw_fontset_getwidth(drw, tag) + 12;
			drw_text(drw, x0 + w0 - tw - 8, y, (unsigned)tw, (unsigned)rowh, 6, tag, 0);
		}
	}
	if (nf > v) { /* scrollbar */
		int th = ph - 2 * x0 - rowh * 2 - 8, bh = MAX(10, th * v / nf), by = x0 + rowh + 4 + (th - bh) * top / MAX(1, nf - v);
		drw_setscheme(drw, scheme[SAcc]);
		drw_rect(drw, pw - border - 5, by, 3, (unsigned)bh, 1, 0);
	}
	drw_setscheme(drw, scheme[status_err ? SErr : SDim]);
	drw_text(drw, x0, ph - x0 - rowh, (unsigned)w0, (unsigned)rowh, (unsigned)rowpadx,
		status[0] ? status : editing ? "Enter accept   Esc cancel   Ctrl-U clear" : "Tab next  S-Tab prev  Space/Enter change  Left/Right step  Esc close", 0);

	blit();
}

static void
blit(void)
{
	if (!shm_pixels || shm_size != (size_t)pw * ph * 4) {
		struct wl_shm_pool *pool;
		if (wl_buf) { wl_buffer_destroy(wl_buf); munmap(shm_pixels, shm_size); close(shm_fd); }
		shm_size = (size_t)pw * ph * 4;
		{
			char name[] = "/settings-shm-XXXXXX";
			shm_fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
			if (shm_fd < 0) die("settings: shm_open");
			shm_unlink(name);
		}
		if (ftruncate(shm_fd, (off_t)shm_size) < 0) die("settings: ftruncate");
		shm_pixels = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
		if (shm_pixels == MAP_FAILED) die("settings: mmap");
		pool = wl_shm_create_pool(shm, shm_fd, (int32_t)shm_size);
		wl_buf = wl_shm_pool_create_buffer(pool, 0, pw, ph, pw * 4, WL_SHM_FORMAT_ARGB8888);
		wl_shm_pool_destroy(pool);
	}
	memcpy(shm_pixels, drw->pixels, shm_size);
	wl_surface_attach(surface, wl_buf, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, pw, ph);
	wl_surface_commit(surface);
}


/* ---- the volume panel: two bars (speaker, microphone) over the compositor's `volume` command */
static struct { int pct, muted; } vol[2];
static long vseq = -1, vstep_seen;
static int vosd_ms = 1500, vstep = 5, vmax = 100, vsel, vdrag = -1;
static long long vdeadline;

static long long
now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* ask the compositor; 1 when something differs from what we drew */
static int
vol_poll(void)
{
	char *r = ctl("volume get");
	int a, am, b, bm, osd, st, mx, ch;
	long sq;
	if (!r) { snprintf(status, sizeof status, "compositor not reachable"); status_err = 1; return 0; }
	if (sscanf(r, "spk %d %d mic %d %d seq %ld osd %d step %d max %d", &a, &am, &b, &bm, &sq, &osd, &st, &mx) != 8) {
		snprintf(status, sizeof status, "%.150s", r + (!strncmp(r, "error: ", 7) ? 7 : 0));
		status[strcspn(status, "\n")] = 0;
		status_err = 1; free(r); return 0;
	}
	free(r);
	status_err = 0; status[0] = 0;
	ch = a != vol[0].pct || am != vol[0].muted || b != vol[1].pct || bm != vol[1].muted || sq != vseq;
	vol[0].pct = a; vol[0].muted = am; vol[1].pct = b; vol[1].muted = bm; vseq = sq;
	vosd_ms = osd; vstep = st; vmax = mx > 0 ? mx : 100;
	if (ch && osdmode) vdeadline = now_ms() + vosd_ms;
	return ch;
}

static void
vol_send(const char *fmt, ...)
{
	char line[96];
	va_list ap;
	char *r;
	va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
	if ((r = ctl(line))) free(r);
	vol_poll();
	if (osdmode) vdeadline = now_ms() + vosd_ms;
}

/* geometry shared by drawing and the mouse: row i of the two */
static int vol_rowh(void) { return fh + 2 * rowpady; }
static int vol_rowy(int i) { return border + pad + vol_rowh() + 8 + i * (vol_rowh() + 6); }
static int vol_barx(void) { return border + pad + 90; }
static int vol_barw(void) { return pw - border - pad - 80 - vol_barx(); }
static int vol_row_at(int y) { for (int i = 0; i < 2; i++) if (y >= vol_rowy(i) && y < vol_rowy(i) + vol_rowh()) return i; return -1; }

static void
vol_set_at(int row, int x)
{
	int bx = vol_barx(), bw = vol_barw(), pct;
	if (bw < 2) return;
	pct = (int)lround((double)(x - bx) * vmax / (bw - 1));
	if (pct < 0) pct = 0;
	if (pct > vmax) pct = vmax;
	vol_send("volume set %s %d", row ? "mic" : "spk", pct);
}

static void
vol_paint(void)
{
	int x0 = border + pad, w0 = pw - 2 * x0, rowh = vol_rowh(), y, i;
	char buf[64];
	drw_setscheme(drw, scheme[SNorm]);
	drw_rect(drw, 0, 0, (unsigned)pw, (unsigned)ph, 1, 1);
	drw_setscheme(drw, scheme[SAcc]);
	for (y = 0; y < border; y++) drw_rect(drw, y, y, (unsigned)(pw - 2 * y), (unsigned)(ph - 2 * y), 0, 0);
	drw_text(drw, x0, x0, (unsigned)w0, (unsigned)rowh, (unsigned)rowpadx, "Volume", 0);
	for (i = 0; i < 2; i++) {
		int issel = !osdmode && i == vsel, ry = vol_rowy(i), bx = vol_barx(), bw = vol_barw(), fill;
		drw_setscheme(drw, scheme[issel ? SSel : SNorm]);
		drw_text(drw, x0, ry, 90, (unsigned)rowh, (unsigned)rowpadx, i ? "mic" : "speaker", 0);
		drw_setscheme(drw, scheme[SDim]);
		drw_rect(drw, bx, ry + 4, (unsigned)bw, (unsigned)(rowh - 8), 0, 0);
		fill = (int)((long long)(bw - 4) * vol[i].pct / vmax);
		if (fill > bw - 4) fill = bw - 4;
		drw_setscheme(drw, scheme[vol[i].muted ? SDim : vol[i].pct > 100 ? SErr : SAcc]);
		if (fill > 0) drw_rect(drw, bx + 2, ry + 6, (unsigned)fill, (unsigned)(rowh - 12), 1, 0);
		if (vol[i].muted) snprintf(buf, sizeof buf, "MUTE"); else snprintf(buf, sizeof buf, "%d%%", vol[i].pct);
		drw_setscheme(drw, scheme[vol[i].muted ? SErr : SNorm]);
		drw_text(drw, bx + bw + 4, ry, 76, (unsigned)rowh, 6, buf, 0);
	}
	drw_setscheme(drw, scheme[status_err ? SErr : SDim]);
	drw_text(drw, x0, ph - x0 - rowh, (unsigned)w0, (unsigned)rowh, (unsigned)rowpadx,
		status[0] ? status : osdmode ? "click, drag or wheel to adjust" : "Tab row  </> level  m mute  Esc", 0);
}

static void
vol_handle(xkb_keysym_t sym)
{
	const char *pre = vsel ? "mic" : "";
	switch (sym) {
	case XKB_KEY_Escape: case XKB_KEY_q: running = 0; return;
	case XKB_KEY_Tab: case XKB_KEY_ISO_Left_Tab: case XKB_KEY_Up: case XKB_KEY_Down: case XKB_KEY_j: case XKB_KEY_k: vsel = !vsel; break;
	case XKB_KEY_Left: case XKB_KEY_h: case XKB_KEY_minus: case XKB_KEY_underscore: vol_send("volume %sdown", pre); break;
	case XKB_KEY_Right: case XKB_KEY_l: case XKB_KEY_plus: case XKB_KEY_equal: vol_send("volume %sup", pre); break;
	case XKB_KEY_Prior: vol_send("volume %sup 25", pre); break;
	case XKB_KEY_Next: vol_send("volume %sdown 25", pre); break;
	case XKB_KEY_Home: vol_send("volume set %s 0", vsel ? "mic" : "spk"); break;
	case XKB_KEY_End: vol_send("volume set %s %d", vsel ? "mic" : "spk", vmax); break;
	case XKB_KEY_m: case XKB_KEY_space: case XKB_KEY_Return: case XKB_KEY_KP_Enter: vol_send("volume %smute", vsel ? "mic" : ""); break;
	default: break;
	}
}

static void
vol_wheel(int dir, int row)
{
	if (row < 0) row = vsel;
	vol_send("volume %s%s", row ? "mic" : "", dir > 0 ? "down" : "up");
}

static void scroll_by(int dir);

/* ---- test hook */
static void
feed_test_keys(void)
{
	const char *k = getenv("DESKTOP_PANEL_KEYS");
	char *copy, *tok, *save = NULL;
	if (!k) return;
	copy = strdup(k);
	for (tok = strtok_r(copy, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
		char u[8] = "";
		xkb_keysym_t s;
		if (volmode && !strncmp(tok, "click:", 6)) { /* click:spk:70 = a click at 70% of the speaker bar */
			int row = tok[6] == 'm', pct = atoi(tok + 10);
			vol_set_at(row, vol_barx() + (vol_barw() - 1) * pct / vmax); continue;
		}
		if (volmode && !strncmp(tok, "mute:", 5)) { vol_send("volume %smute", tok[5] == 'm' ? "mic" : ""); continue; }
		if (!strcmp(tok, "wheeldown") || !strcmp(tok, "wheelup")) { scroll_by(tok[5] == 'd' ? 1 : -1); continue; } /* the mouse wheel path */
		if (!strncmp(tok, "ctrl+", 5)) { s = xkb_keysym_from_name(tok + 5, XKB_KEYSYM_NO_FLAGS); handle(s, "", 1); continue; }
		s = xkb_keysym_from_name(tok, XKB_KEYSYM_NO_FLAGS);
		if (volmode && s != XKB_KEY_NoSymbol) { vol_handle(s); continue; }
		if (s == XKB_KEY_NoSymbol) { fprintf(stderr, "settings: unknown test key '%s'\n", tok); continue; }
		xkb_keysym_to_utf8(s, u, sizeof u);
		handle(s, u, 0);
	}
	free(copy);
	if (getenv("DESKTOP_PANEL_DUMP") && volmode) {
		printf("spk=%d spkmuted=%d mic=%d micmuted=%d sel=%s\n", vol[0].pct, vol[0].muted, vol[1].pct, vol[1].muted, vsel ? "mic" : "spk");
		fflush(stdout);
	} else if (getenv("DESKTOP_PANEL_DUMP")) {
		printf("selected=%s editing=%d top=%d status=%s\n", F[sel].key, editing, top, status);
		for (int i = 0; i < nf; i++) printf("%s=%s\n", F[i].key, F[i].cur);
		fflush(stdout);
	}
}

/* ---- wayland */
static void
layer_configure(void *data, struct zwlr_layer_surface_v1 *ls, uint32_t serial, uint32_t w, uint32_t h)
{
	int first = !configured;
	zwlr_layer_surface_v1_ack_configure(ls, serial);
	if (w) pw = (int)w;
	if (h) ph = (int)h;
	configured = 1;
	drw_resize(drw, (unsigned)pw, (unsigned)ph);
	scroll_to_sel();
	if (first && volmode) vol_poll();
	if (first) feed_test_keys();
	draw();
}
static void layer_closed(void *data, struct zwlr_layer_surface_v1 *ls) { running = 0; }
static const struct zwlr_layer_surface_v1_listener layer_listener = { .configure = layer_configure, .closed = layer_closed };

static void
kb_keymap(void *data, struct wl_keyboard *kb, uint32_t format, int32_t fd, uint32_t size)
{
	char *map;
	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) { close(fd); return; }
	map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (map == MAP_FAILED) return;
	if (xkb_keymap) xkb_keymap_unref(xkb_keymap);
	xkb_keymap = xkb_keymap_new_from_string(xkb_ctx, map, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(map, size);
	if (xkb_state) xkb_state_unref(xkb_state);
	xkb_state = xkb_state_new(xkb_keymap);
}
static void kb_enter(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf, struct wl_array *keys) {}
static void kb_leave(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf) {}
static void
kb_key(void *data, struct wl_keyboard *kb, uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
	char buf[8] = "";
	xkb_keysym_t sym;
	int ctrl;
	if (state != WL_KEYBOARD_KEY_STATE_PRESSED || !xkb_state) return;
	sym = xkb_state_key_get_one_sym(xkb_state, key + 8);
	ctrl = xkb_state_mod_name_is_active(xkb_state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0;
	if (xkb_state_key_get_utf8(xkb_state, key + 8, buf, sizeof buf) < 0) buf[0] = 0;
	if (volmode) vol_handle(sym); else handle(sym, buf, ctrl);
	draw();
}
static void kb_mods(void *d, struct wl_keyboard *kb, uint32_t serial, uint32_t dep, uint32_t lat, uint32_t lock, uint32_t grp)
{
	if (xkb_state) xkb_state_update_mask(xkb_state, dep, lat, lock, 0, 0, grp);
}
static void kb_repeat(void *d, struct wl_keyboard *k, int32_t r, int32_t del) {}
static const struct wl_keyboard_listener kb_listener = { .keymap = kb_keymap, .enter = kb_enter, .leave = kb_leave, .key = kb_key,
	.modifiers = kb_mods, .repeat_info = kb_repeat };

static int
row_at(int y)
{
	int x0 = border + pad, rowh = fh + 2 * rowpady, r = (y - (x0 + rowh + 4));
	if (r < 0) return -1;
	r = top + r / rowh;
	return r < nf && r < top + visible_rows() ? r : -1;
}
static void ptr_enter(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *sf, wl_fixed_t x, wl_fixed_t y) { px = wl_fixed_to_int(x); py = wl_fixed_to_int(y); }
static void ptr_leave(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *sf) {}
static void
ptr_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y)
{
	px = wl_fixed_to_int(x); py = wl_fixed_to_int(y);
	if (volmode && osdmode) vdeadline = now_ms() + vosd_ms; /* the pointer on the OSD keeps it up */
	if (volmode && vdrag >= 0) { vol_set_at(vdrag, px); draw(); }
}
static void
ptr_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t, uint32_t button, uint32_t state)
{
	int r = volmode ? -1 : row_at(py), x0 = border + pad, labw = (pw - 2 * x0) * 55 / 100;
	if (volmode) {
		int row = vol_row_at(py);
		if (state == WL_POINTER_BUTTON_STATE_RELEASED) { vdrag = -1; return; }
		if (state != WL_POINTER_BUTTON_STATE_PRESSED || row < 0) return;
		vsel = row;
		if (px >= vol_barx() + vol_barw()) vol_send("volume %smute", row ? "mic" : "");   /* the MUTE / percent text */
		else if (px >= vol_barx()) { vdrag = row; vol_set_at(row, px); }
		draw();
		return;
	}
	if (state != WL_POINTER_BUTTON_STATE_PRESSED || r < 0) return;
	if (editing) editing = 0;
	sel = r;
	if (px >= x0 + labw) {
		const char *ty = F[r].type;
		if (!strcmp(ty, "number")) step_number(r, px < x0 + labw + (pw - 2 * x0 - labw) / 3 ? -1 : 1);
		else activate(r);
	}
	draw();
}
static void
scroll_by(int dir)
{
	int v = visible_rows();
	if (volmode) { vol_wheel(dir, vol_row_at(py)); return; }
	top += dir * 3;
	if (top > nf - v) top = nf - v;
	if (top < 0) top = 0;
}
static void
ptr_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t axis, wl_fixed_t value)
{
	if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) return;
	scroll_by(wl_fixed_to_double(value) > 0 ? 1 : -1);
	draw();
}
static void ptr_frame(void *d, struct wl_pointer *p) {}
static void ptr_axis_source(void *d, struct wl_pointer *p, uint32_t s) {}
static void ptr_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) {}
static void ptr_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t n) {}
static const struct wl_pointer_listener ptr_listener = { .enter = ptr_enter, .leave = ptr_leave, .motion = ptr_motion, .button = ptr_button,
	.axis = ptr_axis, .frame = ptr_frame, .axis_source = ptr_axis_source, .axis_stop = ptr_axis_stop, .axis_discrete = ptr_axis_discrete };

static void
seat_caps(void *data, struct wl_seat *s, uint32_t caps)
{
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard) { keyboard = wl_seat_get_keyboard(s); wl_keyboard_add_listener(keyboard, &kb_listener, NULL); }
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) { pointer = wl_seat_get_pointer(s); wl_pointer_add_listener(pointer, &ptr_listener, NULL); }
}
static void seat_name(void *d, struct wl_seat *s, const char *n) {}
static const struct wl_seat_listener seat_listener = { .capabilities = seat_caps, .name = seat_name };

static void
reg_global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t version)
{
	if (!strcmp(iface, wl_compositor_interface.name)) compositor = wl_registry_bind(reg, name, &wl_compositor_interface, MIN(version, 4));
	else if (!strcmp(iface, wl_shm_interface.name)) shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	else if (!strcmp(iface, wl_seat_interface.name)) { seat = wl_registry_bind(reg, name, &wl_seat_interface, MIN(version, 5)); wl_seat_add_listener(seat, &seat_listener, NULL); }
	else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name)) layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, MIN(version, 4));
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) {}
static const struct wl_registry_listener reg_listener = { .global = reg_global, .global_remove = reg_remove };

int
settings_main(int argc, char *argv[])
{
	struct wl_registry *registry;
	int i;
	for (i = 1; i < argc; i++)
		if (!strcmp(argv[i], "volume")) volmode = 1;
		else if (volmode && !strcmp(argv[i], "--osd")) osdmode = 1;
		else if (!strcmp(argv[i], "-l")) { /* no window: print the schema the compositor gave us, for scripts */
			char *r = ctl("schema");
			if (!r) die("settings: compositor not running (no control socket)");
			fputs(r, stdout);
			free(r);
			return 0;
		} else
			die("usage: desktop settings [-l] | desktop settings volume [--osd]");
	if (volmode) { /* one volume panel at a time: the OSD that a key press starts must not pile up */
		const char *rt = getenv("XDG_RUNTIME_DIR");
		char lock[512];
		int lfd;
		snprintf(lock, sizeof lock, "%s/desktop-volume.lock", rt ? rt : "/tmp");
		lfd = open(lock, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
		if (lfd >= 0 && flock(lfd, LOCK_EX | LOCK_NB)) {
			/* an OSD already up: it just follows the volume, so it quits; a panel the user asked for waits for it to time out */
			for (i = 0; !osdmode && i < 40 && flock(lfd, LOCK_EX | LOCK_NB); i++) usleep(100000);
			if (osdmode || i >= 40) return 0;
		}
		if (!ctl("volume get")) die("volume: compositor not running (no control socket)");
	} else if (load_schema()) die("settings: cannot read the schema - is the desktop (dwl) running? ($XDG_RUNTIME_DIR/dwl.sock)");
	if (!setlocale(LC_CTYPE, "")) fputs("warning: no locale support\n", stderr);
	if (!(display = wl_display_connect(NULL))) die("settings: cannot connect to Wayland display");
	xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &reg_listener, NULL);
	wl_display_roundtrip(display);
	wl_display_roundtrip(display);
	if (!compositor || !shm || !layer_shell) die("settings: compositor is missing wl_compositor/wl_shm/wlr-layer-shell");

	drw = drw_create(1, 1);
	if (!drw_fontset_create(drw, fonts, LENGTH(fonts))) die("no fonts could be loaded.");
	fh = (int)drw->fonts->h;
	lrpad = fh;
	for (i = 0; i < SLast; i++) scheme[i] = drw_scm_create(drw, colors[i], 2);

	if (volmode) { pw = 460; ph = 2 * (border + pad) + 4 * (fh + 2 * rowpady) + 20; }
	surface = wl_compositor_create_surface(compositor);
	layer_surface = zwlr_layer_shell_v1_get_layer_surface(layer_shell, surface, NULL, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, volmode ? "volume" : "settings");
	zwlr_layer_surface_v1_add_listener(layer_surface, &layer_listener, NULL);
	zwlr_layer_surface_v1_set_size(layer_surface, (uint32_t)pw, (uint32_t)ph);
	zwlr_layer_surface_v1_set_anchor(layer_surface, 0);
	zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, -1);
	zwlr_layer_surface_v1_set_keyboard_interactivity(layer_surface, osdmode ? ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE : ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
	if (osdmode) { /* bottom centre, out of the way, never takes the keyboard from a dmenu */
		zwlr_layer_surface_v1_set_anchor(layer_surface, ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM);
		zwlr_layer_surface_v1_set_margin(layer_surface, 0, 0, 80, 0);
	}
	wl_surface_commit(surface);
	if (volmode) { /* poll the socket as well: other keys / scripts change the volume while we are up */
		vdeadline = now_ms() + vosd_ms;
		while (running) {
			struct pollfd pfd = { wl_display_get_fd(display), POLLIN, 0 };
			while (wl_display_prepare_read(display) != 0)
				if (wl_display_dispatch_pending(display) < 0) return 0;
			if (wl_display_flush(display) < 0 && errno != EAGAIN) { wl_display_cancel_read(display); break; }
			if (poll(&pfd, 1, 100) > 0 && (pfd.revents & POLLIN)) {
				if (wl_display_read_events(display) < 0 || wl_display_dispatch_pending(display) < 0) break;
			} else
				wl_display_cancel_read(display);
			if (configured && vol_poll()) draw();
			if (osdmode && now_ms() > vdeadline) running = 0;
		}
		return 0;
	}
	while (running && wl_display_dispatch(display) >= 0)
		;
	(void)lrpad; (void)ncfg;
	return 0;
}
