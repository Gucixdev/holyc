/* theme.c - the colour slots every widget draws with. A UI is re-coloured by swapping one lt_theme.
 * `matrix` is the default look. Besides the slots a theme carries a gradient (grad[0..ngrad-1], low -> high) that graphs,
 * sparklines and progress bars walk along; ngrad == 0 means "no gradient" (mono).
 * Palette themes are table-driven (see PAL below); adding one is one row. */
#include "lazytui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RGBX(x) LT_RGB(((x) >> 16) & 255, ((x) >> 8) & 255, (x) & 255)

static void set_grad(lt_theme *t, const unsigned *g, int n)
{
	t->ngrad = n;
	for (int i = 0; i < n && i < LT_GRAD_MAX; i++) t->grad[i] = RGBX(g[i]);
}

lt_color lt_theme_ramp(const lt_theme *t, double f)
{
	if (t->ngrad <= 0) return LT_DEFAULT;
	if (!(f > 0)) f = 0;
	if (f > 1) f = 1;
	if (t->ngrad == 1) return t->grad[0];
	double p = f * (t->ngrad - 1);
	int i = (int)p;
	if (i >= t->ngrad - 1) return t->grad[t->ngrad - 1];
	return lt_color_mix(t->grad[i], t->grad[i + 1], p - i);
}

/* the terminal's own colours (whatever the user's palette makes of them) with a light-blue gradient */
lt_theme lt_theme_terminal(void)
{
	static const unsigned g[] = {0x2a5d9f, 0x3f8fd8, 0x6cc0f0, 0xa8e2ff, 0xe4f6ff};
	lt_theme t;
	for (int i = 0; i < LT_T_COUNT; i++) t.s[i] = LT_STYLE(LT_DEFAULT, LT_DEFAULT, 0);
	t.s[LT_T_DIM] = LT_STYLE(LT_PAL(8), LT_DEFAULT, 0);
	t.s[LT_T_ACCENT] = LT_STYLE(LT_PAL(6), LT_DEFAULT, LT_BOLD);
	t.s[LT_T_OK] = LT_STYLE(LT_PAL(2), LT_DEFAULT, 0);
	t.s[LT_T_WARN] = LT_STYLE(LT_PAL(3), LT_DEFAULT, 0);
	t.s[LT_T_ERR] = LT_STYLE(LT_PAL(1), LT_DEFAULT, LT_BOLD);
	t.s[LT_T_BAR] = LT_STYLE(LT_PAL(6), LT_PAL(8), 0);
	t.s[LT_T_BAR_BG] = LT_STYLE(LT_PAL(8), LT_PAL(8), 0);
	t.s[LT_T_BORDER] = LT_STYLE(LT_RGB(157, 78, 221), LT_DEFAULT, 0);
	t.s[LT_T_TITLE] = LT_STYLE(LT_DEFAULT, LT_DEFAULT, LT_BOLD);
	t.s[LT_T_KEYCAP] = LT_STYLE(LT_DEFAULT, LT_DEFAULT, LT_REVERSE);
	t.s[LT_T_MOD_CTRL] = LT_STYLE(LT_PAL(0), LT_PAL(4), 0);
	t.s[LT_T_MOD_ALT] = LT_STYLE(LT_PAL(0), LT_PAL(3), 0);
	t.s[LT_T_MOD_SHIFT] = LT_STYLE(LT_PAL(0), LT_PAL(2), 0);
	t.s[LT_T_LABEL] = LT_STYLE(LT_PAL(7), LT_DEFAULT, 0);
	t.s[LT_T_SELECT] = LT_STYLE(LT_DEFAULT, LT_DEFAULT, LT_REVERSE);
	set_grad(&t, g, 5);
	return t;
}

/* one palette -> a whole theme. surf is the raised ground (bars, keycaps); sel is the selection bar's ground. */
typedef struct { const char *name; unsigned bg, fg, dim, accent, ok, warn, err, border, surf, sel, g[5]; bool light; } Pal;

