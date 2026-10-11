/* bar.c - every one-line status bar and the box they sit in, in one file (a handful of small, related widgets - splitting each into
 * its own 30-80 line file was more files than it was worth): progressbar, pacmanbar, ringbar, zonebar (+legend), textbar, searchbar
 * (drawing only), panel (the bordered frame), keybar (the fzf-style hint strip). */
#include "internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* ================================================================== progressbar: a labelled bar filled 0..1, eighth-block resolution, follows the theme gradient when it has one */
/* label on the left, percent on the right, bar in between: returns the bar's rectangle */
static lt_rect frame_bar(lt_canvas *c, lt_rect r, const lt_theme *t, double v, const char *label, bool percent)
{
	lt_rect all = lt_full(c);
	r = lt_one_row(r);
	int left = 0, right = 0;
	if (label && *label) {
		int lw = lt_utf8_width(label), maxl = r.w / 3;
		if (lw > maxl) lw = maxl;
		left = lt_text(c, all, r.x, r.y, label, lt_slot_style(t, LT_T_LABEL), lw) + 1;
	}
	if (percent) {
		char b[16];
		snprintf(b, sizeof b, "%3d%%", (int)floor(lt_clamp01(v) * 100 + 0.5));
		right = 5;
		if (r.w - left - right >= 6) lt_text(c, all, r.x + r.w - 4, r.y, b, lt_slot_style(t, LT_T_TEXT), 4);
		else right = 0;
	}
	lt_rect bar = {r.x + left, r.y, r.w - left - right, 1};
	if (bar.w < 1) bar.w = 1;
	return bar;
}

void lt_progressbar(lt_canvas *c, lt_rect r, const lt_theme *t, double v, const char *label, bool percent)
{
	lt_rect all = lt_full(c), b = frame_bar(c, r, t, v, label, percent);
	v = lt_clamp01(v);
	lt_style on = LT_STYLE(lt_slot_style(t, LT_T_BAR).fg, lt_slot_style(t, LT_T_BAR_BG).bg, lt_slot_style(t, LT_T_BAR).attr);
	lt_style off = LT_STYLE(lt_slot_style(t, LT_T_BAR).fg, lt_slot_style(t, LT_T_BAR_BG).bg, lt_slot_style(t, LT_T_BAR_BG).attr);
	if (!lt_canvas_caps(c)->unicode) {
		if (b.w < 4) { lt_fill(c, b, v >= 1 ? '#' : '-', on); return; }
		int inner = b.w - 2, fill = (int)floor(v * inner);
		lt_put(c, all, b.x, b.y, '[', off);
		for (int i = 0; i < inner; i++) lt_put(c, all, b.x + 1 + i, b.y, i < fill ? '=' : (i == fill && v < 1 ? '>' : ' '), i <= fill && v > 0 ? on : off);
		lt_put(c, all, b.x + b.w - 1, b.y, ']', off);
		return;
	}
	static const uint32_t eighths[9] = {' ', 0x258F, 0x258E, 0x258D, 0x258C, 0x258B, 0x258A, 0x2589, 0x2588};
	double cells = v * b.w;
	int whole_cells = (int)floor(cells), eighth = (int)floor((cells - whole_cells) * 8 + 0.5);
	if (eighth == 8) { whole_cells++; eighth = 0; }
	for (int i = 0; i < b.w; i++) {
		lt_style g = on;
		if (t->ngrad > 0) g.fg = lt_theme_ramp(t, b.w > 1 ? (double)i / (b.w - 1) : 0);          /* hue follows the position along the bar */
		if (i < whole_cells) lt_put(c, all, b.x + i, b.y, 0x2588, g);
		else if (i == whole_cells && eighth > 0) lt_put(c, all, b.x + i, b.y, eighths[eighth], g);
		else lt_put(c, all, b.x + i, b.y, ' ', off);
	}
}

