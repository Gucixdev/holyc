/* nav.c - tabs, breadcrumbs, menus (bar + pull-down + context), tree bar. */
#include "internal.h"
#include <stdio.h>

/* ================================================================== tabs */
struct lt_tabs { char **label; int n, sel; lt_side side; int *pos, *len; lt_rect strip; };

lt_tabs *lt_tabs_new(const char *const *labels, int n, lt_side side)
{
	lt_tabs *t = calloc(1, sizeof *t);
	t->n = n; t->side = side;
	t->label = calloc((size_t)(n ? n : 1), sizeof *t->label);
	t->pos = calloc((size_t)(n ? n : 1), sizeof(int)); t->len = calloc((size_t)(n ? n : 1), sizeof(int));
	for (int i = 0; i < n; i++) t->label[i] = strdup(labels[i]);
	return t;
}
void lt_tabs_free(lt_tabs *t) { if (!t) return; for (int i = 0; i < t->n; i++) free(t->label[i]); free(t->label); free(t->pos); free(t->len); free(t); }
int lt_tabs_selected(const lt_tabs *t) { return t->sel; }
void lt_tabs_set(lt_tabs *t, int i) { if (i >= 0 && i < t->n) t->sel = i; }

static int tabs_strip_w(const lt_tabs *t) { int w = 0; for (int i = 0; i < t->n; i++) { int l = lt_utf8_width(t->label[i]) + 2; if (l > w) w = l; } return w + 1; }

lt_rect lt_tabs_pane(const lt_tabs *t, lt_rect w)
{
	switch (t->side) {
	case LT_SIDE_TOP: return (lt_rect){w.x, w.y + 2, w.w, w.h > 2 ? w.h - 2 : 0};
	case LT_SIDE_BOTTOM: return (lt_rect){w.x, w.y, w.w, w.h > 2 ? w.h - 2 : 0};
	case LT_SIDE_LEFT: { int sw = tabs_strip_w(t); return (lt_rect){w.x + sw, w.y, w.w > sw ? w.w - sw : 0, w.h}; }
	default: { int sw = tabs_strip_w(t); return (lt_rect){w.x, w.y, w.w > sw ? w.w - sw : 0, w.h}; }
	}
}

void lt_tabs_draw(lt_tabs *t, lt_canvas *c, lt_rect w, const lt_theme *th, bool focused)
{
	if (w.w < 3 || w.h < 2) return;
	lt_rect all = lt_full(c);
	bool uni = lt_canvas_caps(c)->unicode;
	lt_style rule = th->s[LT_T_BORDER];
	if (t->side == LT_SIDE_TOP || t->side == LT_SIDE_BOTTOM) {
		int y = t->side == LT_SIDE_TOP ? w.y : w.y + w.h - 1, ry = t->side == LT_SIDE_TOP ? w.y + 1 : w.y + w.h - 2;
		t->strip = (lt_rect){w.x, y, w.w, 1};
		lt_fill(c, t->strip, ' ', th->s[LT_T_TEXT]);
		for (int i = 0; i < w.w; i++) lt_put(c, all, w.x + i, ry, uni ? 0x2500 : '-', rule);
		int x = w.x;
		for (int i = 0; i < t->n; i++) {
			int l = lt_utf8_width(t->label[i]) + 2;
			t->pos[i] = x; t->len[i] = l;
			lt_style st = i == t->sel ? (focused ? th->s[LT_T_SELECT] : th->s[LT_T_ACCENT]) : th->s[LT_T_DIM];
			if (i == t->sel && !focused) st.attr |= LT_BOLD;
			if (x + l > w.x + w.w) { t->len[i] = 0; continue; }
			lt_put(c, all, x, y, ' ', st);
			lt_text(c, all, x + 1, y, t->label[i], st, l - 2);
			lt_put(c, all, x + l - 1, y, ' ', st);
			if (i == t->sel) for (int k = 0; k < l; k++) lt_put(c, all, x + k, ry, uni ? 0x2501 : '=', th->s[LT_T_ACCENT]);
			x += l + 1;
		}
	} else {
		int sw = tabs_strip_w(t), x0 = t->side == LT_SIDE_LEFT ? w.x : w.x + w.w - sw;
		t->strip = (lt_rect){x0, w.y, sw, w.h};
		lt_fill(c, t->strip, ' ', th->s[LT_T_TEXT]);
		int rx = t->side == LT_SIDE_LEFT ? x0 + sw - 1 : x0;
		for (int i = 0; i < w.h; i++) lt_put(c, all, rx, w.y + i, uni ? 0x2502 : '|', rule);
		for (int i = 0; i < t->n && i < w.h; i++) {
			t->pos[i] = w.y + i; t->len[i] = sw - 1;
			lt_style st = i == t->sel ? (focused ? th->s[LT_T_SELECT] : th->s[LT_T_ACCENT]) : th->s[LT_T_DIM];
			lt_fill(c, (lt_rect){x0, w.y + i, sw - 1, 1}, ' ', st);
			lt_text(c, all, x0 + 1, w.y + i, t->label[i], st, sw - 2);
			if (i == t->sel) lt_put(c, all, rx, w.y + i, uni ? 0x2503 : '#', th->s[LT_T_ACCENT]);
		}
	}
}

