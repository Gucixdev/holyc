/* theme.h - the colorado theme: semantic roles + 16 ANSI colours + fonts/cursor/icons/metrics. stb-style single header.
 *
 * File format (plain text, one `key = value` per line, `#` starts a comment line, unknown keys are an error):
 *   name, variant (dark|light), bg bg_alt fg fg_dim accent accent_fg border border_focus urgent warn ok
 *   selection_bg selection_fg link, color0..color15 (all #rrggbb, all required),
 *   font, mono_font, font_size, cursor_theme, cursor_size, icon_theme, gtk_theme, radius, gap, border_width (optional, defaults below)
 *
 *   co_theme_parse(text, &t, err, errlen)   0 ok, -1 with a "line N: ..." message
 *   co_theme_emit(&t, &sb)                  canonical text, parse(emit(t)) == t
 *   co_theme_derive(&t, name, accent, bg?, fg?, dark)  full theme from 1-3 colours
 *   co_theme_check(&t, out)                 WCAG report of every fg/bg pair, returns number of failures
 */
#ifndef COLORADO_THEME_H
#define COLORADO_THEME_H
#include <stdarg.h>
#include <stdlib.h>
#include "color.h"

/* ---- tiny string builder shared by every module ---- */
typedef struct { char *p; size_t n, cap; } co_sb;
static void co_sb_addn(co_sb *s, const char *t, size_t n) {
	if (s->n + n + 1 > s->cap) { s->cap = (s->n + n + 1) * 2 + 256; s->p = realloc(s->p, s->cap); if (!s->p) { perror("colorado: realloc"); exit(1); } }
	memcpy(s->p + s->n, t, n); s->n += n; s->p[s->n] = 0;
}
static void co_sb_add(co_sb *s, const char *t) { co_sb_addn(s, t, strlen(t)); }
static void co_sb_f(co_sb *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void co_sb_f(co_sb *s, const char *fmt, ...) {
	char buf[1024]; va_list ap; va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
	if (n < 0) return;
	if ((size_t)n < sizeof buf) co_sb_addn(s, buf, (size_t)n);
	else { char *big = malloc((size_t)n + 1); va_start(ap, fmt); vsnprintf(big, (size_t)n + 1, fmt, ap); va_end(ap); co_sb_addn(s, big, (size_t)n); free(big); }
}
static void co_sb_free(co_sb *s) { free(s->p); s->p = NULL; s->n = s->cap = 0; }

enum { CR_BG, CR_BG_ALT, CR_FG, CR_FG_DIM, CR_ACCENT, CR_ACCENT_FG, CR_BORDER, CR_BORDER_FOCUS, CR_URGENT, CR_WARN, CR_OK,
	CR_SEL_BG, CR_SEL_FG, CR_LINK, CR_ANSI0, CR_N = CR_ANSI0 + 16 };

static const char *co_role_name(int r) {
	static const char *n[CR_ANSI0] = { "bg", "bg_alt", "fg", "fg_dim", "accent", "accent_fg", "border", "border_focus", "urgent", "warn", "ok", "selection_bg", "selection_fg", "link" };
	static char a[16][8];
	if (r < CR_ANSI0) return n[r];
	snprintf(a[r - CR_ANSI0], 8, "color%d", r - CR_ANSI0);
	return a[r - CR_ANSI0];
}
static int co_role_find(const char *s) { for (int i = 0; i < CR_N; i++) if (!strcmp(co_role_name(i), s)) return i; return -1; }

typedef struct {
	char name[48];
	int dark;
	co_rgb c[CR_N];
	char font[64], mono_font[64], cursor_theme[64], icon_theme[64], gtk_theme[64];
	int font_size, cursor_size, radius, gap, border_width;
} co_theme;

static void co_theme_defaults(co_theme *t) {
	memset(t, 0, sizeof *t);
	t->dark = 1;
	snprintf(t->font, sizeof t->font, "Sans");
	snprintf(t->mono_font, sizeof t->mono_font, "monospace");
	snprintf(t->cursor_theme, sizeof t->cursor_theme, "Adwaita");
	snprintf(t->icon_theme, sizeof t->icon_theme, "Adwaita");
	t->font_size = 11; t->cursor_size = 24; t->radius = 6; t->gap = 8; t->border_width = 2;
}

static char *co_trim(char *s) {
	while (*s == ' ' || *s == '\t') s++;
	size_t n = strlen(s);
	while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
	return s;
}
static int co_set_str(char *dst, size_t cap, const char *v) { if (!*v || strlen(v) >= cap) return -1; for (const char *p = v; *p; p++) if ((unsigned char)*p < 32 || *p == '"' || *p == '\\') return -1; strcpy(dst, v); return 0; }
static int co_set_int(int *dst, const char *v, int lo, int hi) { char *e; long x = strtol(v, &e, 10); if (*v == 0 || *e || x < lo || x > hi) return -1; *dst = (int)x; return 0; }

static int co_theme_parse(const char *text, co_theme *t, char *err, size_t errlen) {
	co_theme_defaults(t);
	unsigned long long have = 0;
	int line = 0, named = 0;
	const char *p = text;
	while (*p) {
		const char *nl = strchr(p, '\n'); size_t len = nl ? (size_t)(nl - p) : strlen(p);
		char buf[512]; line++;
		if (len >= sizeof buf) { snprintf(err, errlen, "line %d: too long", line); return -1; }
		memcpy(buf, p, len); buf[len] = 0; p += len + (nl ? 1 : 0);
		char *l = co_trim(buf);
		if (!*l || *l == '#') continue;
		char *eq = strchr(l, '=');
		if (!eq) { snprintf(err, errlen, "line %d: expected key = value", line); return -1; }
		*eq = 0; char *k = co_trim(l), *v = co_trim(eq + 1);
		int r = co_role_find(k);
		if (r >= 0) {
			if (co_hex_parse(v, &t->c[r]) || *v != '#') { snprintf(err, errlen, "line %d: %s: bad colour '%s' (want #rrggbb)", line, k, v); return -1; }
			have |= 1ULL << r;
		} else if (!strcmp(k, "name")) { if (co_set_str(t->name, sizeof t->name, v)) goto bad; named = 1; }
		else if (!strcmp(k, "variant")) { if (!strcmp(v, "dark")) t->dark = 1; else if (!strcmp(v, "light")) t->dark = 0; else goto bad; }
		else if (!strcmp(k, "font")) { if (co_set_str(t->font, sizeof t->font, v)) goto bad; }
		else if (!strcmp(k, "mono_font")) { if (co_set_str(t->mono_font, sizeof t->mono_font, v)) goto bad; }
		else if (!strcmp(k, "cursor_theme")) { if (co_set_str(t->cursor_theme, sizeof t->cursor_theme, v)) goto bad; }
		else if (!strcmp(k, "icon_theme")) { if (co_set_str(t->icon_theme, sizeof t->icon_theme, v)) goto bad; }
		else if (!strcmp(k, "gtk_theme")) { if (co_set_str(t->gtk_theme, sizeof t->gtk_theme, v)) goto bad; }
		else if (!strcmp(k, "font_size")) { if (co_set_int(&t->font_size, v, 4, 72)) goto bad; }
		else if (!strcmp(k, "cursor_size")) { if (co_set_int(&t->cursor_size, v, 8, 256)) goto bad; }
		else if (!strcmp(k, "radius")) { if (co_set_int(&t->radius, v, 0, 64)) goto bad; }
		else if (!strcmp(k, "gap")) { if (co_set_int(&t->gap, v, 0, 128)) goto bad; }
		else if (!strcmp(k, "border_width")) { if (co_set_int(&t->border_width, v, 0, 32)) goto bad; }
		else { snprintf(err, errlen, "line %d: unknown key '%s'", line, k); return -1; }
		continue;
	bad:
		snprintf(err, errlen, "line %d: bad value for %s", line, k); return -1;
	}
	if (!named) { snprintf(err, errlen, "missing 'name'"); return -1; }
	for (int i = 0; i < CR_N; i++) if (!(have & (1ULL << i))) { snprintf(err, errlen, "missing role '%s'", co_role_name(i)); return -1; }
	return 0;
}

static void co_theme_emit(const co_theme *t, co_sb *o) {
	char h[8];
	co_sb_f(o, "# colorado theme\nname = %s\nvariant = %s\n\n", t->name, t->dark ? "dark" : "light");
	for (int i = 0; i < CR_N; i++) {
		if (i == CR_ANSI0) co_sb_add(o, "\n");
		co_hex(t->c[i], h); co_sb_f(o, "%s = %s\n", co_role_name(i), h);
	}
	co_sb_f(o, "\nfont = %s\nmono_font = %s\nfont_size = %d\ncursor_theme = %s\ncursor_size = %d\nicon_theme = %s\n", t->font, t->mono_font, t->font_size, t->cursor_theme, t->cursor_size, t->icon_theme);
	if (t->gtk_theme[0]) co_sb_f(o, "gtk_theme = %s\n", t->gtk_theme);
	co_sb_f(o, "radius = %d\ngap = %d\nborder_width = %d\n", t->radius, t->gap, t->border_width);
}

/* ---- derivation from 1-3 base colours ---- */
static co_rgb co_hue(double h, double s, double l, co_rgb bg, double min) { return co_ensure(co_from_hsl(h, s, l), bg, min); }

static void co_theme_derive(co_theme *t, const char *name, co_rgb accent, const co_rgb *bg_in, const co_rgb *fg_in, int dark) {
	co_theme_defaults(t);
	snprintf(t->name, sizeof t->name, "%s", name);
	t->dark = dark;
	double ah, as, al, h, s, l;
	co_to_hsl(accent, &ah, &as, &al);
	co_rgb bg, fg;
	if (bg_in) bg = *bg_in; else bg = co_from_hsl(ah, fmin(as, 0.25), dark ? 0.09 : 0.97);
	co_to_hsl(bg, &h, &s, &l);
	if (bg_in) dark = t->dark = co_lum(bg) < 0.4;
	if (fg_in) fg = *fg_in; else fg = co_from_hsl(h, fmin(s, 0.15), dark ? 0.90 : 0.10);
	fg = co_ensure(fg, bg, 7.0);
	co_rgb *c = t->c;
	c[CR_BG] = bg;
	c[CR_BG_ALT] = co_from_hsl(h, s, l + (dark ? 0.05 : -0.05));
	c[CR_FG] = fg;
	c[CR_FG_DIM] = co_ensure(co_mix(fg, bg, 0.40), c[CR_BG_ALT], 4.5);
	c[CR_ACCENT] = accent;
	{ co_rgb w = co_rgb_of(255, 255, 255), k = co_rgb_of(0, 0, 0); c[CR_ACCENT_FG] = co_contrast(accent, k) >= co_contrast(accent, w) ? k : w; }
	c[CR_BORDER] = co_mix(bg, fg, 0.22);
	c[CR_BORDER_FOCUS] = co_ensure(accent, bg, 3.0);
	c[CR_URGENT] = co_hue(2, 0.75, 0.55, bg, 4.5);
	c[CR_WARN] = co_hue(38, 0.85, 0.55, bg, 4.5);
	c[CR_OK] = co_hue(140, 0.55, 0.50, bg, 4.5);
	c[CR_SEL_BG] = accent; c[CR_SEL_FG] = c[CR_ACCENT_FG];
	c[CR_LINK] = co_ensure(accent, bg, 4.5);
	static const double hue[8] = { 0, 2, 140, 40, 215, 300, 185, 0 };
	for (int i = 1; i < 7; i++) {
		c[CR_ANSI0 + i] = co_hue(hue[i], 0.60, 0.55, bg, 4.5);
		double hh, ss, ll; co_to_hsl(c[CR_ANSI0 + i], &hh, &ss, &ll);
		c[CR_ANSI0 + 8 + i] = co_ensure(co_from_hsl(hh, ss, ll + (dark ? 0.10 : -0.10)), bg, 4.5);
	}
	c[CR_ANSI0 + 0] = dark ? c[CR_BG_ALT] : fg;
	c[CR_ANSI0 + 8] = co_mix(c[CR_ANSI0], dark ? fg : bg, dark ? 0.35 : 0.30);
	c[CR_ANSI0 + 7] = dark ? co_mix(fg, bg, 0.12) : co_mix(bg, fg, 0.08);
	c[CR_ANSI0 + 15] = fg;
	if (!dark) { c[CR_ANSI0 + 0] = fg; c[CR_ANSI0 + 7] = c[CR_BG_ALT]; c[CR_ANSI0 + 8] = co_mix(fg, bg, 0.35); c[CR_ANSI0 + 15] = bg; }
}

/* ---- WCAG check ---- */
typedef struct { int fg, bg; double min; const char *what; } co_pair;
static const co_pair co_pairs[] = {
	{ CR_FG, CR_BG, 4.5, "text" }, { CR_FG, CR_BG_ALT, 4.5, "text on panels" }, { CR_FG_DIM, CR_BG, 3.0, "secondary text (large/UI)" },
	{ CR_FG_DIM, CR_BG_ALT, 3.0, "secondary text on panels" },
	{ CR_ACCENT_FG, CR_ACCENT, 4.5, "text on accent" }, { CR_SEL_FG, CR_SEL_BG, 4.5, "selected text" },
	{ CR_LINK, CR_BG, 4.5, "links" }, { CR_BORDER_FOCUS, CR_BG, 3.0, "focus indicator" },
	{ CR_URGENT, CR_BG, 3.0, "urgent" }, { CR_WARN, CR_BG, 3.0, "warning" }, { CR_OK, CR_BG, 3.0, "ok" },
};
static int co_theme_check(const co_theme *t, co_sb *o) {
	int fails = 0; char a[8], b[8];
	co_sb_f(o, "%-14s %-14s %7s %6s  %s\n", "foreground", "background", "ratio", "need", "result");
	for (size_t i = 0; i < sizeof co_pairs / sizeof *co_pairs + 12; i++) {
		int f, g; double min; const char *what;
		if (i < sizeof co_pairs / sizeof *co_pairs) { f = co_pairs[i].fg; g = co_pairs[i].bg; min = co_pairs[i].min; what = co_pairs[i].what; }
		else { int k = (int)(i - sizeof co_pairs / sizeof *co_pairs); /* ANSI: 1-6 and 9-14 vs bg (3.0), 7 and 15 (4.5) */
			static const int idx[12] = { 1, 2, 3, 4, 5, 6, 9, 10, 11, 12, 13, 14 }; f = CR_ANSI0 + idx[k]; g = CR_BG; min = 3.0; what = "ansi"; }
		double r = co_contrast(t->c[f], t->c[g]);
		int bad = r + 1e-9 < min; fails += bad;
		co_hex(t->c[f], a); co_hex(t->c[g], b);
		co_sb_f(o, "%-14s %-14s %7.2f %6.1f  %s  %s on %s - %s\n", co_role_name(f), co_role_name(g), r, min, bad ? "FAIL" : "ok  ", a, b, what);
	}
	{ int ff[2] = { t->dark ? CR_ANSI0 + 7 : CR_ANSI0 + 0, t->dark ? CR_ANSI0 + 15 : CR_ANSI0 + 8 }; double need[2] = { 4.5, t->dark ? 4.5 : 3.0 };
	  for (int k = 0; k < 2; k++) { double r = co_contrast(t->c[ff[k]], t->c[CR_BG]); int bad = r + 1e-9 < need[k]; fails += bad;
		co_sb_f(o, "%-14s %-14s %7.2f %6.1f  %s  default terminal text\n", co_role_name(ff[k]), "bg", r, need[k], bad ? "FAIL" : "ok  "); } }
	co_sb_f(o, "%d failure%s\n", fails, fails == 1 ? "" : "s");
	return fails;
}
#endif