/* ================================================================== pacmanbar: a pacman -Sy style download line, byte-exact with pacman's own callback.c */
/* the same arithmetic as pacman's callback.c (fill_progress and the info columns) */
static void humanize(double v, char *num, size_t nn, const char **unit, int decimals_rule)
{
	static const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB", "ZiB", "YiB"};
	int u = 0;
	while (v > 2048.0 && u < 8) { v /= 1024.0; u++; }
	if (decimals_rule) snprintf(num, nn, "%.*f", v < 10 ? 2 : v < 100 ? 1 : 0, v);      /* rates: 4.15  10.6  798 */
	else snprintf(num, nn, "%.1f", v);                                                  /* sizes: 22.0 */
	*unit = units[u];
}

size_t lt_pacman_line(char *out, size_t cap, int cols, const char *name, double done, double total, double rate, int eta_s, lt_pacman_state *st)
{
	if (cap == 0) return 0;
	char *o = out;
	size_t n = 0;
	int infolen = cols * 6 / 10;
	if (infolen < 50) infolen = 50;
	int F = infolen - 30;                                         /* name column */
	char nm[512];
	size_t nl = strlen(name ? name : "");
	if ((int)nl > F - 1) { int keep = F - 4 > 0 ? F - 4 : 0; snprintf(nm, sizeof nm, "%.*s...", keep, name); }
	else snprintf(nm, sizeof nm, "%s", name ? name : "");
	done = lt_finite_or(done, 0); rate = lt_finite_or(rate, 0);
	char sn[32], rn[32], eta[24];
	const char *su, *ru;
	humanize(done, sn, sizeof sn, &su, 0);
	humanize(rate < 0 ? 0 : rate, rn, sizeof rn, &ru, 1);
	if (eta_s < 0) snprintf(eta, sizeof eta, "--:--");
	else if (eta_s >= 3600) snprintf(eta, sizeof eta, "%02d:%02d:%02d", eta_s / 3600, eta_s / 60 % 60, eta_s % 60);
	else snprintf(eta, sizeof eta, "%02d:%02d", eta_s / 60, eta_s % 60);
	char sz[48];
	snprintf(sz, sizeof sz, "%s %3s", sn, su);
	n += (size_t)snprintf(o + n, cap - n, " %-*s %10s %5s %3s/s %s", F, nm, sz, rn, ru, eta);
	double pf = total > 0 ? lt_finite_or(done, 0) * 100.0 / total : 0;
	int percent = !(pf > 0) ? 0 : pf > 100 ? 100 : (int)pf;
	int proglen = cols - infolen;
	int hashlen = proglen > 8 ? proglen - 8 : 0;
	int hash = percent * hashlen / 100;
	lt_pacman_state local = {0, 0};
	if (!st) st = &local;
	if (percent == 0) { st->lasthash = 0; st->mouth = 0; }
	if (hashlen > 0 && n + (size_t)hashlen + 4 < cap) {
		o[n++] = ' '; o[n++] = '[';
		for (int i = hashlen; i > 0; --i) {
			if (i > hashlen - hash) o[n++] = '-';
			else if (i == hashlen - hash) {
				if (st->lasthash != hash) { st->lasthash = hash; st->mouth = st->mouth == 1 ? 0 : 1; }
				o[n++] = st->mouth ? 'c' : 'C';
			} else o[n++] = i % 3 == 0 ? 'o' : ' ';
		}
		o[n++] = ']';
	}
	if (proglen >= 5) n += (size_t)snprintf(o + n, cap - n, " %3d%%", percent);
	o[n < cap ? n : cap - 1] = 0;
	return n;
}