lt_wres lt_tabs_event(lt_tabs *t, lt_rect w, const lt_event *ev)
{
	if (t->n == 0) return LT_W_IGNORED;
	int base = ev->key & 0xFFFFFF, old = t->sel;
	bool horiz = t->side == LT_SIDE_TOP || t->side == LT_SIDE_BOTTOM;
	if (ev->kind == LT_EV_MOUSE) {
		(void)w;
		if (base == LT_K_WHEEL_UP || base == LT_K_WHEEL_DOWN) { if (!lt_in(t->strip, ev->x, ev->y)) return LT_W_IGNORED; t->sel = lt_clampi(t->sel + (base == LT_K_WHEEL_UP ? -1 : 1), 0, t->n - 1); return t->sel != old ? LT_W_CHANGED : LT_W_HANDLED; }
		if (base != LT_K_CLICK || !lt_in(t->strip, ev->x, ev->y)) return LT_W_IGNORED;
		for (int i = 0; i < t->n; i++) {
			if (!t->len[i]) continue;
			bool hit = horiz ? (ev->x >= t->pos[i] && ev->x < t->pos[i] + t->len[i]) : ev->y == t->pos[i];
			if (hit) { t->sel = i; return i != old ? LT_W_CHANGED : LT_W_HANDLED; }
		}
		return LT_W_HANDLED;
	}
	if (ev->kind != LT_EV_KEY || (ev->key & (LT_MOD_CTRL | LT_MOD_ALT))) return LT_W_IGNORED;
	int prev = horiz ? LT_K_LEFT : LT_K_UP, next = horiz ? LT_K_RIGHT : LT_K_DOWN;
	if (base == prev) t->sel = lt_clampi(t->sel - 1, 0, t->n - 1);
	else if (base == next) t->sel = lt_clampi(t->sel + 1, 0, t->n - 1);
	else if (base == LT_K_HOME) t->sel = 0;
	else if (base == LT_K_END) t->sel = t->n - 1;
	else return LT_W_IGNORED;
	return t->sel != old ? LT_W_CHANGED : LT_W_HANDLED;
}

/* ================================================================== breadcrumbs */
struct lt_breadcrumbs { char **seg; int n, cursor; int *bx, *bw; };
lt_breadcrumbs *lt_breadcrumbs_new(void) { return calloc(1, sizeof(lt_breadcrumbs)); }
static void bc_clear(lt_breadcrumbs *b) { for (int i = 0; i < b->n; i++) free(b->seg[i]); free(b->seg); free(b->bx); free(b->bw); b->seg = NULL; b->bx = b->bw = NULL; b->n = 0; }
void lt_breadcrumbs_free(lt_breadcrumbs *b) { if (b) { bc_clear(b); free(b); } }