static lt_theme from_pal(const Pal *p)
{
	lt_theme t;
	lt_color bg = RGBX(p->bg), fg = RGBX(p->fg), surf = RGBX(p->surf), ink = p->light ? RGBX(p->fg) : RGBX(p->bg);
	for (int i = 0; i < LT_T_COUNT; i++) t.s[i] = LT_STYLE(fg, bg, 0);
	t.s[LT_T_DIM] = LT_STYLE(RGBX(p->dim), bg, 0);
	t.s[LT_T_ACCENT] = LT_STYLE(RGBX(p->accent), bg, LT_BOLD);
	t.s[LT_T_OK] = LT_STYLE(RGBX(p->ok), bg, 0);
	t.s[LT_T_WARN] = LT_STYLE(RGBX(p->warn), bg, 0);
	t.s[LT_T_ERR] = LT_STYLE(RGBX(p->err), bg, LT_BOLD);
	t.s[LT_T_BAR] = LT_STYLE(RGBX(p->accent), surf, 0);
	t.s[LT_T_BAR_BG] = LT_STYLE(RGBX(p->dim), surf, 0);
	t.s[LT_T_BORDER] = LT_STYLE(RGBX(p->border), bg, 0);
	t.s[LT_T_TITLE] = LT_STYLE(RGBX(p->accent), bg, LT_BOLD);
	t.s[LT_T_KEYCAP] = LT_STYLE(fg, surf, LT_BOLD);
	t.s[LT_T_MOD_CTRL] = LT_STYLE(ink, RGBX(p->g[0]), LT_BOLD);
	t.s[LT_T_MOD_ALT] = LT_STYLE(ink, RGBX(p->warn), LT_BOLD);
	t.s[LT_T_MOD_SHIFT] = LT_STYLE(ink, RGBX(p->ok), LT_BOLD);
	t.s[LT_T_LABEL] = LT_STYLE(RGBX(p->dim), bg, 0);
	t.s[LT_T_SELECT] = LT_STYLE(p->light ? bg : RGBX(p->bg), RGBX(p->sel), LT_BOLD);
	set_grad(&t, p->g, 5);
	return t;
}