void lt_pacmanbar(lt_canvas *c, lt_rect r, const lt_theme *t, const char *name, double done, double total, double rate, int eta_s, lt_pacman_state *st)
{
	r = lt_one_row(r);
	if (r.w < 1) return;
	char line[1024];
	lt_pacman_line(line, sizeof line, r.w, name, done, total, rate, eta_s, st);
	lt_rect all = lt_full(c);
	lt_text(c, all, r.x, r.y, line, lt_slot_style(t, LT_T_TEXT), r.w);
	/* the only colour: the chomping mouth (the characters themselves are pacman's) */
	char *open = strchr(line, '['), *close = open ? strchr(open, ']') : NULL;
	if (open && close) {
		lt_style pac = lt_slot_style(t, LT_T_WARN);
		pac.attr |= LT_BOLD;
		for (char *p = open + 1; p < close; p++)
			if ((*p == 'C' || *p == 'c') && (int)(p - line) < r.w) { lt_put(c, all, r.x + (int)(p - line), r.y, (uint32_t)*p, pac); break; }
	}
}

/* ================================================================== ringbar: a marquee - text scrolling through the whole width as an infinite ring, no bar underneath */
void lt_ringbar(lt_canvas *c, lt_rect r, const lt_theme *t, double phase, const char *text, lt_color color)
{
	lt_rect all = lt_full(c);
	r = lt_one_row(r);
	if (r.w < 1) return;
	phase = fmod(lt_finite_or(phase, 0), 1e6);
	if (!text || !*text) return;
	/* the ring: the text, three blanks, and again - repeated to any width */
	int n = 0;
	lt_arena_mark sm_ = lt_arena_save(lt_scratch());
	uint32_t *ring = lt_arena_alloc(lt_scratch(), (strlen(text) + 4) * sizeof *ring);
	for (const char *s = text; *s;) { uint32_t q; s += lt_utf8_decode(s, &q); ring[n++] = q; }
	for (int i = 0; i < 3; i++) ring[n++] = ' ';
	int cells = 0;                                            /* the ring in cells (wide characters count 2) */
	for (int i = 0; i < n; i++) { char tmp[5]; size_t l = lt_utf8_encode(ring[i], tmp); tmp[l] = 0; int w = lt_utf8_width(tmp); cells += w ? w : 1; }
	int off = cells ? ((int)floor(phase) % cells + cells) % cells : 0;
	lt_style st = lt_slot_style(t, LT_T_ACCENT);
	if (color != LT_DEFAULT) st.fg = color;
	int idx = 0, skip = off, col = 0;
	while (col < r.w) {                                        /* walk the ring starting `off` cells in */
		char tmp[5]; size_t l = lt_utf8_encode(ring[idx % n], tmp); tmp[l] = 0;
		int w = lt_utf8_width(tmp); if (w == 0) w = 1;
		if (skip >= w) { skip -= w; idx++; continue; }
		if (skip > 0 || col + w > r.w) { lt_put(c, all, r.x + col, r.y, ' ', st); col++; skip = 0; if (w == 2) idx++; continue; }
		lt_put(c, all, r.x + col, r.y, ring[idx % n], st);
		col += w; idx++;
	}
 	lt_arena_restore(lt_scratch(), sm_);
}

/* ================================================================== zonebar: a bar split into weighted zones (used/cache/free style), with an optional piebar-style legend below */
/* black or white text, whichever reads better on `bg` */
static lt_color ink_for(lt_color bg)
{
	if ((bg >> 24) == 2) return (0.299 * ((bg >> 16) & 255) + 0.587 * ((bg >> 8) & 255) + 0.114 * (bg & 255)) > 140 ? LT_RGB(0, 0, 0) : LT_RGB(255, 255, 255);
	if ((bg >> 24) == 1) { unsigned n = bg & 255; return (n == 0 || n == 1 || n == 4 || n == 5 || n == 8) ? LT_PAL(15) : LT_PAL(0); }
	return LT_PAL(0);
}

/* draws just the bar row (r is collapsed to one row); returns the total weight, so lt_zonebar_ex's legend can compute percentages
 * without summing twice, and 0 when there is nothing to show (the caller then paints an empty track). */