void lt_breadcrumbs_set_path(lt_breadcrumbs *b, const char *path, char sep)
{
	bc_clear(b);
	if (!path) path = "";
	int cap = 8;
	b->seg = malloc((size_t)cap * sizeof *b->seg);
	if (*path == sep) { b->seg[b->n++] = strdup("/"); }
	for (const char *p = path; *p;) {
		while (*p == sep) p++;
		const char *e = p;
		while (*e && *e != sep) e++;
		if (e > p) {
			if (b->n == cap) { cap *= 2; b->seg = realloc(b->seg, (size_t)cap * sizeof *b->seg); }
			b->seg[b->n] = malloc((size_t)(e - p) + 1);
			memcpy(b->seg[b->n], p, (size_t)(e - p)); b->seg[b->n][e - p] = 0; b->n++;
		}
		p = e;
	}
	b->bx = calloc((size_t)(b->n ? b->n : 1), sizeof(int)); b->bw = calloc((size_t)(b->n ? b->n : 1), sizeof(int));
	b->cursor = b->n ? b->n - 1 : 0;
}
int lt_breadcrumbs_count(const lt_breadcrumbs *b) { return b->n; }
int lt_breadcrumbs_cursor(const lt_breadcrumbs *b) { return b->cursor; }

void lt_breadcrumbs_path_upto(const lt_breadcrumbs *b, int index, char sep, char *out, size_t n)
{
	size_t k = 0;
	if (n == 0) return;
	out[0] = 0;
	for (int i = 0; i <= index && i < b->n; i++) {
		bool root = i == 0 && !strcmp(b->seg[0], "/");
		if (root) { if (k + 1 < n) { out[k++] = sep; out[k] = 0; } continue; }
		if (k > 0 && out[k - 1] != sep && k + 1 < n) out[k++] = sep;
		size_t l = strlen(b->seg[i]);
		if (k + l < n) { memcpy(out + k, b->seg[i], l); k += l; out[k] = 0; }
	}
}

void lt_breadcrumbs_draw(lt_breadcrumbs *b, lt_canvas *c, lt_rect r, const lt_theme *t, bool focused)
{
	if (r.w < 3 || r.h < 1) return;
	r.h = 1;
	lt_rect all = lt_full(c);
	bool uni = lt_canvas_caps(c)->unicode;
	lt_fill(c, r, ' ', t->s[LT_T_TEXT]);
	const char *sepg = uni ? "\xE2\x80\xBA" : ">";
	/* how many segments from the end fit; the first one is always kept, the gap becomes "..." */
	int total = 0;
	for (int i = 0; i < b->n; i++) total += lt_utf8_width(b->seg[i]) + 2 + (i ? 1 : 0);
	int first_shown = 0;
	bool collapsed = false;
	if (total > r.w && b->n > 2) {
		int used = lt_utf8_width(b->seg[0]) + 2 + 4;                           /* first segment + " ... " */
		int i = b->n - 1;
		while (i > 1) { int w = lt_utf8_width(b->seg[i]) + 3; if (used + w > r.w) break; used += w; i--; }
		first_shown = i + 1; collapsed = true;
		if (first_shown > b->n - 1) first_shown = b->n - 1;
	}
	int x = r.x;
	for (int i = 0; i < b->n; i++) {
		b->bw[i] = 0;
		if (collapsed && i > 0 && i < first_shown) {
			if (i == 1) { x += lt_text(c, all, x, r.y, " ", t->s[LT_T_DIM], 1); x += lt_text(c, all, x, r.y, uni ? "\xE2\x80\xA6" : "...", t->s[LT_T_DIM], 3); x += lt_text(c, all, x, r.y, " ", t->s[LT_T_DIM], 1); x += lt_text(c, all, x, r.y, sepg, t->s[LT_T_DIM], 1); }
			continue;
		}
		if (i > 0 && !(collapsed && i == first_shown)) x += lt_text(c, all, x, r.y, sepg, t->s[LT_T_DIM], 1);
		int w = lt_utf8_width(b->seg[i]) + 2;
		if (x + w > r.x + r.w) break;
		lt_style st = i == b->n - 1 ? t->s[LT_T_ACCENT] : t->s[LT_T_TEXT];
		if (i == b->cursor && focused) st = t->s[LT_T_SELECT];
		b->bx[i] = x; b->bw[i] = w;
		lt_fill(c, (lt_rect){x, r.y, w, 1}, ' ', st);
		lt_text(c, all, x + 1, r.y, b->seg[i], st, w - 2);
		x += w;
	}
}