/* name, bg, fg, dim, accent, ok, warn, err, border, surface, select, gradient(5) */
static const Pal PAL[] = {
	{"dracula", 0x282a36, 0xf8f8f2, 0x6272a4, 0xbd93f9, 0x50fa7b, 0xf1fa8c, 0xff5555, 0xbd93f9, 0x44475a, 0xbd93f9, {0x8be9fd, 0x50fa7b, 0xf1fa8c, 0xffb86c, 0xff5555}, false},
	{"nord", 0x2e3440, 0xd8dee9, 0x616e88, 0x88c0d0, 0xa3be8c, 0xebcb8b, 0xbf616a, 0x81a1c1, 0x3b4252, 0x88c0d0, {0x5e81ac, 0x88c0d0, 0xa3be8c, 0xebcb8b, 0xbf616a}, false},
	{"gruvbox-dark", 0x282828, 0xebdbb2, 0x928374, 0xfabd2f, 0xb8bb26, 0xfe8019, 0xfb4934, 0x83a598, 0x3c3836, 0xfabd2f, {0x83a598, 0x8ec07c, 0xfabd2f, 0xfe8019, 0xfb4934}, false},
	{"gruvbox-light", 0xfbf1c7, 0x3c3836, 0x928374, 0xb57614, 0x79740e, 0xaf3a03, 0x9d0006, 0x076678, 0xebdbb2, 0xb57614, {0x076678, 0x427b58, 0xb57614, 0xaf3a03, 0x9d0006}, true},
	{"solarized-dark", 0x002b36, 0x93a1a1, 0x586e75, 0x268bd2, 0x859900, 0xb58900, 0xdc322f, 0x2aa198, 0x073642, 0x268bd2, {0x268bd2, 0x2aa198, 0x859900, 0xb58900, 0xdc322f}, false},
	{"solarized-light", 0xfdf6e3, 0x657b83, 0x93a1a1, 0x268bd2, 0x859900, 0xb58900, 0xdc322f, 0x2aa198, 0xeee8d5, 0x268bd2, {0x268bd2, 0x2aa198, 0x859900, 0xb58900, 0xdc322f}, true},
	{"tokyo-night", 0x1a1b26, 0xc0caf5, 0x565f89, 0x7aa2f7, 0x9ece6a, 0xe0af68, 0xf7768e, 0xbb9af7, 0x292e42, 0x7aa2f7, {0x7aa2f7, 0x7dcfff, 0x9ece6a, 0xe0af68, 0xf7768e}, false},
	{"one-dark", 0x282c34, 0xabb2bf, 0x5c6370, 0x61afef, 0x98c379, 0xe5c07b, 0xe06c75, 0xc678dd, 0x3e4451, 0x61afef, {0x61afef, 0x56b6c2, 0x98c379, 0xe5c07b, 0xe06c75}, false},
	{"one-light", 0xfafafa, 0x383a42, 0xa0a1a7, 0x4078f2, 0x50a14f, 0xc18401, 0xe45649, 0xa626a4, 0xe4e4e6, 0x4078f2, {0x4078f2, 0x0184bc, 0x50a14f, 0xc18401, 0xe45649}, true},
	{"monokai", 0x272822, 0xf8f8f2, 0x75715e, 0x66d9ef, 0xa6e22e, 0xe6db74, 0xf92672, 0xae81ff, 0x3e3d32, 0x66d9ef, {0x66d9ef, 0xa6e22e, 0xe6db74, 0xfd971f, 0xf92672}, false},
	{"rose-pine", 0x191724, 0xe0def4, 0x6e6a86, 0xc4a7e7, 0x9ccfd8, 0xf6c177, 0xeb6f92, 0xc4a7e7, 0x26233a, 0xc4a7e7, {0x31748f, 0x9ccfd8, 0xf6c177, 0xebbcba, 0xeb6f92}, false},
	{"catppuccin-mocha", 0x1e1e2e, 0xcdd6f4, 0x6c7086, 0xcba6f7, 0xa6e3a1, 0xf9e2af, 0xf38ba8, 0xcba6f7, 0x313244, 0xcba6f7, {0x89b4fa, 0x94e2d5, 0xa6e3a1, 0xfab387, 0xf38ba8}, false},
	{"catppuccin-macchiato", 0x24273a, 0xcad3f5, 0x6e738d, 0xc6a0f6, 0xa6da95, 0xeed49f, 0xed8796, 0xc6a0f6, 0x363a4f, 0xc6a0f6, {0x8aadf4, 0x8bd5ca, 0xa6da95, 0xf5a97f, 0xed8796}, false},
	{"catppuccin-frappe", 0x303446, 0xc6d0f5, 0x737994, 0xca9ee6, 0xa6d189, 0xe5c890, 0xe78284, 0xca9ee6, 0x414559, 0xca9ee6, {0x8caaee, 0x81c8be, 0xa6d189, 0xef9f76, 0xe78284}, false},
	{"catppuccin-latte", 0xeff1f5, 0x4c4f69, 0x9ca0b0, 0x8839ef, 0x40a02b, 0xdf8e1d, 0xd20f39, 0x8839ef, 0xccd0da, 0x8839ef, {0x1e66f5, 0x179299, 0x40a02b, 0xfe640b, 0xd20f39}, true},
	{"ayu-dark", 0x0f1419, 0xe6e1cf, 0x5c6773, 0x39bae6, 0xc2d94c, 0xffb454, 0xf07178, 0x39bae6, 0x131721, 0x39bae6, {0x59c2ff, 0x95e6cb, 0xc2d94c, 0xffb454, 0xf07178}, false},
	{"ayu-light", 0xfafafa, 0x5c6773, 0x828c99, 0x399ee6, 0x86b300, 0xf29718, 0xf51818, 0x399ee6, 0xf3f4f5, 0x399ee6, {0x399ee6, 0x4cbf99, 0x86b300, 0xf29718, 0xf51818}, true},
	{"everforest-dark", 0x2d353b, 0xd3c6aa, 0x7a8478, 0x83c092, 0xa7c080, 0xdbbc7f, 0xe67e80, 0x83c092, 0x374247, 0x83c092, {0x7fbbb3, 0x83c092, 0xa7c080, 0xdbbc7f, 0xe67e80}, false},
	{"kanagawa", 0x1f1f28, 0xdcd7ba, 0x727169, 0x7e9cd8, 0x98bb6c, 0xe6c384, 0xe82424, 0x7e9cd8, 0x2a2a37, 0x7e9cd8, {0x7e9cd8, 0x6a9589, 0x98bb6c, 0xe6c384, 0xe82424}, false},
	{"github-dark", 0x0d1117, 0xc9d1d9, 0x8b949e, 0x58a6ff, 0x3fb950, 0xd29922, 0xf85149, 0x58a6ff, 0x161b22, 0x58a6ff, {0x58a6ff, 0x39c5cf, 0x3fb950, 0xd29922, 0xf85149}, false},
};
#define NPAL ((int)(sizeof PAL / sizeof PAL[0]))

