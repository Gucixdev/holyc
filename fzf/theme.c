/* theme.c - the default (dark 256) theme of fzf and the --color parser. */
#include "ui.h"

static Col mk(int fg, int bg, int attr) { return (Col){fg, bg, attr}; }

void theme_default(Theme *t)
{
	for (int i = 0; i < C_COUNT; i++) t->c[i] = mk(COL_UNDEF, COL_UNDEF, 0);
	t->colored = true;
	t->c[C_FG] = mk(COL_DEF, COL_UNDEF, 0);
	t->c[C_BG] = mk(COL_UNDEF, COL_DEF, 0);
	t->c[C_HL] = mk(108, COL_UNDEF, 0);
	t->c[C_FGP] = mk(254, 236, ATTR_BOLD);
	t->c[C_HLP] = mk(151, COL_UNDEF, 0);
	t->c[C_INFO] = mk(144, COL_UNDEF, 0);
	t->c[C_BORDER] = mk(59, COL_UNDEF, 0);
	t->c[C_PROMPT] = mk(110, COL_UNDEF, ATTR_BOLD);
	t->c[C_POINTER] = mk(161, COL_UNDEF, ATTR_BOLD);
	t->c[C_MARKER] = mk(168, COL_UNDEF, 0);
	t->c[C_SPINNER] = mk(148, COL_UNDEF, 0);
	t->c[C_HEADER] = mk(109, COL_UNDEF, 0);
	t->c[C_FOOTER] = mk(109, COL_UNDEF, 0);
	t->c[C_LABEL] = mk(145, COL_UNDEF, 0);
	t->c[C_QUERY] = mk(COL_DEF, COL_UNDEF, ATTR_BOLD);
}

static void theme_16(Theme *t)
{
	theme_default(t);
	t->c[C_HL] = mk(2, COL_UNDEF, 0); t->c[C_FGP] = mk(15, 8, 0);
	t->c[C_HLP] = mk(10, COL_UNDEF, 0); t->c[C_INFO] = mk(3, COL_UNDEF, 0); t->c[C_BORDER] = mk(COL_UNDEF, COL_UNDEF, 0);
	t->c[C_PROMPT] = mk(4, COL_UNDEF, 0); t->c[C_POINTER] = mk(1, COL_UNDEF, 0); t->c[C_MARKER] = mk(5, COL_UNDEF, 0);
	t->c[C_SPINNER] = mk(2, COL_UNDEF, 0); t->c[C_HEADER] = mk(6, COL_UNDEF, 0); t->c[C_FOOTER] = mk(6, COL_UNDEF, 0);
	t->c[C_LABEL] = mk(COL_UNDEF, COL_UNDEF, 0);
}
static void theme_light(Theme *t)
{
	theme_default(t);
	t->c[C_HL] = mk(65, COL_UNDEF, 0); t->c[C_FGP] = mk(236, 254, 0);
	t->c[C_HLP] = mk(65, COL_UNDEF, 0); t->c[C_INFO] = mk(101, COL_UNDEF, 0); t->c[C_BORDER] = mk(145, COL_UNDEF, 0);
	t->c[C_PROMPT] = mk(110, COL_UNDEF, 0); t->c[C_POINTER] = mk(161, COL_UNDEF, 0); t->c[C_MARKER] = mk(168, COL_UNDEF, 0);
	t->c[C_SPINNER] = mk(148, COL_UNDEF, 0); t->c[C_HEADER] = mk(31, COL_UNDEF, 0); t->c[C_FOOTER] = mk(31, COL_UNDEF, 0);
	t->c[C_LABEL] = mk(145, COL_UNDEF, 0);
}