lt_wres lt_breadcrumbs_event(lt_breadcrumbs *b, lt_rect r, const lt_event *ev)
{
	if (b->n == 0) return LT_W_IGNORED;
	int base = ev->key & 0xFFFFFF, old = b->cursor;
	r.h = 1;
	if (ev->kind == LT_EV_MOUSE) {
		if (base != LT_K_CLICK || !lt_in(r, ev->x, ev->y)) return LT_W_IGNORED;
		for (int i = 0; i < b->n; i++) if (b->bw[i] && ev->x >= b->bx[i] && ev->x < b->bx[i] + b->bw[i]) { b->cursor = i; return LT_W_ACTIVATE; }
		return LT_W_HANDLED;
	}
	if (ev->kind != LT_EV_KEY || (ev->key & (LT_MOD_CTRL | LT_MOD_ALT))) return LT_W_IGNORED;
	if (base == LT_K_LEFT) b->cursor = lt_clampi(b->cursor - 1, 0, b->n - 1);
	else if (base == LT_K_RIGHT) b->cursor = lt_clampi(b->cursor + 1, 0, b->n - 1);
	else if (base == LT_K_HOME) b->cursor = 0;
	else if (base == LT_K_END) b->cursor = b->n - 1;
	else if (base == LT_K_ENTER) return LT_W_ACTIVATE;
	else return LT_W_IGNORED;
	return b->cursor != old ? LT_W_CHANGED : LT_W_HANDLED;
}

/* ================================================================== menu */
typedef struct { char *label, *shortcut; int id; bool sep, check, checked, enabled; } mitem;
typedef struct { char *title; mitem *it; int n, cap; } mmenu;
struct lt_menu {
	mmenu *m; int n;
	int open;                        /* -1 closed, else the open menu */
	int cursor;                      /* item under the cursor in the open menu */
	int last_id;
	int *tx, *tw;                    /* title positions on the bar */
	lt_rect bar, popup;
	int px, py;                      /* popup origin */
	bool context;
};

lt_menu *lt_menu_new(void) { lt_menu *m = calloc(1, sizeof *m); m->open = -1; m->last_id = -1; return m; }
void lt_menu_free(lt_menu *m)
{
	if (!m) return;
	for (int i = 0; i < m->n; i++) { for (int k = 0; k < m->m[i].n; k++) { free(m->m[i].it[k].label); free(m->m[i].it[k].shortcut); } free(m->m[i].it); free(m->m[i].title); }
	free(m->m); free(m->tx); free(m->tw); free(m);
}
int lt_menu_add(lt_menu *m, const char *title)
{
	m->m = realloc(m->m, (size_t)(m->n + 1) * sizeof *m->m);
	m->m[m->n] = (mmenu){strdup(title ? title : ""), NULL, 0, 0};
	m->tx = realloc(m->tx, (size_t)(m->n + 1) * sizeof(int)); m->tw = realloc(m->tw, (size_t)(m->n + 1) * sizeof(int));
	m->tx[m->n] = m->tw[m->n] = 0;
	return m->n++;
}
static void add_item(lt_menu *m, int menu, mitem it)
{
	if (menu < 0 || menu >= m->n) return;
	mmenu *mm = &m->m[menu];
	if (mm->n == mm->cap) { mm->cap = mm->cap ? mm->cap * 2 : 8; mm->it = realloc(mm->it, (size_t)mm->cap * sizeof *mm->it); }
	mm->it[mm->n++] = it;
}
void lt_menu_item(lt_menu *m, int menu, const char *label, const char *sc, int id) { add_item(m, menu, (mitem){strdup(label ? label : ""), strdup(sc ? sc : ""), id, false, false, false, true}); }
void lt_menu_check(lt_menu *m, int menu, const char *label, const char *sc, int id, bool on) { add_item(m, menu, (mitem){strdup(label ? label : ""), strdup(sc ? sc : ""), id, false, true, on, true}); }
void lt_menu_sep(lt_menu *m, int menu) { add_item(m, menu, (mitem){strdup(""), strdup(""), -1, true, false, false, false}); }
static mitem *find_item(const lt_menu *m, int id) { for (int i = 0; i < m->n; i++) for (int k = 0; k < m->m[i].n; k++) if (!m->m[i].it[k].sep && m->m[i].it[k].id == id) return &m->m[i].it[k]; return NULL; }
void lt_menu_set_checked(lt_menu *m, int id, bool on) { mitem *it = find_item(m, id); if (it) it->checked = on; }
void lt_menu_set_enabled(lt_menu *m, int id, bool on) { mitem *it = find_item(m, id); if (it) it->enabled = on; }
bool lt_menu_checked(const lt_menu *m, int id) { mitem *it = find_item(m, id); return it && it->checked; }
int lt_menu_id(const lt_menu *m) { return m->last_id; }
bool lt_menu_is_open(const lt_menu *m) { return m->open >= 0; }
void lt_menu_close(lt_menu *m) { m->open = -1; m->context = false; }