/* phosphor green on black. The gradient runs from a deep green through the bright phosphor to a pale glow. */
lt_theme lt_theme_matrix(void)
{
	static const unsigned g[] = {0x004d1a, 0x00a83a, 0x00ff41, 0x8dff70, 0xd8ffe0};
	lt_theme t;
	lt_color bg = LT_RGB(0, 6, 0), green = LT_RGB(0, 255, 65), mid = LT_RGB(0, 170, 45), dark = LT_RGB(0, 90, 24), pale = LT_RGB(190, 255, 205);
	for (int i = 0; i < LT_T_COUNT; i++) t.s[i] = LT_STYLE(green, bg, 0);
	t.s[LT_T_DIM] = LT_STYLE(dark, bg, 0);
	t.s[LT_T_ACCENT] = LT_STYLE(pale, bg, LT_BOLD);
	t.s[LT_T_OK] = LT_STYLE(green, bg, 0);
	t.s[LT_T_WARN] = LT_STYLE(LT_RGB(210, 255, 0), bg, LT_BOLD);
	t.s[LT_T_ERR] = LT_STYLE(LT_RGB(255, 70, 60), bg, LT_BOLD);
	t.s[LT_T_BAR] = LT_STYLE(green, LT_RGB(0, 36, 9), 0);
	t.s[LT_T_BAR_BG] = LT_STYLE(dark, LT_RGB(0, 36, 9), 0);
	t.s[LT_T_BORDER] = LT_STYLE(LT_RGB(157, 78, 221), bg, 0);                           /* purple frames, like gtop */
	t.s[LT_T_TITLE] = LT_STYLE(pale, bg, LT_BOLD);
	t.s[LT_T_KEYCAP] = LT_STYLE(bg, green, LT_BOLD);
	t.s[LT_T_MOD_CTRL] = LT_STYLE(bg, LT_RGB(0, 150, 60), 0);
	t.s[LT_T_MOD_ALT] = LT_STYLE(bg, LT_RGB(0, 220, 90), 0);
	t.s[LT_T_MOD_SHIFT] = LT_STYLE(bg, pale, 0);
	t.s[LT_T_LABEL] = LT_STYLE(mid, bg, 0);
	t.s[LT_T_SELECT] = LT_STYLE(bg, green, LT_BOLD);
	set_grad(&t, g, 5);
	return t;
}

lt_theme lt_theme_default(void) { return lt_theme_matrix(); }

/* TempleOS: white ground, VGA blue text; the accents are the 16-colour palette Terry used. */
lt_theme lt_theme_temple(void)
{
	static const unsigned g[] = {0x0000aa, 0x00aaaa, 0x00aa00, 0xaa5500, 0xaa0000};
	lt_theme t;
	lt_color bg = LT_RGB(255, 255, 255), blue = LT_RGB(0, 0, 170), lblue = LT_RGB(85, 85, 255);
	for (int i = 0; i < LT_T_COUNT; i++) t.s[i] = LT_STYLE(blue, bg, 0);
	t.s[LT_T_DIM] = LT_STYLE(LT_RGB(120, 120, 200), bg, 0);
	t.s[LT_T_ACCENT] = LT_STYLE(LT_RGB(170, 0, 0), bg, LT_BOLD);
	t.s[LT_T_OK] = LT_STYLE(LT_RGB(0, 150, 0), bg, 0);
	t.s[LT_T_WARN] = LT_STYLE(LT_RGB(170, 85, 0), bg, LT_BOLD);
	t.s[LT_T_ERR] = LT_STYLE(LT_RGB(170, 0, 0), bg, LT_BOLD);
	t.s[LT_T_BAR] = LT_STYLE(blue, LT_RGB(200, 200, 230), 0);
	t.s[LT_T_BAR_BG] = LT_STYLE(lblue, LT_RGB(200, 200, 230), 0);
	t.s[LT_T_BORDER] = LT_STYLE(blue, bg, 0);
	t.s[LT_T_TITLE] = LT_STYLE(blue, bg, LT_BOLD);
	t.s[LT_T_KEYCAP] = LT_STYLE(bg, blue, LT_BOLD);
	t.s[LT_T_MOD_CTRL] = LT_STYLE(bg, LT_RGB(0, 0, 170), 0);
	t.s[LT_T_MOD_ALT] = LT_STYLE(bg, LT_RGB(170, 85, 0), 0);
	t.s[LT_T_MOD_SHIFT] = LT_STYLE(bg, LT_RGB(0, 150, 0), 0);
	t.s[LT_T_LABEL] = LT_STYLE(LT_RGB(60, 60, 200), bg, 0);
	t.s[LT_T_SELECT] = LT_STYLE(bg, blue, LT_BOLD);
	set_grad(&t, g, 5);
	return t;
}