/* the TempleOS-like scheme (blue ground, yellow text) used by the dmenu rofi mode */
static void theme_temple(Theme *t)
{
	theme_default(t);
	t->c[C_FG] = mk(11, 4, 0); t->c[C_FGP] = mk(0, 14, ATTR_BOLD); t->c[C_HL] = mk(15, COL_UNDEF, ATTR_BOLD);
	t->c[C_HLP] = mk(4, COL_UNDEF, ATTR_BOLD); t->c[C_INFO] = mk(14, COL_UNDEF, 0); t->c[C_BORDER] = mk(14, COL_UNDEF, 0);
	t->c[C_PROMPT] = mk(15, COL_UNDEF, ATTR_BOLD); t->c[C_POINTER] = mk(12, COL_UNDEF, ATTR_BOLD); t->c[C_MARKER] = mk(10, COL_UNDEF, 0);
	t->c[C_HEADER] = mk(14, COL_UNDEF, 0); t->c[C_FOOTER] = mk(14, COL_UNDEF, 0); t->c[C_LABEL] = mk(15, COL_UNDEF, 0);
	t->c[C_GUTTER].fg = 4; t->c[C_GUTTER].bg = 4; t->c[C_QUERY] = mk(15, COL_UNDEF, ATTR_BOLD);
}

static int color_of(const char *s)
{
	static const char *names[] = {"black", "red", "green", "yellow", "blue", "magenta", "cyan", "white"};
	if (!*s) return COL_UNDEF;
	if (!strcmp(s, "-1") || !strcmp(s, "default")) return COL_DEF;
	for (int i = 0; i < 8; i++) {
		if (!strcmp(s, names[i])) return i;
		char b[24]; snprintf(b, sizeof b, "bright-%s", names[i]);
		if (!strcmp(s, b)) return i + 8;
	}
	if (!strcmp(s, "grey") || !strcmp(s, "gray")) return 8;
	if (s[0] == '#' && strlen(s) == 7) {
		unsigned r, g, b;
		if (sscanf(s + 1, "%2x%2x%2x", &r, &g, &b) == 3) return 0x1000000 | (int)(r << 16 | g << 8 | b);
	}
	char *e;
	long v = strtol(s, &e, 10);
	if (!*e && v >= 0 && v < 256) return (int)v;
	return COL_UNDEF;
}
static int attr_of(const char *s)
{
	if (!strcmp(s, "regular") || !strcmp(s, "normal")) return 0;
	static const struct { const char *n; int a; } at[] = {{"bold", ATTR_BOLD}, {"dim", ATTR_DIM}, {"italic", ATTR_ITAL}, {"underline", ATTR_UL},
		{"blink", ATTR_BLINK}, {"reverse", ATTR_REV}, {"strikethrough", ATTR_STRIKE}};
	for (size_t i = 0; i < sizeof at / sizeof at[0]; i++) if (!strcmp(s, at[i].n)) return at[i].a;
	return -1;
}

static int slot_of(const char *n, bool *bg)
{
	static const struct { const char *name; int slot; bool bg; } m[] = {
		{"fg", C_FG, false}, {"bg", C_FG, true}, {"hl", C_HL, false}, {"fg+", C_FGP, false}, {"bg+", C_FGP, true}, {"hl+", C_HLP, false},
		{"info", C_INFO, false}, {"border", C_BORDER, false}, {"prompt", C_PROMPT, false}, {"pointer", C_POINTER, false},
		{"marker", C_MARKER, false}, {"spinner", C_SPINNER, false}, {"header", C_HEADER, false}, {"footer", C_FOOTER, false},
		{"gutter", C_GUTTER, true}, {"query", C_QUERY, false}, {"disabled", C_DISABLED, false}, {"preview-fg", C_PFG, false},
		{"preview-bg", C_PFG, true}, {"label", C_LABEL, false}, {"separator", C_SEP, false}, {"scrollbar", C_SCROLL, false},
		{"preview-scrollbar", C_PSCROLL, false}, {"preview-border", C_PBORDER, false}, {"preview-label", C_PLABEL, false},
		{"selected-fg", C_SELFG, false}, {"selected-bg", C_SELFG, true}, {"ghost", C_GHOST, false}, {"nth", C_NTH, false},
		{"list-fg", C_FG, false}, {"list-bg", C_FG, true}, {"input-fg", C_QUERY, false}, {"input-bg", C_QUERY, true},
		{"header-bg", C_HEADER, true}, {"footer-bg", C_FOOTER, true}, {"border-label", C_LABEL, false},
		{"list-border", C_BORDER, false}, {"input-border", C_BORDER, false}, {"header-border", C_BORDER, false},
		{"footer-border", C_BORDER, false}, {"list-label", C_LABEL, false}, {"input-label", C_LABEL, false},
		{"header-label", C_LABEL, false}, {"footer-label", C_LABEL, false}};
	for (size_t i = 0; i < sizeof m / sizeof m[0]; i++)
		if (!strcmp(n, m[i].name)) { *bg = m[i].bg; return m[i].slot; }
	return -1;
}