static double bar_row(lt_canvas *c, lt_rect r, const lt_theme *t, const lt_zone *z, int n)
{
	if (n <= 0 || r.w <= 0) return 0;
	double total = 0;
	for (int i = 0; i < n; i++) total += z[i].weight > 0 ? z[i].weight : 0;
	if (total <= 0) { lt_fill(c, r, ' ', lt_slot_style(t, LT_T_BAR_BG)); return 0; }
	/* largest remainder: integer widths that sum to exactly r.w */
	lt_arena_mark sm_ = lt_arena_save(lt_scratch());
	int *w = lt_arena_calloc(lt_scratch(), (size_t)n * sizeof *w);
	double *rem = lt_arena_calloc(lt_scratch(), (size_t)n * sizeof *rem);
	int used = 0;
	for (int i = 0; i < n; i++) {
		double q = (z[i].weight > 0 ? z[i].weight : 0) / total * r.w;
		w[i] = (int)floor(q); rem[i] = q - w[i]; used += w[i];
	}
	while (used < r.w) {
		int best = 0;
		for (int i = 1; i < n; i++) if (rem[i] > rem[best]) best = i;
		w[best]++; rem[best] = -1; used++;
	}
	int x = r.x;
	for (int i = 0; i < n; i++) {
		lt_style base = lt_slot_style(t, z[i].colour);
		lt_style st = base.fg == LT_DEFAULT ? LT_STYLE(LT_DEFAULT, LT_DEFAULT, LT_REVERSE) : LT_STYLE(ink_for(base.fg), base.fg, LT_BOLD);
		lt_fill(c, (lt_rect){x, r.y, w[i], 1}, ' ', st);        /* pure colour, no label text - a legend (lt_zonebar_ex) is what names a zone */
		x += w[i];
	}
	lt_arena_restore(lt_scratch(), sm_);
	return total;
}

void lt_zonebar(lt_canvas *c, lt_rect r, const lt_theme *t, const lt_zone *z, int n) { bar_row(c, lt_one_row(r), t, z, n); }

/* the bar, then (with_legend) one compact line below it: a coloured square, its label and its percentage, per zone, side by side -
 * everything the old stacked per-row legend showed, just laid out horizontally instead of one row per zone. A zone that doesn't fit
 * the remaining width is simply not reached (nothing wraps or overlaps). */
void lt_zonebar_ex(lt_canvas *c, lt_rect r, const lt_theme *t, const lt_zone *z, int n, bool with_legend)
{
	if (n <= 0 || r.w <= 0 || r.h <= 0) return;
	lt_rect bar = {r.x, r.y, r.w, 1};
	double total = bar_row(c, bar, t, z, n);
	if (!with_legend || r.h < 2) return;
	lt_rect all = lt_full(c);
	bool uni = lt_canvas_caps(c)->unicode;
	int y = r.y + 1, x = r.x, end = r.x + r.w;
	for (int i = 0; i < n && x < end; i++) {
		lt_style base = lt_slot_style(t, z[i].colour);
		lt_color sq = base.fg != LT_DEFAULT ? base.fg : t->s[LT_T_TEXT].fg;
		lt_put(c, all, x, y, uni ? 0x25A0 : '#', LT_STYLE(sq, LT_DEFAULT, 0));
		x += 2;
		if (x >= end) break;
		double w = z[i].weight > 0 ? z[i].weight : 0;
		int pct = total > 0 ? (int)floor(w / total * 100 + 0.5) : 0;
		char seg[40];
		if (z[i].label) snprintf(seg, sizeof seg, "%s %d%%", z[i].label, pct);
		else snprintf(seg, sizeof seg, "%d%%", pct);
		int sw = lt_utf8_width(seg);
		if (x + sw > end) sw = end - x;                                    /* truncate rather than overlap the next swatch */
		lt_text(c, all, x, y, seg, t->s[LT_T_TEXT], sw);
		x += sw + 2;                                                       /* a gap before the next swatch */
	}
}