/* black on white, no gradient: the classic paper look (and safe anywhere) */
lt_theme lt_theme_mono(void)
{
	lt_theme t;
	lt_color bg = LT_RGB(255, 255, 255), fg = LT_RGB(0, 0, 0);
	for (int i = 0; i < LT_T_COUNT; i++) t.s[i] = LT_STYLE(fg, bg, 0);
	t.s[LT_T_DIM] = LT_STYLE(LT_RGB(120, 120, 120), bg, 0);
	t.s[LT_T_ACCENT].attr = LT_BOLD; t.s[LT_T_TITLE].attr = LT_BOLD; t.s[LT_T_ERR].attr = LT_BOLD | LT_UNDERLINE;
	t.s[LT_T_OK].fg = LT_RGB(70, 70, 70); t.s[LT_T_WARN].fg = LT_RGB(130, 130, 130);          /* zones stay tellable apart in greys */
	t.s[LT_T_BAR] = LT_STYLE(fg, LT_RGB(215, 215, 215), 0);
	t.s[LT_T_BAR_BG] = LT_STYLE(LT_RGB(150, 150, 150), LT_RGB(215, 215, 215), 0);
	t.s[LT_T_BORDER] = LT_STYLE(fg, bg, 0);
	t.s[LT_T_KEYCAP] = LT_STYLE(bg, fg, 0);
	t.s[LT_T_SELECT] = LT_STYLE(bg, fg, LT_BOLD);
	t.s[LT_T_MOD_CTRL] = LT_STYLE(bg, fg, LT_UNDERLINE);
	t.s[LT_T_MOD_ALT] = LT_STYLE(bg, LT_RGB(90, 90, 90), 0);
	t.s[LT_T_MOD_SHIFT] = LT_STYLE(bg, LT_RGB(60, 60, 60), LT_BOLD);
	t.s[LT_T_LABEL] = LT_STYLE(LT_RGB(70, 70, 70), bg, 0);
	t.ngrad = 0;
	return t;
}

/* mono, upside down: black ground, white text - for a terminal whose own palette is already dark and should stay that way */
lt_theme lt_theme_mono_reverse(void)
{
	lt_theme t = lt_theme_mono();
	lt_color bg = LT_RGB(0, 0, 0), fg = LT_RGB(255, 255, 255);
	for (int i = 0; i < LT_T_COUNT; i++) { if (t.s[i].fg == LT_RGB(0, 0, 0)) t.s[i].fg = fg; else if (t.s[i].fg == LT_RGB(255, 255, 255)) t.s[i].fg = bg; if (t.s[i].bg == LT_RGB(255, 255, 255)) t.s[i].bg = bg; else if (t.s[i].bg == LT_RGB(0, 0, 0)) t.s[i].bg = fg; }
	t.s[LT_T_DIM].fg = LT_RGB(160, 160, 160);
	t.s[LT_T_OK].fg = LT_RGB(190, 190, 190); t.s[LT_T_WARN].fg = LT_RGB(210, 210, 210);
	t.s[LT_T_BAR] = LT_STYLE(fg, LT_RGB(45, 45, 45), 0);
	t.s[LT_T_BAR_BG] = LT_STYLE(LT_RGB(110, 110, 110), LT_RGB(45, 45, 45), 0);
	t.s[LT_T_LABEL].fg = LT_RGB(190, 190, 190);
	t.s[LT_T_MOD_ALT].fg = LT_RGB(170, 170, 170);
	t.s[LT_T_MOD_SHIFT].fg = LT_RGB(200, 200, 200);
	return t;
}

