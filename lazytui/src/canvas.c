/* canvas.c - a grid of styled cells that widgets draw into, and the diff renderer that turns two grids into the
 * shortest ANSI stream. Colours are quantised to what the terminal can show (truecolour -> 256 -> 16 -> 8) and
 * drawing characters fall back to ASCII on terminals without unicode (the Linux console). Nothing here touches the tty. */
#include "lazytui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct lt_canvas { int w, h; struct cell { uint32_t ch; lt_style st; } *c; lt_caps caps; lt_rect clip; bool has_clip; };

/* ------------------------------------------------------------------ utf-8 and widths */
size_t lt_utf8_decode(const char *s, uint32_t *r)
{
	const unsigned char *u = (const unsigned char *)s;
	if (u[0] < 0x80) { *r = u[0]; return u[0] ? 1 : 0; }
	if ((u[0] & 0xE0) == 0xC0 && (u[1] & 0xC0) == 0x80) { *r = ((uint32_t)(u[0] & 0x1F) << 6) | (u[1] & 0x3F); if (*r >= 0x80) return 2; }
	else if ((u[0] & 0xF0) == 0xE0 && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80) { *r = ((uint32_t)(u[0] & 0x0F) << 12) | ((uint32_t)(u[1] & 0x3F) << 6) | (u[2] & 0x3F); if (*r >= 0x800) return 3; }
	else if ((u[0] & 0xF8) == 0xF0 && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80 && (u[3] & 0xC0) == 0x80) { *r = ((uint32_t)(u[0] & 0x07) << 18) | ((uint32_t)(u[1] & 0x3F) << 12) | ((uint32_t)(u[2] & 0x3F) << 6) | (u[3] & 0x3F); if (*r >= 0x10000 && *r <= 0x10FFFF) return 4; }
	*r = 0xFFFD;
	return 1;
}
size_t lt_utf8_encode(uint32_t r, char *o)
{
	if (r < 0x80) { o[0] = (char)r; return 1; }
	if (r < 0x800) { o[0] = (char)(0xC0 | (r >> 6)); o[1] = (char)(0x80 | (r & 0x3F)); return 2; }
	if (r < 0x10000) { o[0] = (char)(0xE0 | (r >> 12)); o[1] = (char)(0x80 | ((r >> 6) & 0x3F)); o[2] = (char)(0x80 | (r & 0x3F)); return 3; }
	o[0] = (char)(0xF0 | (r >> 18)); o[1] = (char)(0x80 | ((r >> 12) & 0x3F)); o[2] = (char)(0x80 | ((r >> 6) & 0x3F)); o[3] = (char)(0x80 | (r & 0x3F));
	return 4;
}
static int rune_width(uint32_t r)
{
	if (r == 0) return 0;
	if (r < 32 || (r >= 0x7F && r < 0xA0)) return 0;
	if ((r >= 0x0300 && r <= 0x036F) || (r >= 0x200B && r <= 0x200F) || (r >= 0xFE00 && r <= 0xFE0F) || (r >= 0x20D0 && r <= 0x20FF)) return 0;
	if ((r >= 0x1100 && r <= 0x115F) || (r >= 0x2E80 && r <= 0xA4CF) || (r >= 0xAC00 && r <= 0xD7A3) || (r >= 0xF900 && r <= 0xFAFF) ||
	    (r >= 0xFE30 && r <= 0xFE6F) || (r >= 0xFF00 && r <= 0xFF60) || (r >= 0xFFE0 && r <= 0xFFE6) || (r >= 0x1F300 && r <= 0x1F64F) ||
	    (r >= 0x1F900 && r <= 0x1F9FF) || (r >= 0x20000 && r <= 0x3FFFD)) return 2;
	return 1;
}
int lt_utf8_width(const char *s)
{
	int w = 0;
	while (*s) { uint32_t r; s += lt_utf8_decode(s, &r); w += rune_width(r); }
	return w;
}