/* ================================================================== textbar: a full-width line of text in a slot's colour (a status line, a footer) */
void lt_textbar(lt_canvas *c, lt_rect r, const lt_theme *t, const char *text, lt_slot slot)
{
	r = lt_one_row(r);
	lt_style st = lt_slot_style(t, slot);
	lt_fill(c, r, ' ', st);
	lt_text(c, lt_full(c), r.x + 1, r.y, text ? text : "", st, r.w - 2 > 0 ? r.w - 2 : 0);
}

/* ================================================================== searchbar: the query line's drawing (editing lives in input.c's lt_input / lt_searchbar_event) */
void lt_searchbar(lt_canvas *c, lt_rect r, const lt_theme *t, const char *prompt, const char *query, int cursor)
{
	lt_rect all = lt_full(c);
	r = lt_one_row(r);
	lt_fill(c, r, ' ', lt_slot_style(t, LT_T_TEXT));
	int x = r.x + lt_text(c, all, r.x, r.y, prompt ? prompt : "> ", lt_slot_style(t, LT_T_ACCENT), r.w);
	int cx = x, col = 0, ci = 0;
	for (const char *s = query ? query : ""; *s; ci++) {
		uint32_t q;
		size_t l = lt_utf8_decode(s, &q);
		if (ci == cursor) cx = x + col;
		lt_put(c, all, x + col, r.y, q, lt_slot_style(t, LT_T_TEXT));
		char tmp[5]; memcpy(tmp, s, l); tmp[l] = 0;
		col += lt_utf8_width(tmp);
		s += l;
	}
	if (cursor < 0) return;                                      /* no cursor: the line is not focused */
	if (cursor >= ci) cx = x + col;
	lt_style cur = lt_slot_style(t, LT_T_TEXT);
	cur.attr ^= LT_REVERSE;
	lt_put(c, all, cx, r.y, ' ', cur);                          /* a blank cell drawn reversed; text under it is redrawn */
	int k = 0;
	for (const char *s = query ? query : ""; *s; k++) {
		uint32_t q; size_t l = lt_utf8_decode(s, &q);
		if (k == cursor) lt_put(c, all, cx, r.y, q, cur);
		s += l;
	}
}

/* ================================================================== panel: the bordered frame every group of widgets sits in (round/sharp/double/HUD) */
lt_rect lt_panel_inner(lt_rect r) { return (lt_rect){r.x + 2, r.y + 1, r.w - 4 > 0 ? r.w - 4 : 0, r.h - 2 > 0 ? r.h - 2 : 0}; }