void theme_apply(Theme *t, const char *spec)
{
	char *c = xstrdup(spec), *save = NULL;
	for (char *tok = strtok_r(c, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
		while (*tok == ' ') tok++;
		if (!strcmp(tok, "dark")) { theme_default(t); continue; }
		if (!strcmp(tok, "light")) { theme_light(t); continue; }
		if (!strcmp(tok, "temple")) { theme_temple(t); continue; }
		if (!strcmp(tok, "16")) { theme_16(t); continue; }
		if (!strcmp(tok, "bw")) { t->colored = false; continue; }
		char *colon = strchr(tok, ':');
		if (!colon) { die("invalid color specification: %s", tok); }
		*colon = 0;
		bool bg;
		int slot = slot_of(tok, &bg);
		if (slot < 0) die("invalid color specification: %s", tok);
		char *rest = colon + 1, *sv2 = NULL;
		bool first = true;
		int attr = 0;
		bool have_attr = false;
		int colv = COL_UNDEF;
		for (char *p = strtok_r(rest, ":", &sv2); p; p = strtok_r(NULL, ":", &sv2)) {
			int a = first ? -1 : attr_of(p);
			if (first) { colv = color_of(p); if (colv == COL_UNDEF && *p) { a = attr_of(p); if (a >= 0) { attr |= a; have_attr = true; } } }
			else if (a >= 0) { attr |= a; have_attr = true; }
			first = false;
		}
		/* "bg" and "bg+" style names set the background of the slot; foreground ones the foreground */
		if (bg) { if (colv != COL_UNDEF) t->c[slot].bg = colv; }
		else { if (colv != COL_UNDEF) t->c[slot].fg = colv; if (have_attr) t->c[slot].attr = attr; }
	}
	free(c);
}

/* inherited slots: pointer/marker/gutter and friends take from the list colours */
static void inherit(Col *d, const Col *p)
{
	if (d->fg == COL_UNDEF) d->fg = p->fg;
	if (d->bg == COL_UNDEF) d->bg = p->bg;
}
void theme_resolve(Theme *t)
{
	Col *c = t->c;
	if (!t->colored) { for (int i = 0; i < C_COUNT; i++) c[i] = mk(COL_DEF, COL_DEF, 0); c[C_HL].attr = ATTR_BOLD | ATTR_UL; c[C_FGP].attr = ATTR_REV; c[C_POINTER].attr = ATTR_BOLD; return; }
	if (c[C_FG].fg == COL_UNDEF) c[C_FG].fg = COL_DEF;
	if (c[C_FG].bg == COL_UNDEF) c[C_FG].bg = COL_DEF;
	if (c[C_FGP].bg == COL_UNDEF) c[C_FGP].bg = 236;
	if (c[C_GUTTER].bg == COL_UNDEF) c[C_GUTTER].bg = c[C_FGP].bg;
	if (c[C_GUTTER].fg == COL_UNDEF) c[C_GUTTER].fg = c[C_FGP].bg;
	inherit(&c[C_POINTER], &c[C_FGP]);
	if (c[C_POINTER].bg == COL_UNDEF) c[C_POINTER].bg = c[C_FGP].bg;
	if (c[C_MARKER].bg == COL_UNDEF) c[C_MARKER].bg = c[C_FGP].bg;
	inherit(&c[C_SEP], &c[C_BORDER]);
	inherit(&c[C_SCROLL], &c[C_BORDER]);
	inherit(&c[C_PBORDER], &c[C_BORDER]);
	inherit(&c[C_PSCROLL], &c[C_PBORDER]);
	inherit(&c[C_PFG], &c[C_FG]);
	inherit(&c[C_PLABEL], &c[C_LABEL]);
	inherit(&c[C_DISABLED], &c[C_INFO]);
	inherit(&c[C_GHOST], &c[C_DISABLED]);
	if (c[C_QUERY].fg == COL_UNDEF) c[C_QUERY].fg = COL_DEF;
	for (int i = 0; i < C_COUNT; i++) { if (c[i].fg == COL_UNDEF) c[i].fg = COL_DEF; if (c[i].bg == COL_UNDEF) c[i].bg = COL_DEF; }
	/* the background of the bg+ slot is the current line, stored in C_BGP.bg; fg+ text uses C_FGP.fg */
}

static void emit_col(Buf *b, int v, bool bg)
{
	if (v == COL_DEF || v == COL_UNDEF) return;
	if (v & 0x1000000) buf_printf(b, ";%d;2;%d;%d;%d", bg ? 48 : 38, (v >> 16) & 255, (v >> 8) & 255, v & 255);
	else if (v < 8) buf_printf(b, ";%d", (bg ? 40 : 30) + v);
	else if (v < 16) buf_printf(b, ";%d", (bg ? 100 : 90) + v - 8);
	else buf_printf(b, ";%d;5;%d", bg ? 48 : 38, v);
}
void sgr_emit(Buf *b, Col c)
{
	buf_adds(b, "\033[0");
	if (c.attr & ATTR_BOLD) buf_adds(b, ";1");
	if (c.attr & ATTR_DIM) buf_adds(b, ";2");
	if (c.attr & ATTR_ITAL) buf_adds(b, ";3");
	if (c.attr & ATTR_UL) buf_adds(b, ";4");
	if (c.attr & ATTR_BLINK) buf_adds(b, ";5");
	if (c.attr & ATTR_REV) buf_adds(b, ";7");
	if (c.attr & ATTR_STRIKE) buf_adds(b, ";9");
	emit_col(b, c.fg, false);
	emit_col(b, c.bg, true);
	buf_addc(b, 'm');
}

/* One theme file for every tool: $FZF_THEME_FILE, else ~/.config/fzf/theme. It holds a --color value
 * (a preset name such as dark/light/16/temple/bw and/or slot:colour pairs, separated by commas or newlines). */
void theme_load_file(Theme *t)
{
	const char *p = getenv("FZF_THEME_FILE");
	char path[1024];
	if (!p || !*p) {
		const char *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
		if (x && *x) snprintf(path, sizeof path, "%s/fzf/theme", x);
		else if (h) snprintf(path, sizeof path, "%s/.config/fzf/theme", h);
		else return;
		p = path;
	}
	FILE *f = fopen(p, "r");
	if (!f) return;
	Buf b;
	buf_init(&b);
	char line[512];
	while (fgets(line, sizeof line, f)) {
		size_t n = strlen(line);
		while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = 0;
		char *s = line;
		while (*s == ' ' || *s == '\t') s++;
		if (!*s || *s == '#') continue;
		if (b.len) buf_addc(&b, ',');
		buf_adds(&b, s);
	}
	fclose(f);
	if (b.len) theme_apply(t, b.p);
	buf_free(&b);
}