/* the drawing characters of unicode mode and their ASCII stand-ins */
uint32_t lt_ascii_of(uint32_t r)
{
	switch (r) {
	case 0x2500: case 0x2501: case 0x2550: return '-';
	case 0x2502: case 0x2503: case 0x2551: return '|';
	case 0x250C: case 0x2510: case 0x2514: case 0x2518: case 0x256D: case 0x256E: case 0x256F: case 0x2570: case 0x250F: case 0x2513:
	case 0x2517: case 0x251B: case 0x2554: case 0x2557: case 0x255A: case 0x255D: return '+';
	case 0x2588: case 0x2589: case 0x258A: case 0x258B: case 0x258C: case 0x258D: case 0x258E: case 0x258F: case 0x2590: return '#';
	case 0x2591: case 0x2592: case 0x2593: return '.';
	case 0x2191: return '^'; case 0x2193: return 'v'; case 0x2190: return '<'; case 0x2192: return '>';
	case 0x00B7: case 0x2022: return '.';
	case 0x25CF: case 0x25CB: return 'o';
	case 0x15E7: return 'C';
	case 0x25AA: case 0x25A0: return '#';
	case 0x25AB: case 0x25A1: return '.';
	case 0x00D7: return 'x';
	case 0x2026: return '.';
	}
	if (r >= 0x2800 && r <= 0x28FF) return '*';
	return '?';
}
static bool is_symbol(uint32_t r) { return (r >= 0x2190 && r <= 0x2BFF) || (r >= 0x25A0 && r <= 0x25FF) || r == 0xB7 || r == 0xD7 || r == 0x15E7 || (r >= 0x2500 && r <= 0x259F) || (r >= 0x2800 && r <= 0x28FF) || r == 0x2026; }

/* ------------------------------------------------------------------ canvas */
lt_canvas *lt_canvas_new(int w, int h, lt_caps caps)
{
	lt_canvas *c = calloc(1, sizeof *c);
	c->caps = caps;
	lt_canvas_resize(c, w, h);
	return c;
}
void lt_canvas_set_clip(lt_canvas *c, lt_rect r) { c->clip = r; c->has_clip = true; }
void lt_canvas_reset_clip(lt_canvas *c) { c->has_clip = false; }
void lt_canvas_free(lt_canvas *c) { if (c) { free(c->c); free(c); } }
void lt_canvas_resize(lt_canvas *c, int w, int h)
{
	if (w < 1) w = 1;
	if (h < 1) h = 1;
	free(c->c);
	c->w = w; c->h = h;
	c->c = calloc((size_t)w * (size_t)h, sizeof *c->c);
	lt_canvas_clear(c, LT_STYLE(LT_DEFAULT, LT_DEFAULT, 0));
}
void lt_canvas_clear(lt_canvas *c, lt_style st)
{
	for (int i = 0; i < c->w * c->h; i++) { c->c[i].ch = ' '; c->c[i].st = st; }
}
void lt_canvas_copy(lt_canvas *d, const lt_canvas *s)
{
	if (d->w != s->w || d->h != s->h) lt_canvas_resize(d, s->w, s->h);
	memcpy(d->c, s->c, (size_t)s->w * (size_t)s->h * sizeof *s->c);
}
static bool in_rect(lt_rect r, int x, int y);
/* ---- snapshots: a rectangle of cells taken once and blitted many times (a costly widget whose inputs did not change) ---- */
struct lt_snap { int w, h; struct cell *c; };
lt_snap *lt_snap_take(const lt_canvas *c, lt_rect r)
{
	lt_snap *s = calloc(1, sizeof *s);
	if (r.w < 1 || r.h < 1) return s;
	s->w = r.w; s->h = r.h;
	s->c = calloc((size_t)r.w * (size_t)r.h, sizeof *s->c);
	for (int y = 0; y < r.h; y++)
		for (int x = 0; x < r.w; x++) {
			int X = r.x + x, Y = r.y + y;
			struct cell *d = &s->c[y * r.w + x];
			if (X < 0 || Y < 0 || X >= c->w || Y >= c->h) { d->ch = ' '; d->st = LT_STYLE(LT_DEFAULT, LT_DEFAULT, 0); continue; }
			*d = c->c[Y * c->w + X];
		}
	return s;
}
void lt_snap_blit(const lt_snap *s, lt_canvas *c, int x0, int y0)
{
	if (!s || !s->c) return;
	for (int y = 0; y < s->h; y++)
		for (int x = 0; x < s->w; x++) {
			int X = x0 + x, Y = y0 + y;
			if (X < 0 || Y < 0 || X >= c->w || Y >= c->h) continue;
			if (c->has_clip && !in_rect(c->clip, X, Y)) continue;
			c->c[Y * c->w + X] = s->c[y * s->w + x];
		}
}
void lt_snap_free(lt_snap *s) { if (s) { free(s->c); free(s); } }
int lt_canvas_w(const lt_canvas *c) { return c->w; }
int lt_canvas_h(const lt_canvas *c) { return c->h; }
const lt_caps *lt_canvas_caps(const lt_canvas *c) { return &c->caps; }
uint32_t lt_canvas_char(const lt_canvas *c, int x, int y) { return (x < 0 || y < 0 || x >= c->w || y >= c->h) ? ' ' : c->c[y * c->w + x].ch; }
lt_color lt_canvas_bg(const lt_canvas *c, int x, int y) { return (x < 0 || y < 0 || x >= c->w || y >= c->h) ? LT_DEFAULT : c->c[y * c->w + x].st.bg; }
lt_color lt_canvas_fg(const lt_canvas *c, int x, int y) { return (x < 0 || y < 0 || x >= c->w || y >= c->h) ? LT_DEFAULT : c->c[y * c->w + x].st.fg; }