static int first_enabled(const mmenu *mm, int from, int dir)
{
	for (int i = from; i >= 0 && i < mm->n; i += dir) if (!mm->it[i].sep && mm->it[i].enabled) return i;
	return -1;
}

void lt_menu_open_at(lt_menu *m, int menu, int x, int y)
{
	if (menu < 0 || menu >= m->n) return;
	m->open = menu; m->px = x; m->py = y; m->context = true;
	int f = first_enabled(&m->m[menu], 0, 1);
	m->cursor = f < 0 ? 0 : f;
}

void lt_menu_draw_bar(lt_menu *m, lt_canvas *c, lt_rect r, const lt_theme *t)
{
	if (r.w < 1) return;
	r.h = 1;
	m->bar = r;
	lt_rect all = lt_full(c);
	lt_fill(c, r, ' ', t->s[LT_T_TEXT]);
	int x = r.x + 1;
	for (int i = 0; i < m->n; i++) {
		int w = lt_utf8_width(m->m[i].title) + 2;
		m->tx[i] = x; m->tw[i] = w;
		lt_style st = (i == m->open && !m->context) ? t->s[LT_T_SELECT] : t->s[LT_T_TEXT];
		if (x + w > r.x + r.w) { m->tw[i] = 0; continue; }
		lt_fill(c, (lt_rect){x, r.y, w, 1}, ' ', st);
		lt_text(c, all, x + 1, r.y, m->m[i].title, st, w - 2);
		x += w + 1;
	}
}

static void popup_geom(const lt_menu *m, int canvas_w, int canvas_h, lt_rect *out)
{
	const mmenu *mm = &m->m[m->open];
	int w = 8;
	for (int i = 0; i < mm->n; i++) { int l = lt_utf8_width(mm->it[i].label) + 6 + (mm->it[i].shortcut[0] ? lt_utf8_width(mm->it[i].shortcut) + 3 : 0); if (l > w) w = l; }
	int h = mm->n + 2;
	int x = m->context ? m->px : m->tx[m->open], y = m->context ? m->py : m->bar.y + 1;
	if (x + w > canvas_w) x = canvas_w - w;
	if (y + h > canvas_h) y = canvas_h - h;
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	*out = (lt_rect){x, y, w, h};
}

void lt_menu_draw_popup(lt_menu *m, lt_canvas *c, const lt_theme *t)
{
	if (m->open < 0 || m->open >= m->n) return;
	lt_rect p;
	popup_geom(m, lt_canvas_w(c), lt_canvas_h(c), &p);
	m->popup = p;
	lt_rect all = lt_full(c);
	bool uni = lt_canvas_caps(c)->unicode;
	lt_fill(c, p, ' ', t->s[LT_T_TEXT]);
	lt_panel(c, p, t, NULL);
	const mmenu *mm = &m->m[m->open];
	for (int i = 0; i < mm->n; i++) {
		const mitem *it = &mm->it[i];
		int y = p.y + 1 + i;
		if (it->sep) { for (int k = 1; k < p.w - 1; k++) lt_put(c, all, p.x + k, y, uni ? 0x2500 : '-', t->s[LT_T_BORDER]); continue; }
		lt_style st = i == m->cursor ? t->s[LT_T_SELECT] : t->s[it->enabled ? LT_T_TEXT : LT_T_DIM];
		lt_fill(c, (lt_rect){p.x + 1, y, p.w - 2, 1}, ' ', st);
		if (it->check) lt_put(c, all, p.x + 2, y, it->checked ? (uni ? 0x2713 : 'x') : ' ', st);
		lt_text(c, all, p.x + 4, y, it->label, st, p.w - 6);
		if (it->shortcut[0]) { int sw = lt_utf8_width(it->shortcut); lt_text(c, all, p.x + p.w - 2 - sw, y, it->shortcut, i == m->cursor ? st : t->s[LT_T_DIM], sw); }
	}
}