void lt_panel_ex(lt_canvas *c, lt_rect r, const lt_theme *t, const char *title, lt_panel_style style)
{
	lt_rect all = lt_full(c);
	if (r.w < 2 || r.h < 2) return;
	lt_style b = lt_slot_style(t, LT_T_BORDER);
	uint32_t hz = 0x2500, vt = 0x2502, tl = 0x256D, tr = 0x256E, bl = 0x2570, br = 0x256F;
	if (style == LT_PANEL_SHARP) { tl = 0x250C; tr = 0x2510; bl = 0x2514; br = 0x2518; }
	else if (style == LT_PANEL_DOUBLE) { hz = 0x2550; vt = 0x2551; tl = 0x2554; tr = 0x2557; bl = 0x255A; br = 0x255D; }
	else if (style == LT_PANEL_HUD) { tl = 0x250F; tr = 0x2513; bl = 0x2517; br = 0x251B; }
	for (int x = r.x + 1; x < r.x + r.w - 1; x++) { lt_put(c, all, x, r.y, hz, b); lt_put(c, all, x, r.y + r.h - 1, hz, b); }
	for (int y = r.y + 1; y < r.y + r.h - 1; y++) { lt_put(c, all, r.x, y, vt, b); lt_put(c, all, r.x + r.w - 1, y, vt, b); }
	lt_put(c, all, r.x, r.y, tl, b); lt_put(c, all, r.x + r.w - 1, r.y, tr, b);
	lt_put(c, all, r.x, r.y + r.h - 1, bl, b); lt_put(c, all, r.x + r.w - 1, r.y + r.h - 1, br, b);
	if (title && *title && r.w > 8) {
		bool hud = style == LT_PANEL_HUD;
		int tw = lt_utf8_width(title);
		int room = r.w - (hud ? 12 : 6);
		if (tw > room) tw = room > 0 ? room : 0;
		int x = r.x + 2;
		if (hud) x += lt_text(c, all, x, r.y, "[", b, 1);
		x += lt_text(c, all, x, r.y, " ", lt_slot_style(t, LT_T_TITLE), 1);
		x += lt_text(c, all, x, r.y, title, lt_slot_style(t, LT_T_TITLE), tw);
		x += lt_text(c, all, x, r.y, " ", lt_slot_style(t, LT_T_TITLE), 1);
		if (hud) lt_text(c, all, x, r.y, "]", b, 1);
	}
	if (style == LT_PANEL_HUD && r.w > 14) {                    /* status pips on the right of the top edge */
		lt_put(c, all, r.x + r.w - 8, r.y, 0x25AA, lt_slot_style(t, LT_T_ACCENT));
		lt_put(c, all, r.x + r.w - 6, r.y, 0x25AA, lt_slot_style(t, LT_T_OK));
		lt_put(c, all, r.x + r.w - 4, r.y, 0x25AB, lt_slot_style(t, LT_T_DIM));
	}
}
void lt_panel(lt_canvas *c, lt_rect r, const lt_theme *t, const char *title) { lt_panel_ex(c, r, t, title, LT_PANEL_ROUND); }

/* ================================================================== keybar: the fzf-style hint strip, Ctrl/Alt/Shift shown as their own coloured tags */
typedef struct { char text[32]; int mods; char *label; int id; int x, y, w; } khint;
struct lt_keybar { khint *h; int n; };

static void pretty(int key, char *out, size_t n)
{
	static const struct { int k; const char *t; } names[] = {
		{LT_K_ENTER, "Enter"}, {LT_K_TAB, "Tab"}, {LT_K_BTAB, "Tab"}, {LT_K_ESC, "Esc"}, {LT_K_BSPACE, "Backspace"}, {LT_K_UP, "\xE2\x86\x91"},
		{LT_K_DOWN, "\xE2\x86\x93"}, {LT_K_LEFT, "\xE2\x86\x90"}, {LT_K_RIGHT, "\xE2\x86\x92"}, {LT_K_HOME, "Home"}, {LT_K_END, "End"},
		{LT_K_PGUP, "PgUp"}, {LT_K_PGDN, "PgDn"}, {LT_K_INS, "Ins"}, {LT_K_DEL, "Del"}, {LT_K_CLICK, "Click"}, {LT_K_DCLICK, "2\xC3\x97" "Click"},
		{LT_K_RCLICK, "RClick"}, {LT_K_WHEEL_UP, "Wheel\xE2\x86\x91"}, {LT_K_WHEEL_DOWN, "Wheel\xE2\x86\x93"}, {' ', "Space"}};
	int b = key & 0xFFFFFF;
	for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) if (names[i].k == b) { snprintf(out, n, "%s", names[i].t); return; }
	if (b >= LT_K_F1 && b <= LT_K_F12) { snprintf(out, n, "F%d", b - LT_K_F1 + 1); return; }
	uint32_t r = (uint32_t)b;
	if (r >= 'a' && r <= 'z') r -= 32;
	char u[8];
	size_t l = lt_utf8_encode(r, u);
	u[l] = 0;
	snprintf(out, n, "%s", u);
}