static bool in_rect(lt_rect r, int x, int y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

void lt_put(lt_canvas *c, lt_rect clip, int x, int y, uint32_t ch, lt_style st)
{
	if (!c->caps.unicode && is_symbol(ch)) ch = lt_ascii_of(ch);
	int w = rune_width(ch);
	if (w == 0 || x < 0 || y < 0 || x >= c->w || y >= c->h || !in_rect(clip, x, y)) return;
	if (c->has_clip && !in_rect(c->clip, x, y)) return;
	if (w == 2 && (x + 1 >= c->w || !in_rect(clip, x + 1, y) || (c->has_clip && !in_rect(c->clip, x + 1, y)))) { ch = ' '; w = 1; }
	struct cell *p = &c->c[y * c->w + x];
	if (p->ch == 0 && x > 0) c->c[y * c->w + x - 1].ch = ' ';      /* overwriting the right half of a wide char */
	p->ch = ch; p->st = st;
	if (w == 2) { p[1].ch = 0; p[1].st = st; }
}

int lt_text(lt_canvas *c, lt_rect clip, int x, int y, const char *s, lt_style st, int maxw)
{
	int used = 0;
	while (*s) {
		uint32_t r;
		s += lt_utf8_decode(s, &r);
		if (r < 32) r = ' ';
		int w = rune_width(r);
		if (w == 0) continue;
		if (used + w > maxw) break;
		lt_put(c, clip, x + used, y, r, st);
		used += w;
	}
	return used;
}

void lt_fill(lt_canvas *c, lt_rect r, uint32_t ch, lt_style st)
{
	lt_rect all = {0, 0, c->w, c->h};
	for (int y = r.y; y < r.y + r.h; y++) for (int x = r.x; x < r.x + r.w; x++) lt_put(c, all, x, y, ch, st);
}

char *lt_canvas_dump(const lt_canvas *c)
{
	size_t cap = (size_t)c->w * (size_t)c->h * 4 + (size_t)c->h + 1, n = 0;
	char *o = malloc(cap);
	for (int y = 0; y < c->h; y++) {
		size_t rowstart = n;
		for (int x = 0; x < c->w; x++) {
			uint32_t ch = c->c[y * c->w + x].ch;
			if (ch == 0) continue;
			n += lt_utf8_encode(ch, o + n);
		}
		while (n > rowstart && o[n - 1] == ' ') n--;            /* trailing blanks are noise in a snapshot */
		o[n++] = '\n';
	}
	o[n] = 0;
	return o;
}

/* ------------------------------------------------------------------ colours */
static void pal_rgb(int i, int *r, int *g, int *b)
{
	static const int base[16][3] = {{0, 0, 0}, {170, 0, 0}, {0, 170, 0}, {170, 85, 0}, {0, 0, 170}, {170, 0, 170}, {0, 170, 170}, {170, 170, 170},
	                                {85, 85, 85}, {255, 85, 85}, {85, 255, 85}, {255, 255, 85}, {85, 85, 255}, {255, 85, 255}, {85, 255, 255}, {255, 255, 255}};
	if (i < 16) { *r = base[i][0]; *g = base[i][1]; *b = base[i][2]; }
	else if (i < 232) { int k = i - 16; static const int lv[6] = {0, 95, 135, 175, 215, 255}; *r = lv[k / 36]; *g = lv[(k / 6) % 6]; *b = lv[k % 6]; }
	else { *r = *g = *b = 8 + (i - 232) * 10; }
}
static int nearest(int r, int g, int b, int from, int to)
{
	int best = from;
	long bd = 1L << 40;
	for (int i = from; i < to; i++) {
		int pr, pg, pb;
		pal_rgb(i, &pr, &pg, &pb);
		long d = (long)(r - pr) * (r - pr) + (long)(g - pg) * (g - pg) + (long)(b - pb) * (b - pb);
		if (d < bd) { bd = d; best = i; }
	}
	return best;
}

bool lt_color_rgb(lt_color c, int *r, int *g, int *b)
{
	if (c == LT_DEFAULT) return false;
	if ((c >> 24) == 2) { *r = (c >> 16) & 255; *g = (c >> 8) & 255; *b = c & 255; return true; }
	pal_rgb((int)(c & 255), r, g, b);
	return true;
}
lt_color lt_color_mix(lt_color a, lt_color b, double t)
{
	int ar, ag, ab, br, bg, bb;
	if (!lt_color_rgb(a, &ar, &ag, &ab)) return b;
	if (!lt_color_rgb(b, &br, &bg, &bb)) return a;
	if (!(t > 0)) t = 0;
	if (t > 1) t = 1;
	return LT_RGB((int)(ar + (br - ar) * t + 0.5), (int)(ag + (bg - ag) * t + 0.5), (int)(ab + (bb - ab) * t + 0.5));
}

/* appends the SGR parameter for one colour, adapted to caps->colors; fg selects 30s/90s/38 vs 40s/100s/48 */
static size_t put_u(char *o, unsigned v)
{
	char t[12];
	int k = 0;
	do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
	for (int i = 0; i < k; i++) o[i] = t[k - 1 - i];
	return (size_t)k;
}
/* RGB -> palette index is a search over up to 240 entries: memoise it (direct-mapped, 4096 slots) */
static int nearest_cached(int r, int g, int b, int lo, int hi)
{
	static struct { uint32_t key; int idx; bool used; } memo[4096];
	uint32_t key = ((uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b) ^ ((uint32_t)lo << 24) ^ ((uint32_t)hi << 20);
	unsigned slot = (key * 2654435761u) >> 20;
	if (memo[slot].used && memo[slot].key == key) return memo[slot].idx;
	int idx = nearest(r, g, b, lo, hi);
	memo[slot].key = key; memo[slot].idx = idx; memo[slot].used = true;
	return idx;
}

static void emit_color(char *o, size_t *n, lt_color c, bool fg, int colors)
{
	if (c == LT_DEFAULT) return;
	int base = fg ? 30 : 40;
	if ((c >> 24) == 2) {
		int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
		if (colors >= (1 << 24)) {
			o[(*n)++] = ';'; *n += put_u(o + *n, fg ? 38u : 48u); o[(*n)++] = ';'; o[(*n)++] = '2';
			o[(*n)++] = ';'; *n += put_u(o + *n, (unsigned)r); o[(*n)++] = ';'; *n += put_u(o + *n, (unsigned)g); o[(*n)++] = ';'; *n += put_u(o + *n, (unsigned)b);
			return;
		}
		int idx = colors >= 256 ? nearest_cached(r, g, b, 16, 256) : nearest_cached(r, g, b, 0, colors >= 16 ? 16 : 8);
		c = LT_PAL(idx);
	}
	int i = (int)(c & 255);
	if (colors < 256 && i >= 16) { int r, g, b; pal_rgb(i, &r, &g, &b); i = nearest(r, g, b, 0, colors >= 16 ? 16 : 8); }
	if (colors < 16 && i >= 8) i -= 8;
	o[(*n)++] = ';';
	if (i < 8) *n += put_u(o + *n, (unsigned)(base + i));
	else if (i < 16) *n += put_u(o + *n, (unsigned)(base + 60 + i - 8));
	else { *n += put_u(o + *n, fg ? 38u : 48u); o[(*n)++] = ';'; o[(*n)++] = '5'; o[(*n)++] = ';'; *n += put_u(o + *n, (unsigned)i); }
}

static bool same_style(lt_style a, lt_style b) { return a.fg == b.fg && a.bg == b.bg && a.attr == b.attr; }

static size_t diff_core(const lt_canvas *c, const lt_canvas *prev, int origin, char *o);
static size_t diff_cap(const lt_canvas *c) { return (size_t)c->w * (size_t)c->h * 72 + 64 + (size_t)c->h * 16; }

char *lt_canvas_diff(const lt_canvas *c, const lt_canvas *prev, int origin, size_t *len)
{
	char *o = malloc(diff_cap(c));
	*len = diff_core(c, prev, origin, o);
	return o;
}
/* the same into a caller-owned buffer that is reused from frame to frame (grown when the canvas grows) */
size_t lt_canvas_diff_buf(const lt_canvas *c, const lt_canvas *prev, int origin, char **buf, size_t *cap)
{
	size_t need = diff_cap(c);
	if (*cap < need) { char *nb = realloc(*buf, need); if (!nb) return 0; *buf = nb; *cap = need; }
	return diff_core(c, prev, origin, *buf);
}

static size_t diff_core(const lt_canvas *c, const lt_canvas *prev, int origin, char *o)
{
	bool same_size = prev && prev->w == c->w && prev->h == c->h;
	size_t n = 0;
	lt_style cur = {~0u, ~0u, 255};
	for (int y = 0; y < c->h; y++) {
		const struct cell *row = c->c + y * c->w, *prow = same_size ? prev->c + y * c->w : NULL;
		int first = -1, last = -1;
		for (int x = 0; x < c->w; x++) {
			if (prow && row[x].ch == prow[x].ch && same_style(row[x].st, prow[x].st)) continue;
			if (first < 0) first = x;
			last = x;
		}
		if (first < 0) continue;
		if (first > 0 && row[first].ch == 0) first--;                       /* never start on the right half of a wide char */
		o[n++] = '\033'; o[n++] = '['; n += put_u(o + n, (unsigned)(origin + y + 1)); o[n++] = ';'; n += put_u(o + n, (unsigned)(first + 1)); o[n++] = 'H';
		for (int x = first; x <= last; x++) {
			if (row[x].ch == 0) continue;
			if (!same_style(row[x].st, cur)) {
				const lt_style *s = &row[x].st;
				o[n++] = '\033'; o[n++] = '['; o[n++] = '0';
				if (s->attr & LT_BOLD) o[n++] = ';', o[n++] = '1';
				if (s->attr & LT_DIM) o[n++] = ';', o[n++] = '2';
				if (s->attr & LT_ITALIC) o[n++] = ';', o[n++] = '3';
				if (s->attr & LT_UNDERLINE) o[n++] = ';', o[n++] = '4';
				if (s->attr & LT_BLINK) o[n++] = ';', o[n++] = '5';
				if (s->attr & LT_REVERSE) o[n++] = ';', o[n++] = '7';
				if (s->attr & LT_STRIKE) o[n++] = ';', o[n++] = '9';
				emit_color(o, &n, s->fg, true, c->caps.colors);
				emit_color(o, &n, s->bg, false, c->caps.colors);
				o[n++] = 'm';
				cur = *s;
			}
			n += lt_utf8_encode(row[x].ch, o + n);
		}
	}
	o[n++] = '\033'; o[n++] = '['; o[n++] = '0'; o[n++] = 'm';
	o[n] = 0;
	return n;
}