/* ---- registry: every built-in theme by name, in a stable order (matrix first: it is the default) ---- */
#define NBASE 5
int lt_theme_count(void) { return NBASE + NPAL; }
const char *lt_theme_name(int i)
{
	static const char *base[NBASE] = {"matrix", "terminal", "temple", "mono", "mono-reverse"};
	if (i < 0 || i >= lt_theme_count()) return NULL;
	return i < NBASE ? base[i] : PAL[i - NBASE].name;
}
lt_theme lt_theme_at(int i)
{
	i = ((i % lt_theme_count()) + lt_theme_count()) % lt_theme_count();
	switch (i) {
	case 0: return lt_theme_matrix();
	case 1: return lt_theme_terminal();
	case 2: return lt_theme_temple();
	case 3: return lt_theme_mono();
	case 4: return lt_theme_mono_reverse();
	}
	return from_pal(&PAL[i - NBASE]);
}
bool lt_theme_by_name(const char *name, lt_theme *out)
{
	for (int i = 0; i < lt_theme_count(); i++) if (!strcmp(name, lt_theme_name(i))) { *out = lt_theme_at(i); return true; }
	return false;
}

static lt_color color_of(const char *s, bool *ok)
{
	static const char *names[] = {"black", "red", "green", "yellow", "blue", "magenta", "cyan", "white"};
	*ok = true;
	if (!*s || !strcmp(s, "default")) return LT_DEFAULT;
	if (s[0] == '#' && strlen(s) == 7) { unsigned r, g, b; if (sscanf(s + 1, "%2x%2x%2x", &r, &g, &b) == 3) return LT_RGB(r, g, b); }
	for (int i = 0; i < 8; i++) {
		if (!strcmp(s, names[i])) return LT_PAL(i);
		char b[24]; snprintf(b, sizeof b, "bright-%s", names[i]);
		if (!strcmp(s, b)) return LT_PAL(i + 8);
	}
	char *e;
	long v = strtol(s, &e, 10);
	if (!*e && v >= 0 && v < 256) return LT_PAL(v);
	*ok = false;
	return LT_DEFAULT;
}

/* "base=nord,accent=#ff8800:bold,bar=green/236,grad=#00f;#0f0;#f00" : base=<theme> first, then slot=fg[/bg][:attr[:attr]] */
lt_theme lt_theme_parse(const char *spec)
{
	static const char *slots[LT_T_COUNT] = {"text", "dim", "accent", "ok", "warn", "err", "bar", "barbg", "border", "title", "keycap", "ctrl", "alt", "shift", "label", "select"};
	lt_theme t = lt_theme_default();
	char *c = strdup(spec), *save = NULL;
	for (char *tok = strtok_r(c, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
		char *eq = strchr(tok, '=');
		if (!eq) continue;
		*eq = 0;
		if (!strcmp(tok, "base")) { lt_theme b; if (lt_theme_by_name(eq + 1, &b)) t = b; continue; }
		if (!strcmp(tok, "grad")) {                                                     /* grad=#0000ff;#00ff00;#ff0000 (or "none") */
			t.ngrad = 0;
			char *sv = NULL;
			for (char *g = strtok_r(eq + 1, ";", &sv); g && t.ngrad < LT_GRAD_MAX; g = strtok_r(NULL, ";", &sv)) { bool ok; lt_color col = color_of(g, &ok); if (ok && col != LT_DEFAULT) t.grad[t.ngrad++] = col; }
			continue;
		}
		int slot = -1;
		for (int i = 0; i < LT_T_COUNT; i++) if (!strcmp(tok, slots[i])) slot = i;
		if (slot < 0) continue;
		char *attrs = strchr(eq + 1, ':');
		if (attrs) *attrs++ = 0;
		char *bg = strchr(eq + 1, '/');
		if (bg) *bg++ = 0;
		bool ok;
		lt_color fg = color_of(eq + 1, &ok);
		if (ok) t.s[slot].fg = fg;
		if (bg) { lt_color b = color_of(bg, &ok); if (ok) t.s[slot].bg = b; }
		if (attrs) {
			t.s[slot].attr = 0;
			char *sv = NULL;
			for (char *a = strtok_r(attrs, ":", &sv); a; a = strtok_r(NULL, ":", &sv)) {
				if (!strcmp(a, "bold")) t.s[slot].attr |= LT_BOLD; else if (!strcmp(a, "dim")) t.s[slot].attr |= LT_DIM;
				else if (!strcmp(a, "italic")) t.s[slot].attr |= LT_ITALIC; else if (!strcmp(a, "underline")) t.s[slot].attr |= LT_UNDERLINE;
				else if (!strcmp(a, "reverse")) t.s[slot].attr |= LT_REVERSE; else if (!strcmp(a, "strike")) t.s[slot].attr |= LT_STRIKE;
			}
		}
	}
	free(c);
	return t;
}
