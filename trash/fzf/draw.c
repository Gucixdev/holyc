/* draw.c - the frame: a grid of cells filled by the layout code and written to the terminal in one go. */
#include "uistate.h"

/* ------------------------------------------------------------------ grid */
Col col_mk(int fg, int bg, int attr) { return (Col){fg, bg, attr}; }
Col col_mix(Col base, Col over)      /* over wins where it says something */
{
	Col r = base;
	if (over.fg != COL_DEF && over.fg != COL_UNDEF) r.fg = over.fg;
	if (over.bg != COL_DEF && over.bg != COL_UNDEF) r.bg = over.bg;
	r.attr |= over.attr;
	return r;
}

void grid_init(int w, int h)
{
	free(U.g.cell);
	U.g.w = w; U.g.h = h;
	U.g.cell = xmalloc((size_t)w * (size_t)h * sizeof(Cell));
}

static void grid_clear(void)
{
	Col c = col_mk(COL_DEF, U.th.c[C_FG].bg, 0);
	for (int i = 0; i < U.g.w * U.g.h; i++) { U.g.cell[i].ch = ' '; U.g.cell[i].c = c; }
}

static inline bool inr(Rect r, int x, int y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

/* --no-unicode: the drawing characters fzf uses, as ASCII */
static rune ascii_of(rune ch)
{
	switch (ch) {
	case 0x2500: case 0x2501: case 0x2550: return '-';
	case 0x2502: case 0x2503: case 0x2551: case 0x2590: return '|';
	case 0x250C: case 0x2510: case 0x2514: case 0x2518: case 0x256D: case 0x256E: case 0x256F: case 0x2570: case 0x250F: case 0x2513: case 0x2517: case 0x251B:
	case 0x2554: case 0x2557: case 0x255A: case 0x255D: return '+';
	case 0x258C: return '>';
	case 0x2191: return '^';
	case 0x2193: return 'v';
	case 0x2190: return '<';
	case 0x2192: return '>';
	case 0xD7: return 'x';
	}
	return ch > 127 ? '?' : ch;
}

static void gput(Rect clip, int x, int y, rune ch, Col c)
{
	if (U.o->ascii && ch > 127 && ch < 0x3000 && !(ch >= 0xA0 && ch < 0x2000)) ch = ascii_of(ch);   /* keep letters (Latin, Cyrillic...), replace symbols */
	int w = rune_width(ch);
	if (w == 0) return;
	if (!inr(clip, x, y) || x >= U.g.w || y >= U.g.h) return;
	if (w == 2 && !inr(clip, x + 1, y)) { ch = ' '; w = 1; }
	Cell *cl = &U.g.cell[y * U.g.w + x];
	cl->ch = ch; cl->c = c;
	if (w == 2) { cl[1].ch = 0; cl[1].c = c; }
}

/* draws s[0..n) at (x,y) in clip, at most maxw columns; returns the columns used */
static int gstr(Rect clip, int x, int y, const char *s, size_t n, Col c, int maxw)
{
	int used = 0;
	for (size_t i = 0; i < n;) {
		rune r;
		int l = u8dec(s + i, n - i, &r);
		i += (size_t)l;
		if (r < 32 || r == 127) r = ' ';
		int w = rune_width(r);
		if (used + w > maxw) break;
		gput(clip, x + used, y, r, c);
		used += w;
	}
	return used;
}
int gs(Rect clip, int x, int y, const char *s, Col c) { return gstr(clip, x, y, s, strlen(s), c, 1 << 20); }

void gfill(Rect r, Col c)
{
	for (int y = r.y; y < r.y + r.h; y++)
		for (int x = r.x; x < r.x + r.w; x++)
			if (x >= 0 && y >= 0 && x < U.g.w && y < U.g.h) { Cell *cl = &U.g.cell[y * U.g.w + x]; cl->ch = ' '; cl->c = c; }
}

/* ------------------------------------------------------------------ borders */
int border_edges(int s, int *t, int *r, int *b, int *l)
{
	*t = *r = *b = *l = 0;
	switch (s) {
	case BORDER_NONE: break;
	case BORDER_HORIZONTAL: *t = *b = 1; break;
	case BORDER_VERTICAL: *r = *l = 1; break;
	case BORDER_TOP: *t = 1; break;
	case BORDER_BOTTOM: *b = 1; break;
	case BORDER_LEFT: *l = 1; break;
	case BORDER_RIGHT: *r = 1; break;
	default: *t = *r = *b = *l = 1;
	}
	return *t || *r || *b || *l;
}
static Rect border_inner(Rect r, int shape)
{
	int t, rr, b, l;
	border_edges(shape, &t, &rr, &b, &l);
	r.x += l * 2; r.w -= l * 2 + rr * 2;                     /* one column of padding next to a vertical edge */
	r.y += t; r.h -= t + b;
	if (r.w < 0) r.w = 0;
	if (r.h < 0) r.h = 0;
	return r;
}
static void draw_border(Rect r, int shape, Col c, const char *label, int labelpos)
{
	if (shape == BORDER_NONE || r.w < 1 || r.h < 1) return;
	int t, rr, b, l;
	border_edges(shape, &t, &rr, &b, &l);
	const char *g[6] = {"╭", "╮", "╰", "╯", "─", "│"};
	if (shape == BORDER_SHARP) { g[0] = "┌"; g[1] = "┐"; g[2] = "└"; g[3] = "┘"; }
	else if (shape == BORDER_BOLD) { g[0] = "┏"; g[1] = "┓"; g[2] = "┗"; g[3] = "┛"; g[4] = "━"; g[5] = "┃"; }
	else if (shape == BORDER_DOUBLE) { g[0] = "╔"; g[1] = "╗"; g[2] = "╚"; g[3] = "╝"; g[4] = "═"; g[5] = "║"; }
	Rect all = {0, 0, U.g.w, U.g.h};
	int x1 = r.x + r.w - 1, y1 = r.y + r.h - 1;
	for (int x = r.x; x <= x1; x++) {
		if (t) gs(all, x, r.y, g[4], c);
		if (b) gs(all, x, y1, g[4], c);
	}
	for (int y = r.y; y <= y1; y++) {
		if (l) gs(all, r.x, y, g[5], c);
		if (rr) gs(all, x1, y, g[5], c);
	}
	if (t && l) gs(all, r.x, r.y, g[0], c);
	if (t && rr) gs(all, x1, r.y, g[1], c);
	if (b && l) gs(all, r.x, y1, g[2], c);
	if (b && rr) gs(all, x1, y1, g[3], c);
	if (label && *label && t) {
		int lw = str_width(label, strlen(label));
		int room = r.w - 4;
		if (room > 0) {
			if (lw > room) lw = room;
			int x = labelpos < 0 ? x1 - 1 - lw - 1 : r.x + 2;
			if (labelpos > 0) x = r.x + labelpos + 1;
			Col lc = col_mix(col_mk(COL_DEF, U.th.c[C_FG].bg, 0), U.th.c[C_LABEL]);
			gstr(all, x, r.y, label, strlen(label), lc, lw);
		}
	}
}

/* ------------------------------------------------------------------ text with colours */
typedef struct { rune ch; int w; Col c; int bytepos; } Glyph;
static Glyph *gl_buf; static size_t gl_cap;

/* decodes text into glyphs: tabs expanded, escapes (spans) applied, match positions highlighted */
static size_t glyphs_of(const char *text, size_t tlen, const AnsiSpan *sp, uint32_t nsp, const int *pos, int npos,
                        Col base, Col hl, int tabstop, int *total_w)
{
	size_t n = 0;
	int w = 0, ri = 0, pi = 0;
	uint32_t si = 0;
	/* pos is sorted descending or unsorted: mark with a small bitmap-ish scan instead */
	for (size_t i = 0; i < tlen;) {
		rune r;
		int l = u8dec(text + i, tlen - i, &r);
		Col c = base;
		if (nsp) {
			while (si < nsp && sp[si].off + sp[si].len <= i) si++;
			if (si < nsp && sp[si].off <= i) {
				const AnsiSpan *s = &sp[si];
				int f = (int)s->fg, b = (int)s->bg;
				if (f != -1) c.fg = f;
				if (b != -1) c.bg = b;
				c.attr |= s->attr;
			}
		}
		bool m = false;
		for (pi = 0; pi < npos; pi++) if (pos[pi] == ri) { m = true; break; }
		if (m) c = col_mix(c, hl);
		int cells = 1;
		if (r == '\t') { cells = tabstop - (w % tabstop); r = ' '; }
		else if (r < 32 || r == 127) r = ' ';
		else cells = rune_width(r);
		if (cells == 0) { i += (size_t)l; ri++; continue; }
		if (r == ' ' && cells > 1) {                       /* a tab */
			for (int k = 0; k < cells; k++) {
				if (n == gl_cap) { gl_cap = gl_cap ? gl_cap * 2 : 1024; gl_buf = xrealloc(gl_buf, gl_cap * sizeof(Glyph)); }
				gl_buf[n++] = (Glyph){' ', 1, c, (int)i};
			}
		} else {
			if (n == gl_cap) { gl_cap = gl_cap ? gl_cap * 2 : 1024; gl_buf = xrealloc(gl_buf, gl_cap * sizeof(Glyph)); }
			gl_buf[n++] = (Glyph){r, cells, c, (int)i};
		}
		w += cells;
		i += (size_t)l; ri++;
	}
	*total_w = w;
	return n;
}

/* draws a line of glyphs into avail columns, scrolled to make column `want_end` visible, with ellipsis marks */
static void draw_glyphs(Rect clip, int x, int y, int avail, size_t n, int total, int want_end, bool right, Col fill)
{
	if (avail <= 0) return;
	const char *ell = U.o->ellipsis;
	int ew = str_width(ell, strlen(ell));
	size_t from = 0, to = n;
	bool cut_l = false, cut_r = false;
	if (total > avail) {
		int off = 0;
		if (right) off = total - avail;
		else if (want_end > avail - ew) { off = want_end - avail + 10; if (off > total - avail) off = total - avail; if (off < 0) off = 0; }
		int acc = 0;
		while (from < n && acc < off) acc += gl_buf[from++].w;
		cut_l = from > 0;
		int used = 0, lead = cut_l ? ew : 0;
		size_t k = from;
		int rem = avail - lead;
		while (k < n && used + gl_buf[k].w <= rem) used += gl_buf[k++].w;
		to = k;
		cut_r = to < n;
		if (cut_r) {                                       /* make room for the right ellipsis */
			while (to > from && used > rem - ew) used -= gl_buf[--to].w;
		}
	}
	int cx = x;
	if (cut_l) { cx += gstr(clip, cx, y, ell, strlen(ell), fill, ew); }
	for (size_t k = from; k < to; k++) { gput(clip, cx, y, gl_buf[k].ch, gl_buf[k].c); cx += gl_buf[k].w; }
	if (cut_r) gstr(clip, cx, y, ell, strlen(ell), fill, x + avail - cx);
}

/* an escape-carrying string (header, footer, preview line) drawn as-is */
static void draw_ansi(Rect clip, int x, int y, int avail, const char *raw, size_t n, Col base)
{
	Buf b;
	buf_init(&b);
	AnsiSpan *sp = NULL;
	uint32_t ns = 0, cap = 0;
	ansi_extract(raw, n, &b, &sp, &ns, &cap);
	int total;
	size_t gn = glyphs_of(b.p ? b.p : "", b.len, sp, ns, NULL, 0, base, base, U.o->tabstop, &total);
	(void)gn;
	draw_glyphs(clip, x, y, avail, gn, total, 0, false, base);
	free(sp);
	buf_free(&b);
}

/* ------------------------------------------------------------------ list */
static int ptr_w, mrk_w;

static void draw_item_row(Rect win, int y, size_t hi, bool cur, int text_x, int avail_w)
{
	Item *it = U.items.v[U.hits[hi].idx];
	bool sel = (it->flags & ITEM_SEL) != 0;
	Theme *T = &U.th;
	Col plain = col_mk(T->c[C_FG].fg, T->c[C_FG].bg, T->c[C_FG].attr);
	Col base = plain, hlc = col_mk(T->c[C_HL].fg, COL_DEF, T->c[C_HL].attr);
	if (cur) {
		base = col_mk(T->c[C_FGP].fg, T->c[C_FGP].bg, T->c[C_FGP].attr);
		hlc = col_mk(T->c[C_HLP].fg, COL_DEF, T->c[C_HLP].attr);
	} else if (sel && (T->c[C_SELFG].fg != COL_DEF || T->c[C_SELFG].bg != COL_DEF)) {
		base = col_mix(base, T->c[C_SELFG]);
	}
	Col ptr = col_mix(base, T->c[C_POINTER]); ptr.bg = cur ? T->c[C_FGP].bg : plain.bg;
	Col mrk = col_mix(base, T->c[C_MARKER]); mrk.bg = cur ? T->c[C_FGP].bg : plain.bg;
	Col gut = col_mk(T->c[C_GUTTER].fg, plain.bg, 0);
	int x = win.x;
	if (cur) { ptr.attr |= 0; gs(win, x, y, U.o->pointer, ptr); }
	else if (!strcmp(U.o->pointer, "▌")) gs(win, x, y, U.o->pointer, gut);
	x += ptr_w;
	if (sel) gs(win, x, y, U.o->marker, mrk);
	else if (cur) gfill((Rect){x, y, mrk_w, 1}, col_mk(COL_DEF, base.bg, 0));
	x += mrk_w;
	if (cur && U.o->highlight_line) gfill((Rect){x, y, win.x + win.w - x, 1}, base);
	if (cur) gfill((Rect){win.x + 0, y, 0, 0}, base);
	MatchOut mo;
	int *pos = NULL, npos = 0, maxe = 0;
	if (!pattern_empty(U.pat) && pattern_match(U.pat, it, U.sc, true, &mo)) { pos = mo.pos; npos = mo.npos; maxe = mo.maxe; }
	int total;
	size_t n = glyphs_of(it->text, it->tlen, U.o->ansi ? it->ansi : NULL, U.o->ansi ? it->nansi : 0, pos, npos, base, hlc,
	                     U.o->tabstop, &total);
	int endcol = 0;
	if (maxe > 0) { int a = 0; for (size_t k = 0; k < n && (int)k < maxe; k++) a += gl_buf[k].w; endcol = a + 1; }
	if (cur) gfill((Rect){text_x, y, total < avail_w ? total : avail_w, 1}, base);
	draw_glyphs(win, text_x, y, avail_w, n, total, endcol, U.o->keep_right, plain);
}

static void draw_scrollbar(Rect r, int shown, size_t total, size_t top)
{
	if (total <= (size_t)shown || shown < 1) return;
	int th = (int)((size_t)shown * (size_t)shown / total);
	if (th < 1) th = 1;
	int tp = (int)((size_t)(shown - th) * top / (total - (size_t)shown));
	Col c = col_mk(U.th.c[C_SCROLL].fg, U.th.c[C_FG].bg, 0);
	for (int i = 0; i < th; i++) gs(r, r.x + r.w - 1, r.y + tp + i, "│", c);
}

/* --wrap: an item takes as many rows as its text needs; continuation lines start with "↳ " */
static int wrap_split(size_t n, int avail, int *starts, int maxlines)
{
	int lines = 1, w = 0, lim = avail;
	if (starts) starts[0] = 0;
	for (size_t k = 0; k < n; k++) {
		if (w + gl_buf[k].w > lim && w > 0) {
			if (starts && lines < maxlines) starts[lines] = (int)k;
			lines++; w = 0; lim = avail - 2 > 0 ? avail - 2 : 1;
		}
		w += gl_buf[k].w;
	}
	return lines;
}

static int item_lines(size_t hi, int avail)
{
	Item *it = U.items.v[U.hits[hi].idx];
	int total;
	Col c = col_mk(COL_DEF, COL_DEF, 0);
	size_t n = glyphs_of(it->text, it->tlen, NULL, 0, NULL, 0, c, c, U.o->tabstop, &total);
	return total <= avail ? 1 : wrap_split(n, avail, NULL, 0);
}

static void draw_list_wrap(Rect r, bool bottom_up)
{
	int avail = r.w - ptr_w - mrk_w - 1;                 /* the last column stays free, like fzf */
	if (avail < 4) avail = 4;
	free(U.rowmap);
	U.rowmap = xmalloc((size_t)r.h * sizeof(size_t));
	for (int i = 0; i < r.h; i++) U.rowmap[i] = (size_t)-1;
	if (!U.nhits) return;
	/* scroll so that the whole current item is visible */
	if (U.cur < U.top) U.top = U.cur;
	for (;;) {
		int rows = 0;
		for (size_t i = U.top; i <= U.cur; i++) rows += item_lines(i, avail);
		if (rows <= r.h || U.top >= U.cur) break;
		U.top++;
	}
	Theme *T = &U.th;
	Col plain = col_mk(T->c[C_FG].fg, T->c[C_FG].bg, T->c[C_FG].attr);
	int used = 0;
	for (size_t hi = U.top; hi < U.nhits && used < r.h; hi++) {
		Item *it = U.items.v[U.hits[hi].idx];
		bool cur = hi == U.cur, sel = (it->flags & ITEM_SEL) != 0;
		Col base = plain, hlc = col_mk(T->c[C_HL].fg, COL_DEF, T->c[C_HL].attr);
		if (cur) { base = col_mk(T->c[C_FGP].fg, T->c[C_FGP].bg, T->c[C_FGP].attr); hlc = col_mk(T->c[C_HLP].fg, COL_DEF, T->c[C_HLP].attr); }
		Col ptr = col_mix(base, T->c[C_POINTER]); ptr.bg = cur ? T->c[C_FGP].bg : plain.bg;
		Col mrk = col_mix(base, T->c[C_MARKER]); mrk.bg = cur ? T->c[C_FGP].bg : plain.bg;
		Col gut = col_mk(T->c[C_GUTTER].fg, plain.bg, 0);
		MatchOut mo;
		int *pos = NULL, npos = 0;
		if (!pattern_empty(U.pat) && pattern_match(U.pat, it, U.sc, true, &mo)) { pos = mo.pos; npos = mo.npos; }
		int total;
		size_t n = glyphs_of(it->text, it->tlen, U.o->ansi ? it->ansi : NULL, U.o->ansi ? it->nansi : 0, pos, npos, base, hlc, U.o->tabstop, &total);
		int starts[64];
		int lines = total <= avail ? 1 : wrap_split(n, avail, starts, 64);
		if (total <= avail) starts[0] = 0;
		int cap = lines < 64 ? lines : 64;
		for (int ln = 0; ln < cap; ln++) {
			int row = used + ln;                              /* rows from the list's start edge */
			int y = bottom_up ? r.y + r.h - used - lines + ln : r.y + row;
			if (row >= r.h || y < r.y) continue;
			U.rowmap[y - r.y] = hi;
			int x = r.x;
			if (cur) gs(r, x, y, U.o->pointer, ptr);
			else if (!strcmp(U.o->pointer, "▌")) gs(r, x, y, U.o->pointer, gut);
			x += ptr_w;
			if (sel) gs(r, x, y, U.o->marker, mrk);
			else if (cur) gfill((Rect){x, y, mrk_w, 1}, col_mk(COL_DEF, base.bg, 0));
			x += mrk_w;
			size_t from = (size_t)starts[ln], to = ln + 1 < lines && ln + 1 < cap ? (size_t)starts[ln + 1] : n;
			int lw = 0;
			for (size_t k = from; k < to; k++) lw += gl_buf[k].w;
			int tx = x;
			if (ln > 0) { Col wc = base; if (cur) gfill((Rect){tx, y, 2, 1}, base); gs(r, tx, y, "↳ ", wc); tx += 2; }
			if (cur) gfill((Rect){tx, y, lw, 1}, base);
			for (size_t k = from; k < to; k++) { gput(r, tx, y, gl_buf[k].ch, gl_buf[k].c); tx += gl_buf[k].w; }
		}
		used += lines;
	}
	if (U.nhits < 20000) {                                 /* scrollbar, measured in rows */
		size_t total_rows = 0, before = 0;
		for (size_t i = 0; i < U.nhits; i++) { int l = item_lines(i, avail); total_rows += (size_t)l; if (i < U.top) before += (size_t)l; }
		if (total_rows > (size_t)r.h) {
			int th = (int)((size_t)r.h * (size_t)r.h / total_rows); if (th < 1) th = 1;
			int tp = (int)((size_t)(r.h - th) * before / (total_rows - (size_t)r.h));
			Col c = col_mk(U.th.c[C_SCROLL].fg, U.th.c[C_FG].bg, 0);
			for (int i = 0; i < th; i++) {
				int row = bottom_up ? r.h - 1 - (tp + i) : tp + i;
				gs((Rect){0, 0, U.g.w, U.g.h}, U.list_padbar ? r.x + r.w : r.x + r.w - 1, r.y + row, "│", c);
			}
		}
	}
}

static void draw_list(Rect r, bool bottom_up)
{
	U.list_rect = r;
	if (r.h <= 0 || r.w <= 0) return;
	ptr_w = str_width(U.o->pointer, strlen(U.o->pointer));
	mrk_w = str_width(U.o->marker, strlen(U.o->marker));
	if (U.o->wrap) { draw_list_wrap(r, bottom_up); return; }
	{                                                     /* keep the cursor inside the window, --scroll-off rows from the edges */
		size_t rows = (size_t)r.h, so = (size_t)U.o->scroll_off;
		if (so > (rows - 1) / 2) so = (rows - 1) / 2;
		if (U.cur < U.top + so) U.top = U.cur >= so ? U.cur - so : 0;
		if (U.cur + so + 1 > U.top + rows) U.top = U.cur + so + 1 - rows;
		if (U.nhits <= rows) U.top = 0;
		else if (U.top > U.nhits - rows) U.top = U.nhits - rows;
	}
	bool bar = U.nhits > (size_t)r.h;
	int avail = r.w - ptr_w - mrk_w - (bar && !U.list_padbar ? 1 : 0);
	for (int i = 0; i < r.h; i++) {
		size_t hi = U.top + (size_t)i;
		if (hi >= U.nhits) break;
		int y = bottom_up ? r.y + r.h - 1 - i : r.y + i;
		draw_item_row(r, y, hi, hi == U.cur, r.x + ptr_w + mrk_w, avail);
	}
	if (bar) {
		Rect br = r; br.w -= 0;
		int th = (int)((size_t)r.h * (size_t)r.h / U.nhits); if (th < 1) th = 1;
		int tp = (int)((size_t)(r.h - th) * U.top / (U.nhits - (size_t)r.h));
		Col c = col_mk(U.th.c[C_SCROLL].fg, U.th.c[C_FG].bg, 0);
		for (int i = 0; i < th; i++) {
			int row = bottom_up ? r.h - 1 - (tp + i) : tp + i;
			gs((Rect){0, 0, U.g.w, U.g.h}, U.list_padbar ? r.x + r.w : r.x + r.w - 1, r.y + row, "│", c);
		}
	}
	(void)draw_scrollbar;
}

/* ------------------------------------------------------------------ preview */
static void pv_line(size_t i, const char **s, size_t *n)
{
	*s = U.pv_out.p + U.pv_lines[i];
	size_t e = (i + 1 < U.pv_nl) ? U.pv_lines[i + 1] : U.pv_out.len;
	if (e > U.pv_lines[i] && U.pv_out.p[e - 1] == '\n') e--;
	if (e > U.pv_lines[i] && U.pv_out.p[e - 1] == '\r') e--;
	*n = e - U.pv_lines[i];
}

static void draw_preview(Rect outer)
{
	U.pv_rect = outer;
	Col pfg = col_mix(col_mk(COL_DEF, U.th.c[C_FG].bg, 0), U.th.c[C_PFG]);
	if (U.th.c[C_PFG].bg != COL_DEF) pfg.bg = U.th.c[C_PFG].bg;
	Col bc = col_mix(col_mk(COL_DEF, pfg.bg, 0), U.th.c[C_PBORDER]);
	Rect in = border_inner(outer, U.pw_active.border);
	gfill(outer, pfg);
	draw_border(outer, U.pw_active.border, bc, U.preview_label, U.o->border_label_pos);
	if (in.w <= 0 || in.h <= 0) { U.pv_rect = in; return; }
	U.pv_rect = in;
	int hl = U.pw_active.header_lines;
	if ((size_t)hl > U.pv_nl) hl = (int)U.pv_nl;
	int y = in.y, room = in.h;
	bool wrap = U.pv_wrap;
	size_t total = U.pv_nl;
	/* fixed header lines first */
	for (int i = 0; i < hl && room > 0; i++, y++, room--) {
		const char *s; size_t n;
		pv_line((size_t)i, &s, &n);
		draw_ansi(in, in.x, y, in.w, s, n, pfg);
	}
	size_t line = (size_t)hl + (size_t)U.pv_scroll;
	int maxs = (int)total - hl - room;
	if (maxs < 0) maxs = 0;
	if (U.pv_scroll > maxs && !wrap) U.pv_scroll = maxs;
	line = (size_t)hl + (size_t)U.pv_scroll;
	for (; line < total && room > 0; line++) {
		const char *s; size_t n;
		pv_line(line, &s, &n);
		if (!wrap) {
			Buf b; buf_init(&b);
			AnsiSpan *sp = NULL; uint32_t ns = 0, cap = 0;
			ansi_extract(s, n, &b, &sp, &ns, &cap);
			int tw;
			size_t gn = glyphs_of(b.p ? b.p : "", b.len, sp, ns, NULL, 0, pfg, pfg, U.o->tabstop, &tw);
			int off = U.pv_hscroll;
			size_t from = 0; int acc = 0;
			while (from < gn && acc < off) acc += gl_buf[from++].w;
			int cx = in.x;
			for (size_t k = from; k < gn && cx - in.x + gl_buf[k].w <= in.w; k++) { gput(in, cx, y, gl_buf[k].ch, gl_buf[k].c); cx += gl_buf[k].w; }
			free(sp); buf_free(&b);
			y++; room--;
		} else {
			Buf b; buf_init(&b);
			AnsiSpan *sp = NULL; uint32_t ns = 0, cap = 0;
			ansi_extract(s, n, &b, &sp, &ns, &cap);
			int tw;
			size_t gn = glyphs_of(b.p ? b.p : "", b.len, sp, ns, NULL, 0, pfg, pfg, U.o->tabstop, &tw);
			size_t k = 0;
			do {
				int cx = in.x;
				while (k < gn && cx - in.x + gl_buf[k].w <= in.w) { gput(in, cx, y, gl_buf[k].ch, gl_buf[k].c); cx += gl_buf[k].w; k++; }
				y++; room--;
			} while (k < gn && room > 0);
			free(sp); buf_free(&b);
		}
	}
	if (total > (size_t)in.h) {
		int t, rr, bb, l;
		border_edges(U.pw_active.border, &t, &rr, &bb, &l);
		int bx = rr ? in.x + in.w : in.x + in.w - 1;               /* the padding column next to the border */
		int th = (int)((size_t)in.h * (size_t)in.h / total);
		if (th < 1) th = 1;
		int maxs = (int)total - in.h;
		int tp = maxs > 0 ? (int)((size_t)(in.h - th) * (size_t)(hl + U.pv_scroll) / (size_t)maxs) : 0;
		Col sc = col_mk(U.th.c[C_PSCROLL].fg, pfg.bg, 0);
		for (int i = 0; i < th; i++) gs((Rect){0, 0, U.g.w, U.g.h}, bx, in.y + tp + i, "│", sc);
		if (U.pw_active.info) {
			char buf[48];
			snprintf(buf, sizeof buf, "%zu/%zu", (size_t)(hl + U.pv_scroll + 1), total);
			Col ic = col_mk(U.th.c[C_INFO].fg, pfg.bg, U.th.c[C_INFO].attr | ATTR_REV);
			int end = rr ? in.x + in.w : in.x + in.w - 1;
			gs((Rect){0, 0, U.g.w, U.g.h}, end - (int)strlen(buf), in.y, buf, ic);
		}
	}
}

/* ------------------------------------------------------------------ frame */
static PW *pw_pick(int total_cols)
{
	PW *p = &U.pw;
	if (p->alt && total_cols < p->threshold) return p->alt;
	return p;
}

static int size_of(int total, int size, bool pct)
{
	int v = pct ? total * size / 100 : size;
	if (v < 1) v = 1;
	if (v > total) v = total;
	return v;
}

static int text_lines(const char *s)
{
	if (!s || !*s) return 0;
	int n = 1;
	for (; *s; s++) if (*s == '\n') n++;
	return n;
}

static void draw_query_line(Rect r, int *cursor_x, int *cursor_y, bool with_info, const char *info)
{
	U.prompt_rect = r;
	Col pc = col_mk(U.th.c[C_PROMPT].fg, U.th.c[C_FG].bg, U.th.c[C_PROMPT].attr);
	Col qc = col_mk(U.th.c[C_QUERY].fg, U.th.c[C_FG].bg, U.th.c[C_QUERY].attr);
	Col fill = col_mk(COL_DEF, U.th.c[C_FG].bg, 0);
	gfill(r, fill);
	if (U.hide_input || U.o->no_input) return;
	int pw = 0;
	for (const char *p = U.prompt; *p;) {          /* blanks of the prompt are not bold */
		rune ch; int l = u8dec(p, strlen(p), &ch);
		Col c1 = pc;
		if (ch == ' ') c1.attr &= ~ATTR_BOLD;
		pw += gstr(r, r.x + pw, r.y, p, (size_t)l, c1, 1 << 20);
		p += l;
	}
	/* query: scroll horizontally when it does not fit */
	int qw = 0;
	for (int i = 0; i < U.qn; i++) qw += rune_width(U.q[i]) ? rune_width(U.q[i]) : 0;
	int room = r.w - pw - 1;
	int skip = 0, cw = 0;
	for (int i = 0; i < U.qc; i++) cw += rune_width(U.q[i]);
	if (cw > room - 1) skip = cw - (room - 1);
	int x = r.x + pw, acc = 0, curx = r.x + pw;
	for (int i = 0; i < U.qn; i++) {
		int w = rune_width(U.q[i]);
		if (acc >= skip && x - r.x + w <= r.w) { gput(r, x, r.y, U.q[i], qc); x += w; }
		acc += w;
		if (i + 1 == U.qc) curx = x;
	}
	if (U.qc == 0) curx = r.x + pw;
	*cursor_x = curx; *cursor_y = r.y;
	if (U.qn == 0 && U.ghost && *U.ghost) {
		Col gc = col_mk(U.th.c[C_GHOST].fg, U.th.c[C_FG].bg, U.th.c[C_GHOST].attr);
		gstr(r, r.x + pw, r.y, U.ghost, strlen(U.ghost), gc, room);
	}
	(void)qw;
	if (with_info && info) {                       /* inline styles: the info follows the query on the same line */
		int ix = r.x + pw + (U.qn ? (x - r.x - pw) : 0) + 1;
		const char *prefix = U.o->info == INFO_INLINE ? " < " : "";
		Col ic = col_mk(U.th.c[C_INFO].fg, U.th.c[C_FG].bg, U.th.c[C_INFO].attr);
		Col sc = col_mk(U.th.c[C_SEP].fg, U.th.c[C_FG].bg, 0);
		if (U.o->info == INFO_INLINE_RIGHT) ix = r.x + r.w - (int)strlen(info) - 2;
		if (ix < r.x + pw + 1) ix = r.x + pw + 1;
		int used = gs(r, ix, r.y, prefix, U.feed_done ? pc : col_mk(U.th.c[C_SPINNER].fg, U.th.c[C_FG].bg, 0));
		ix += used;
		used = gstr(r, ix, r.y, info, strlen(info), ic, r.x + r.w - ix - 1);
		ix += used + 1;
		if (U.o->info == INFO_INLINE && U.info_rule) for (int k = ix; k < r.x + r.w - (U.list_padbar ? 0 : 1); k++) gs(r, k, r.y, "─", sc);
	}
}

static void build_info(char *out, size_t cap)
{
	size_t total = U.items.n;
	size_t found = U.nhits;
	if (total < found) total = found;
	int n = snprintf(out, cap, "%zu/%zu", found, total);
	if (U.o->multi) {
		if (U.o->multi_max) n += snprintf(out + n, cap - (size_t)n, " (%zu/%d)", U.nsel, U.o->multi_max);
		else n += snprintf(out + n, cap - (size_t)n, " (%zu)", U.nsel);
	}
	if (!U.sort && U.o->sort == false) { /* --no-sort from the start is not shown */ }
	if (U.track_on) n += snprintf(out + n, cap - (size_t)n, " +T");
	(void)n;
}

static void draw_info_line(Rect r)
{
	U.info_rect = r;
	Col fill = col_mk(COL_DEF, U.th.c[C_FG].bg, 0);
	gfill(r, fill);
	char info[96];
	build_info(info, sizeof info);
	Col ic = col_mk(U.th.c[C_INFO].fg, fill.bg, U.th.c[C_INFO].attr);
	Col sc = col_mk(U.th.c[C_SEP].fg, fill.bg, 0);
	Col spin = col_mk(U.th.c[C_SPINNER].fg, fill.bg, U.th.c[C_SPINNER].attr);
	int x = r.x;
	if (U.o->info == INFO_HIDDEN) { for (int k = 0; U.info_rule && k < r.w; k++) gs(r, r.x + k, r.y, "─", sc); return; }
	bool loading = !U.feed_done;
	if (U.o->info == INFO_RIGHT) {
		int len = (int)strlen(info);
		int start = r.x + r.w - len - 2;
		for (int k = r.x; U.info_rule && k < start; k++) gs(r, k, r.y, "─", sc);
		gs(r, start + 1, r.y, info, ic);
		return;
	}
	static const char *sp[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
	static const char *sa[] = {"|", "/", "-", "\\"};
	gs(r, x, r.y, loading ? (U.o->ascii ? sa[(int)(U.now * 10) % 4] : sp[(int)(U.now * 10) % 10]) : " ", spin);
	x += 2;
	x += gs(r, x, r.y, info, ic) + 1;
	for (int k = x; U.info_rule && k < r.x + r.w - (U.list_padbar ? 0 : 1); k++) gs(r, k, r.y, "─", sc);
}

static void draw_hdr_lines(Rect r, bool bottom_up)
{
	U.hdr_rect = r;
	Col hc = col_mk(U.th.c[C_HEADER].fg, U.th.c[C_HEADER].bg == COL_DEF ? U.th.c[C_FG].bg : U.th.c[C_HEADER].bg, U.th.c[C_HEADER].attr);
	gfill(r, hc);
	int ind = U.o->header_border != BORDER_NONE ? 0 : ptr_w + mrk_w, y = r.y;
	bool rev = U.o->layout != LAYOUT_REVERSE;              /* default and reverse-list read the --header-lines bottom-up */
	for (int pass = 0; pass < 2; pass++) {
		bool items = rev ? pass == 0 : pass == 1;
		if (!items) {
			if (!U.header) continue;
			const char *p = U.header;
			while (*p && y < r.y + r.h) {
				const char *e = strchr(p, '\n');
				size_t n = e ? (size_t)(e - p) : strlen(p);
				draw_ansi(r, r.x + ind, y++, r.w - ind, p, n, hc);
				if (!e) break;
				p = e + 1;
			}
			continue;
		}
		for (size_t k = 0; k < U.hdr.n && y < r.y + r.h; k++) {
			Item *it = U.hdr.v[rev ? U.hdr.n - 1 - k : k];
			int total;
			size_t n = glyphs_of(it->text, it->tlen, U.o->ansi ? it->ansi : NULL, U.o->ansi ? it->nansi : 0, NULL, 0, hc, hc, U.o->tabstop, &total);
			draw_glyphs(r, r.x + ind, y++, r.w - ind, n, total, 0, false, hc);
		}
	}
	(void)bottom_up;
}

static void draw_footer(Rect r)
{
	Col fc = col_mk(U.th.c[C_FOOTER].fg, U.th.c[C_FOOTER].bg == COL_DEF ? U.th.c[C_FG].bg : U.th.c[C_FOOTER].bg, U.th.c[C_FOOTER].attr);
	gfill(r, fc);
	int ind = U.o->footer_border > 0 ? 0 : ptr_w + mrk_w, y = r.y;
	const char *p = U.footer;
	while (p && *p && y < r.y + r.h) {
		const char *e = strchr(p, '\n');
		size_t n = e ? (size_t)(e - p) : strlen(p);
		draw_ansi(r, r.x + ind, y++, r.w - ind, p, n, fc);
		if (!e) break;
		p = e + 1;
	}
}

static void draw_main(Rect m, int *cx, int *cy)
{
	Opts *o = U.o;
	bool prompt_top = o->layout == LAYOUT_REVERSE;
	bool bottom_up = o->layout == LAYOUT_DEFAULT;
	ptr_w = str_width(o->pointer, strlen(o->pointer));
	mrk_w = str_width(o->marker, strlen(o->marker));
	bool input_on = !o->no_input && !U.hide_input;
	bool inline_info = o->info == INFO_INLINE || o->info == INFO_INLINE_RIGHT;
	int input_h = input_on ? 1 : 0;
	int info_h = (o->info == INFO_HIDDEN && !input_on) ? 0 : (inline_info ? 0 : 1);
	if (o->info == INFO_HIDDEN) info_h = 1;
	if (!input_on) info_h = 0;                          /* the info line belongs to the input */
	int hdr_h = text_lines(U.header) + (int)U.hdr.n;
	int ftr_h = text_lines(U.footer);
	int fshape = o->footer_border > 0 ? o->footer_border : BORDER_NONE;
	int sep_f = (ftr_h && o->footer_border < 0) ? 1 : 0;            /* automatic: a rule under the footer */
	int in_h = input_h + info_h;
	int et, er, eb, el;
	#define EXTRA(shape) (border_edges(shape, &et, &er, &eb, &el) ? et + eb : 0)
	/* the sections in reading order, top to bottom: id, inner height, own border */
	enum { S_F, S_SEP, S_L, S_H, S_I };
	struct { int id, h, shape, ex; Rect r; bool in; } sec[5];
	int ns = 0;
	#define ADD(ID, H, SHAPE) do { sec[ns].id = ID; sec[ns].h = H; sec[ns].shape = SHAPE; sec[ns].ex = EXTRA(SHAPE); sec[ns].in = false; ns++; } while (0)
	if (!prompt_top) {
		if (ftr_h) ADD(S_F, ftr_h, fshape);
		if (sep_f) ADD(S_SEP, 1, BORDER_NONE);
		ADD(S_L, 0, BORDER_NONE);
		if (hdr_h && !o->header_first) ADD(S_H, hdr_h, o->header_border);
		if (in_h) ADD(S_I, in_h, o->input_border);
		if (hdr_h && o->header_first) ADD(S_H, hdr_h, o->header_border);
	} else {
		if (hdr_h && o->header_first) ADD(S_H, hdr_h, o->header_border);
		if (in_h) ADD(S_I, in_h, o->input_border);
		if (hdr_h && !o->header_first) ADD(S_H, hdr_h, o->header_border);
		ADD(S_L, 0, BORDER_NONE);
		if (sep_f) ADD(S_SEP, 1, BORDER_NONE);
		if (ftr_h) ADD(S_F, ftr_h, fshape);
	}
	/* a list border also encloses the neighbouring sections that have no border of their own */
	int li = 0;
	for (int i = 0; i < ns; i++) if (sec[i].id == S_L) li = i;
	int g0 = li, g1 = li;
	if (o->list_border != BORDER_NONE) {
		while (g0 > 0 && sec[g0 - 1].id != S_SEP && sec[g0 - 1].shape == BORDER_NONE) g0--;
		while (g1 + 1 < ns && sec[g1 + 1].id != S_SEP && sec[g1 + 1].shape == BORDER_NONE) g1++;
	}
	int lt, lr_, lb, ll;
	int lgex = border_edges(o->list_border, &lt, &lr_, &lb, &ll) ? lt + lb : 0;
	int used = lgex;
	for (int i = 0; i < ns; i++) used += sec[i].h + sec[i].ex;
	int list_h = m.h - used;
	if (list_h < 0) {                                  /* squeeze the header first */
		for (int i = 0; i < ns && list_h < 0; i++) if (sec[i].id == S_H) {
			int cut = -list_h < sec[i].h ? -list_h : sec[i].h;
			sec[i].h -= cut; list_h += cut;
			if (!sec[i].h) { list_h += sec[i].ex; sec[i].ex = 0; }
		}
		if (list_h < 0) list_h = 0;
	}
	sec[li].h = list_h;
	Rect gin = border_inner((Rect){m.x, m.y, m.w, m.h}, o->list_border);   /* horizontal inset of the enclosed sections */
	int y = m.y, gy0 = m.y;
	for (int i = 0; i < ns; i++) {
		bool inside = o->list_border != BORDER_NONE && i >= g0 && i <= g1;
		if (inside && i == g0) { gy0 = y; y += lt; }
		int x = inside ? gin.x : m.x, w = inside ? gin.w : m.w;
		sec[i].r = (Rect){x, y, w, sec[i].h + sec[i].ex};
		y += sec[i].h + sec[i].ex;
		if (inside && i == g1) {
			Col gbc = col_mk(U.th.c[C_BORDER].fg, U.th.c[C_FG].bg, 0);
			y += lb;
			draw_border((Rect){m.x, gy0, m.w, y - gy0}, o->list_border, gbc, NULL, 0);
		}
	}
	Col sbc = col_mk(U.th.c[C_BORDER].fg, U.th.c[C_FG].bg, 0);
	Rect fr = {0}, sepr = {0}, hr = {0}, lr = {0}, ir = {0}, pr = {0};
	for (int i = 0; i < ns; i++) {
		Rect o_ = sec[i].r, inner = border_inner(o_, sec[i].shape);
		if (sec[i].id != S_L) draw_border(o_, sec[i].shape, sbc, NULL, 0);
		switch (sec[i].id) {
		case S_F: fr = inner; break;
		case S_SEP: sepr = o_; break;
		case S_H: hr = inner; break;
		case S_L: lr = o_; break;
		case S_I:
			if (!prompt_top) { ir = (Rect){inner.x, inner.y, inner.w, info_h}; pr = (Rect){inner.x, inner.y + info_h, inner.w, input_h}; }
			else { pr = (Rect){inner.x, inner.y, inner.w, input_h}; ir = (Rect){inner.x, inner.y + input_h, inner.w, info_h}; }
			break;
		}
	}
	U.list_padbar = o->list_border != BORDER_NONE && (ll || lr_);
	U.info_rule = o->input_border == BORDER_NONE && (!hdr_h || o->header_border == BORDER_NONE);
	draw_list(lr, bottom_up);
	if (hr.h > 0) draw_hdr_lines(hr, bottom_up); else U.hdr_rect = (Rect){0, 0, 0, 0};
	if (ftr_h) {
		draw_footer(fr);
		Col sc = col_mk(U.th.c[C_BORDER].fg, U.th.c[C_FG].bg, 0);
		for (int k = 0; sep_f && k < sepr.w; k++) gs(sepr, sepr.x + k, sepr.y, "─", sc);
	}
	char info[96];
	build_info(info, sizeof info);
	*cx = *cy = -1;
	if (input_on) draw_query_line(pr, cx, cy, inline_info, info);
	else U.prompt_rect = (Rect){0, 0, 0, 0};
	if (info_h) draw_info_line(ir);
	else U.info_rect = (Rect){0, 0, 0, 0};
}

void draw_all(void)
{
	Opts *o = U.o;
	grid_clear();
	Rect whole = {0, 0, U.g.w, U.g.h};
	Rect a = whole;
	int mg[4];
	for (int i = 0; i < 4; i++) {
		int v = o->margin[i];
		int base = (i == 0 || i == 2) ? a.h : a.w;
		mg[i] = v < 0 ? base * (-v - 1) / 100 : v;
	}
	a.x += mg[3]; a.w -= mg[1] + mg[3]; a.y += mg[0]; a.h -= mg[0] + mg[2];
	if (a.w < 1 || a.h < 1) return;
	Col bc = col_mk(U.th.c[C_BORDER].fg, U.th.c[C_FG].bg, 0);
	draw_border(a, o->border, bc, U.border_label, o->border_label_pos);
	Rect in = border_inner(a, o->border);
	int pd[4];
	for (int i = 0; i < 4; i++) {
		int v = o->padding[i];
		int base = (i == 0 || i == 2) ? in.h : in.w;
		pd[i] = v < 0 ? base * (-v - 1) / 100 : v;
	}
	in.x += pd[3]; in.w -= pd[1] + pd[3]; in.y += pd[0]; in.h -= pd[0] + pd[2];
	if (in.w < 1 || in.h < 1) return;
	int hr = hints_rows();
	if (hr && in.h > hr + 5) { in.h -= hr; hints_draw((Rect){in.x, in.y + in.h, in.w, hr}); }
	else U.hint_rows = 0;
	U.area = in;
	Rect m = in;
	bool have_pv = U.pv_cmd != NULL && !U.pv_hidden;
	U.pv_rect = (Rect){0, 0, 0, 0};
	Rect pvr = {0};
	if (have_pv) {
		U.pw_active = *pw_pick(in.w);
		PW *p = &U.pw_active;
		if (p->pos == PW_RIGHT || p->pos == PW_LEFT) {
			int w = size_of(in.w, p->size, p->pct);
			if (w > in.w - 3) w = in.w - 3 > 0 ? in.w - 3 : in.w;
			pvr = (Rect){p->pos == PW_RIGHT ? in.x + in.w - w : in.x, in.y, w, in.h};
			m.w = in.w - w;
			if (p->pos == PW_LEFT) m.x = in.x + w;
		} else {
			int h = size_of(in.h, p->size, p->pct);
			if (h > in.h - 3) h = in.h - 3 > 0 ? in.h - 3 : in.h;
			pvr = (Rect){in.x, p->pos == PW_DOWN ? in.y + in.h - h : in.y, in.w, h};
			m.h = in.h - h;
			if (p->pos == PW_UP) m.y = in.y + h;
		}
	}
	int cx, cy;
	draw_main(m, &cx, &cy);
	if (have_pv) draw_preview(pvr);
	U.pw.hidden = U.pv_hidden;
	/* the cursor */
	term_outs("");
	static int lastx = -1, lasty = -1;
	lastx = cx; lasty = cy;
	U.mouse_last_x = cx; U.mouse_last_y = cy;
}

/* ------------------------------------------------------------------ output */
static Col last_c;
void present(void)
{
	Buf b;
	buf_init(&b);
	buf_adds(&b, "\033[?2026h\033[?25l");
	bool first = true;
	for (int y = 0; y < U.g.h; y++) {
		buf_printf(&b, "\033[%d;1H", U.top_row + y + 1);
		first = true;
		for (int x = 0; x < U.g.w; x++) {
			Cell *c = &U.g.cell[y * U.g.w + x];
			if (c->ch == 0) continue;
			if (first || c->c.fg != last_c.fg || c->c.bg != last_c.bg || c->c.attr != last_c.attr) { sgr_emit(&b, c->c); last_c = c->c; first = false; }
			char u[4];
			size_t n = u8enc(c->ch, u);
			buf_addn(&b, u, n);
		}
	}
	buf_adds(&b, "\033[0m");
	if (U.mouse_last_x >= 0 && !U.hide_input && !U.o->no_input)
		buf_printf(&b, "\033[%d;%dH\033[?25h", U.top_row + U.mouse_last_y + 1, U.mouse_last_x + 1);
	buf_adds(&b, "\033[?2026l");
	term_out(b.p, b.len);
	term_flush();
	buf_free(&b);
}

const char *pw_default_spec = "right,50%,border-rounded";