static lt_wres activate(lt_menu *m)
{
	mmenu *mm = &m->m[m->open];
	if (m->cursor < 0 || m->cursor >= mm->n) return LT_W_HANDLED;
	mitem *it = &mm->it[m->cursor];
	if (it->sep || !it->enabled) return LT_W_HANDLED;
	if (it->check) it->checked = !it->checked;
	m->last_id = it->id;
	lt_menu_close(m);
	return LT_W_ACTIVATE;
}

lt_wres lt_menu_event(lt_menu *m, lt_rect bar, const lt_event *ev, int cw, int ch)
{
	if (m->n == 0) return LT_W_IGNORED;
	int base = ev->key & 0xFFFFFF;
	bar.h = 1;
	if (ev->kind == LT_EV_MOUSE) {
		if (m->open >= 0) {
			lt_rect p;
			popup_geom(m, cw, ch, &p);
			if (base == LT_K_CLICK && lt_in(p, ev->x, ev->y)) {
				int i = ev->y - p.y - 1;
				if (i >= 0 && i < m->m[m->open].n) { m->cursor = i; return activate(m); }
				return LT_W_HANDLED;
			}
			if (base == LT_K_CLICK) {                                                       /* outside: another title switches, anything else closes */
				for (int i = 0; !m->context && i < m->n; i++) if (m->tw[i] && lt_in((lt_rect){m->tx[i], bar.y, m->tw[i], 1}, ev->x, ev->y)) { if (i == m->open) { lt_menu_close(m); return LT_W_CLOSED; } m->open = i; m->cursor = first_enabled(&m->m[i], 0, 1); if (m->cursor < 0) m->cursor = 0; return LT_W_HANDLED; }
				lt_menu_close(m);
				return LT_W_CLOSED;
			}
			return LT_W_HANDLED;
		}
		if (base != LT_K_CLICK) return LT_W_IGNORED;
		for (int i = 0; i < m->n; i++) if (m->tw[i] && lt_in((lt_rect){m->tx[i], bar.y, m->tw[i], 1}, ev->x, ev->y)) { m->open = i; m->context = false; m->cursor = first_enabled(&m->m[i], 0, 1); if (m->cursor < 0) m->cursor = 0; return LT_W_HANDLED; }
		return LT_W_IGNORED;
	}
	if (ev->kind != LT_EV_KEY || (ev->key & (LT_MOD_CTRL | LT_MOD_ALT))) return LT_W_IGNORED;
	if (m->open < 0) {
		if (base == LT_K_DOWN || base == LT_K_ENTER) { m->open = 0; m->context = false; m->cursor = first_enabled(&m->m[0], 0, 1); if (m->cursor < 0) m->cursor = 0; return LT_W_HANDLED; }
		return LT_W_IGNORED;
	}
	mmenu *mm = &m->m[m->open];
	int f;
	switch (base) {
	case LT_K_ESC: lt_menu_close(m); return LT_W_CLOSED;
	case LT_K_DOWN: f = first_enabled(mm, m->cursor + 1, 1); if (f >= 0) m->cursor = f; else { f = first_enabled(mm, 0, 1); if (f >= 0) m->cursor = f; } return LT_W_HANDLED;
	case LT_K_UP: f = first_enabled(mm, m->cursor - 1, -1); if (f >= 0) m->cursor = f; else { f = first_enabled(mm, mm->n - 1, -1); if (f >= 0) m->cursor = f; } return LT_W_HANDLED;
	case LT_K_LEFT: if (!m->context) { m->open = (m->open + m->n - 1) % m->n; f = first_enabled(&m->m[m->open], 0, 1); m->cursor = f < 0 ? 0 : f; } return LT_W_HANDLED;
	case LT_K_RIGHT: if (!m->context) { m->open = (m->open + 1) % m->n; f = first_enabled(&m->m[m->open], 0, 1); m->cursor = f < 0 ? 0 : f; } return LT_W_HANDLED;
	case LT_K_ENTER: case ' ': return activate(m);
	}
	return LT_W_HANDLED;
}