lt_keybar *lt_keybar_new(const lt_hint *hints, int n)
{
	lt_keybar *k = calloc(1, sizeof *k);
	k->h = calloc((size_t)(n ? n : 1), sizeof *k->h);
	for (int i = 0; i < n; i++) {
		khint *h = &k->h[k->n++];
		int key = lt_key_parse(hints[i].key);
		if (key < 0 || strchr(hints[i].key, '/') || (strlen(hints[i].key) > 1 && key >= 0 && (key & 0xFFFFFF) < 32)) { snprintf(h->text, sizeof h->text, "%s", hints[i].key); h->mods = 0; }
		else { pretty(key, h->text, sizeof h->text); h->mods = key & (LT_MOD_CTRL | LT_MOD_ALT | LT_MOD_SHIFT); if ((key & 0xFFFFFF) == LT_K_BTAB) h->mods |= LT_MOD_SHIFT; }
		h->label = strdup(hints[i].label ? hints[i].label : "");
		h->id = hints[i].id;
	}
	return k;
}
void lt_keybar_free(lt_keybar *k) { if (!k) return; for (int i = 0; i < k->n; i++) free(k->h[i].label); free(k->h); free(k); }

/* shown when nothing is held: the plain keys; when modifiers are held: the keys that need exactly (a subset of) those */
static bool visible(const khint *h, int held, bool know)
{
	if (!know) return true;
	if (held) return h->mods && !(h->mods & ~held);
	return h->mods == 0;
}
static int hint_width(const khint *h)
{
	int w = lt_utf8_width(h->text) + 2 + 1 + lt_utf8_width(h->label);
	if (h->mods & LT_MOD_CTRL) w += 6;
	if (h->mods & LT_MOD_ALT) w += 5;
	if (h->mods & LT_MOD_SHIFT) w += 7;
	return w;
}

int lt_keybar_rows_needed(const lt_keybar *k, int width, int held, bool know)
{
	int rows = 1, x = 0;
	for (int i = 0; i < k->n; i++) {
		if (!visible(&k->h[i], held, know)) continue;
		int w = hint_width(&k->h[i]);
		if (x > 0 && x + w > width) { rows++; x = 0; }
		x += w + 3;
	}
	return rows;
}

void lt_keybar_draw(lt_keybar *k, lt_canvas *c, lt_rect r, const lt_theme *t, int held, bool know)
{
	lt_rect all = lt_full(c);
	lt_fill(c, r, ' ', lt_slot_style(t, LT_T_TEXT));
	int x = r.x, row = 0;
	for (int i = 0; i < k->n; i++) k->h[i].w = 0;
	for (int i = 0; i < k->n; i++) {
		khint *h = &k->h[i];
		if (!visible(h, held, know)) continue;
		int w = hint_width(h);
		if (x > r.x && x + w > r.x + r.w) { if (++row >= r.h) break; x = r.x; }
		int y = r.y + row, x0 = x;
		char cap[48];
		if (h->mods & LT_MOD_CTRL) x += lt_text(c, all, x, y, " Ctrl ", lt_slot_style(t, LT_T_MOD_CTRL), 6);
		if (h->mods & LT_MOD_ALT) x += lt_text(c, all, x, y, " Alt ", lt_slot_style(t, LT_T_MOD_ALT), 5);
		if (h->mods & LT_MOD_SHIFT) x += lt_text(c, all, x, y, " Shift ", lt_slot_style(t, LT_T_MOD_SHIFT), 7);
		snprintf(cap, sizeof cap, " %s ", h->text);
		x += lt_text(c, all, x, y, cap, lt_slot_style(t, LT_T_KEYCAP), 40);
		x += lt_text(c, all, x, y, " ", lt_slot_style(t, LT_T_LABEL), 1);
		x += lt_text(c, all, x, y, h->label, lt_slot_style(t, LT_T_LABEL), r.x + r.w - x);
		h->x = x0; h->y = y; h->w = x - x0;
		x += 3;
	}
}

int lt_keybar_hit(const lt_keybar *k, int x, int y)
{
	for (int i = 0; i < k->n; i++) {
		const khint *h = &k->h[i];
		if (h->w && y == h->y && x >= h->x && x < h->x + h->w) return h->id;
	}
	return -1;
}