/* ================================================================== tree bar */
struct lt_treebar { lt_tree *tree; int level, cur; int *bx, *bw, ncap; int first; int px_n; int *px, *pw, *pnode; };
lt_treebar *lt_treebar_new(lt_tree *tree) { lt_treebar *b = calloc(1, sizeof *b); b->tree = tree; b->level = -1; b->cur = 0; return b; }
void lt_treebar_free(lt_treebar *b) { if (b) { free(b->bx); free(b->bw); free(b->px); free(b->pw); free(b->pnode); free(b); } }
int lt_treebar_level(const lt_treebar *b) { return b->level; }
int lt_treebar_node(const lt_treebar *b) { int n = lt_tree_child_count(b->tree, b->level); return n ? lt_tree_child(b->tree, b->level, lt_clampi(b->cur, 0, n - 1)) : -1; }

void lt_treebar_draw(lt_treebar *b, lt_canvas *c, lt_rect r, const lt_theme *t, bool focused)
{
	if (r.w < 4 || r.h < 1) return;
	r.h = 1;
	lt_rect all = lt_full(c);
	bool uni = lt_canvas_caps(c)->unicode;
	lt_fill(c, r, ' ', t->s[LT_T_TEXT]);
	int n = lt_tree_child_count(b->tree, b->level);
	if (n == 0) b->cur = 0; else b->cur = lt_clampi(b->cur, 0, n - 1);
	/* the path: ancestors of the level, outermost first */
	int chain[64], nc = 0;
	for (int q = b->level; q >= 0 && nc < 64; q = lt_tree_parent(b->tree, q)) chain[nc++] = q;
	b->px = realloc(b->px, (size_t)(nc + 2) * sizeof(int)); b->pw = realloc(b->pw, (size_t)(nc + 2) * sizeof(int)); b->pnode = realloc(b->pnode, (size_t)(nc + 2) * sizeof(int));
	b->px_n = 0;
	int x = r.x;
	x += lt_text(c, all, x, r.y, uni ? "\xE2\x8C\x82" : "~", t->s[LT_T_DIM], 1);
	b->px[b->px_n] = r.x; b->pw[b->px_n] = 1; b->pnode[b->px_n++] = -1;
	for (int i = nc - 1; i >= 0; i--) {
		int w = lt_utf8_width(lt_tree_label(b->tree, chain[i])) + 2;
		if (x + 2 + w > r.x + r.w / 2) break;
		x += lt_text(c, all, x, r.y, uni ? " \xE2\x80\xBA" : " >", t->s[LT_T_DIM], 2);
		lt_fill(c, (lt_rect){x, r.y, w, 1}, ' ', t->s[LT_T_ACCENT]);
		lt_text(c, all, x + 1, r.y, lt_tree_label(b->tree, chain[i]), t->s[LT_T_ACCENT], w - 2);
		b->px[b->px_n] = x; b->pw[b->px_n] = w; b->pnode[b->px_n++] = chain[i];
		x += w;
	}
	x += lt_text(c, all, x, r.y, uni ? " \xE2\x80\xBA " : " > ", t->s[LT_T_DIM], 3);
	b->bx = realloc(b->bx, (size_t)(n + 1) * sizeof(int)); b->bw = realloc(b->bw, (size_t)(n + 1) * sizeof(int));
	for (int i = 0; i < n; i++) b->bw[i] = 0;
	if (b->first > b->cur) b->first = b->cur;
	for (;;) {                                                                              /* scroll so that the cursor fits */
		int used = x, last = b->first;
		for (int i = b->first; i < n; i++) { int w = lt_utf8_width(lt_tree_label(b->tree, lt_tree_child(b->tree, b->level, i))) + 3; if (used + w > r.x + r.w && i > b->first) break; used += w + 1; last = i; }
		if (b->cur <= last || b->first >= b->cur) break;
		b->first++;
	}
	for (int i = b->first; i < n; i++) {
		int node = lt_tree_child(b->tree, b->level, i);
		const char *lab = lt_tree_label(b->tree, node);
		bool kids = lt_tree_child_count(b->tree, node) > 0;
		int w = lt_utf8_width(lab) + 2 + (kids ? 2 : 0);
		if (x + w > r.x + r.w) break;
		lt_style st = t->s[LT_T_TEXT];
		if (i == b->cur) st = focused ? t->s[LT_T_SELECT] : t->s[LT_T_ACCENT];
		lt_fill(c, (lt_rect){x, r.y, w, 1}, ' ', st);
		lt_text(c, all, x + 1, r.y, lab, st, w - 2);
		if (kids) lt_text(c, all, x + w - 2, r.y, uni ? "\xE2\x96\xB8" : ">", st, 1);
		b->bx[i] = x; b->bw[i] = w;
		x += w + 1;
	}
}

lt_wres lt_treebar_event(lt_treebar *b, lt_rect r, const lt_event *ev)
{
	int n = lt_tree_child_count(b->tree, b->level), base = ev->key & 0xFFFFFF, old = b->cur, oldl = b->level;
	r.h = 1;
	if (ev->kind == LT_EV_MOUSE) {
		if (base != LT_K_CLICK || !lt_in(r, ev->x, ev->y)) return LT_W_IGNORED;
		for (int i = 0; i < b->px_n; i++) if (ev->x >= b->px[i] && ev->x < b->px[i] + b->pw[i]) { b->level = b->pnode[i]; b->cur = 0; b->first = 0; return LT_W_CHANGED; }   /* a path segment: back to that level */
		for (int i = 0; i < n; i++) if (b->bw[i] && ev->x >= b->bx[i] && ev->x < b->bx[i] + b->bw[i]) {
			b->cur = i;
			int node = lt_tree_child(b->tree, b->level, i);
			if (lt_tree_child_count(b->tree, node) > 0) { b->level = node; b->cur = 0; b->first = 0; return LT_W_CHANGED; }
			return LT_W_ACTIVATE;
		}
		return LT_W_HANDLED;
	}
	if (ev->kind != LT_EV_KEY || (ev->key & (LT_MOD_CTRL | LT_MOD_ALT))) return LT_W_IGNORED;
	switch (base) {
	case LT_K_LEFT: b->cur = lt_clampi(b->cur - 1, 0, n ? n - 1 : 0); break;
	case LT_K_RIGHT: b->cur = lt_clampi(b->cur + 1, 0, n ? n - 1 : 0); break;
	case LT_K_HOME: b->cur = 0; break;
	case LT_K_END: b->cur = n ? n - 1 : 0; break;
	case LT_K_DOWN: case LT_K_ENTER: {
		int node = lt_treebar_node(b);
		if (node < 0) return LT_W_HANDLED;
		if (lt_tree_child_count(b->tree, node) > 0) { b->level = node; b->cur = 0; b->first = 0; return LT_W_CHANGED; }
		return base == LT_K_ENTER ? LT_W_ACTIVATE : LT_W_HANDLED;
	}
	case LT_K_UP: case LT_K_BSPACE:
		if (b->level < 0) return LT_W_HANDLED;
		{ int from = b->level; b->level = lt_tree_parent(b->tree, from); n = lt_tree_child_count(b->tree, b->level); b->cur = 0; for (int i = 0; i < n; i++) if (lt_tree_child(b->tree, b->level, i) == from) b->cur = i; b->first = 0; }
		return LT_W_CHANGED;
	default: return LT_W_IGNORED;
	}
	return (b->cur != old || b->level != oldl) ? LT_W_CHANGED : LT_W_HANDLED;
}
