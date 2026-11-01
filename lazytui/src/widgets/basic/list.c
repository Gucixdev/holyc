/* list.c - scrolling machinery shared by list and tree (scrollbar, wheel, drag, double click) and the list widget. */
#include "internal.h"
#include <ctype.h>
#include <math.h>

/* ------------------------------------------------------------------ scrollbar */
static int thumb_len(int total, int page, int h)
{
	int t = total > 0 ? (int)((double)page * h / total + 0.5) : h;
	return lt_clampi(t, 1, h);
}

void lt_scrollbar(lt_canvas *c, lt_rect r, const lt_theme *t, int total, int offset, int page)
{
	if (r.w < 1 || r.h < 1 || total <= page || page < 1) return;
	lt_rect all = lt_full(c);
	bool uni = lt_canvas_caps(c)->unicode;
	int th = thumb_len(total, page, r.h), room = r.h - th, maxoff = total - page;
	int top = maxoff > 0 ? (int)((double)room * lt_clampi(offset, 0, maxoff) / maxoff + 0.5) : 0;
	lt_style track = t->s[LT_T_DIM], thumb = t->s[LT_T_ACCENT];
	for (int i = 0; i < r.h; i++) {
		bool in = i >= top && i < top + th;
		lt_put(c, all, r.x, r.y + i, in ? (uni ? 0x2503 : '#') : (uni ? 0x2502 : '|'), in ? thumb : track);
	}
}

int lt_scrollbar_offset_at(lt_rect r, int total, int page, int y)
{
	if (total <= page || r.h < 1) return 0;
	int th = thumb_len(total, page, r.h), room = r.h - th, maxoff = total - page;
	if (room <= 0) return 0;
	return lt_clampi((int)((double)(y - r.y - th / 2) * maxoff / room + 0.5), 0, maxoff);
}

/* ------------------------------------------------------------------ pointer helpers */
bool lt_ptr_double(lt_ptr *p, int row)
{
	double now = lt_now();
	bool dbl = p->last_row == row && now - p->last_t < 0.45;
	p->last_t = dbl ? 0 : now;
	p->last_row = row;
	return dbl;
}

lt_rect lt_view_content(lt_rect r, int total)
{
	if (total > r.h && r.w > 2) r.w -= 1;
	return r;
}

/* wheel, scrollbar click and drag. *on_bar_click is set when the press landed on the bar (so the caller does not also select) */
bool lt_view_mouse(lt_ptr *p, lt_rect r, int total, int *offset, const lt_event *ev, bool *on_bar_click)
{
	int base = ev->key & 0xFFFFFF;
	int maxoff = total > r.h ? total - r.h : 0;
	lt_rect bar = {r.x + r.w - 1, r.y, 1, r.h};
	if (on_bar_click) *on_bar_click = false;
	if (base == LT_K_WHEEL_UP || base == LT_K_WHEEL_DOWN) {
		if (!lt_in(r, ev->x, ev->y) || total <= r.h) return false;               /* nothing to scroll here: let the parent have it */
		*offset = lt_clampi(*offset + (base == LT_K_WHEEL_UP ? -3 : 3), 0, maxoff);
		return true;
	}
	if (total <= r.h) { p->dragging = false; return false; }
	if (base == LT_K_CLICK && lt_in(bar, ev->x, ev->y)) {
		p->dragging = true;
		*offset = lt_scrollbar_offset_at(bar, total, r.h, ev->y);
		if (on_bar_click) *on_bar_click = true;
		return true;
	}
	if (base == LT_K_DRAG && p->dragging) { *offset = lt_scrollbar_offset_at(bar, total, r.h, ev->y); return true; }
	if (base == LT_K_RELEASE && p->dragging) { p->dragging = false; return true; }
	return false;
}

/* ------------------------------------------------------------------ list */
typedef struct { char *label; int id; bool marked; } item;
struct lt_list {
	item *it; int n, cap;
	int *shown; int nshown;              /* indexes of the items passing the filter, in order */
	int cursor;                          /* position within shown */
	int offset;                          /* first visible position */
	bool multi;
	char *filter;
	lt_ptr ptr;
	bool follow;                         /* the view follows the cursor (false after wheel / drag scrolling) */
	int rows;                            /* rows of the last draw, for page keys */
};

lt_list *lt_list_new(void) { lt_list *l = calloc(1, sizeof(lt_list)); l->follow = true; return l; }
void lt_list_free(lt_list *l)
{
	if (!l) return;
	for (int i = 0; i < l->n; i++) free(l->it[i].label);
	free(l->it); free(l->shown); free(l->filter); free(l);
}

static bool matches(const char *label, const char *f)
{
	if (!f || !*f) return true;
	size_t fl = strlen(f);
	for (const char *p = label; *p; p++) {
		size_t i = 0;
		while (i < fl && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)f[i])) i++;
		if (i == fl) return true;
	}
	return false;
}

static void refilter(lt_list *l)
{
	int keep = l->nshown && l->cursor < l->nshown ? l->shown[l->cursor] : -1;
	free(l->shown);
	l->shown = malloc((size_t)(l->n + 1) * sizeof(int));
	l->nshown = 0;
	for (int i = 0; i < l->n; i++) if (matches(l->it[i].label, l->filter)) l->shown[l->nshown++] = i;
	l->cursor = 0;
	for (int i = 0; i < l->nshown; i++) if (l->shown[i] == keep) l->cursor = i;
	l->offset = lt_clampi(l->offset, 0, l->nshown > 1 ? l->nshown - 1 : 0);
}

int lt_list_add(lt_list *l, const char *label, int id)
{
	if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 16; l->it = realloc(l->it, (size_t)l->cap * sizeof *l->it); }
	l->it[l->n] = (item){strdup(label ? label : ""), id, false};
	l->n++;
	refilter(l);
	return l->n - 1;
}
void lt_list_clear(lt_list *l)
{
	for (int i = 0; i < l->n; i++) free(l->it[i].label);
	l->n = 0; l->cursor = l->offset = 0;
	refilter(l);
}
void lt_list_set_multi(lt_list *l, bool m) { l->multi = m; }
int lt_list_count(const lt_list *l) { return l->n; }
int lt_list_shown(const lt_list *l) { return l->nshown; }
int lt_list_cursor(const lt_list *l) { return l->nshown ? l->shown[lt_clampi(l->cursor, 0, l->nshown - 1)] : -1; }
void lt_list_set_cursor(lt_list *l, int item_i)
{
	for (int i = 0; i < l->nshown; i++) if (l->shown[i] == item_i) { l->cursor = i; l->offset = lt_scroll_to(l->offset, i, l->rows ? l->rows : 1, l->nshown); return; }
}
int lt_list_id(const lt_list *l, int i) { return i >= 0 && i < l->n ? l->it[i].id : -1; }
const char *lt_list_label(const lt_list *l, int i) { return i >= 0 && i < l->n ? l->it[i].label : ""; }
bool lt_list_marked(const lt_list *l, int i) { return i >= 0 && i < l->n && l->it[i].marked; }
void lt_list_filter(lt_list *l, const char *text)
{
	free(l->filter);
	l->filter = text && *text ? strdup(text) : NULL;
	refilter(l);
	l->offset = 0;
}

static void draw_cell_text(lt_canvas *c, lt_rect clip, int x, int y, const char *s, lt_style st, int maxw)
{
	int w = lt_utf8_width(s);
	if (w <= maxw) { lt_text(c, clip, x, y, s, st, maxw); return; }
	int used = lt_text(c, clip, x, y, s, st, maxw > 1 ? maxw - 1 : 0);
	lt_put(c, clip, x + used, y, lt_canvas_caps(c)->unicode ? 0x2026 : '.', st);
}

void lt_list_draw(lt_list *l, lt_canvas *c, lt_rect r, const lt_theme *t, bool focused)
{
	if (r.w < 1 || r.h < 1) return;
	lt_rect all = lt_full(c);
	l->rows = r.h;
	l->offset = l->follow ? lt_scroll_to(l->offset, l->cursor, r.h, l->nshown) : lt_clampi(l->offset, 0, l->nshown > r.h ? l->nshown - r.h : 0);
	lt_rect body = lt_view_content(r, l->nshown);
	bool uni = lt_canvas_caps(c)->unicode;
	for (int row = 0; row < r.h; row++) {
		int pos = l->offset + row;
		if (pos >= l->nshown) break;
		item *it = &l->it[l->shown[pos]];
		bool cur = pos == l->cursor;
		lt_style st = cur && focused ? t->s[LT_T_SELECT] : t->s[LT_T_TEXT];
		if (cur && !focused) st.attr |= LT_BOLD;
		lt_fill(c, (lt_rect){body.x, body.y + row, body.w, 1}, ' ', st);
		lt_style ptr = cur ? (focused ? st : t->s[LT_T_ACCENT]) : t->s[LT_T_DIM];
		if (cur) lt_put(c, all, body.x, body.y + row, uni ? 0x258C : '>', ptr);
		if (l->multi) lt_put(c, all, body.x + 1, body.y + row, it->marked ? (uni ? 0x25CF : '*') : ' ', cur && focused ? st : t->s[LT_T_ACCENT]);
		int lx = body.x + (l->multi ? 3 : 2);
		draw_cell_text(c, all, lx, body.y + row, it->label, st, body.x + body.w - lx);
	}
	lt_scrollbar(c, (lt_rect){r.x + r.w - 1, r.y, 1, r.h}, t, l->nshown, l->offset, r.h);
}

lt_wres lt_list_event(lt_list *l, lt_rect r, const lt_event *ev)
{
	if (r.w < 1 || r.h < 1) return LT_W_IGNORED;
	int base = ev->key & 0xFFFFFF, mods = ev->key & ~0xFFFFFF;
	int page = r.h > 1 ? r.h - 1 : 1, last = l->nshown - 1, old = l->cursor;
	if (ev->kind == LT_EV_MOUSE) {
		bool bar;
		if (lt_view_mouse(&l->ptr, r, l->nshown, &l->offset, ev, &bar)) { l->follow = false; return LT_W_HANDLED; }
		lt_rect body = lt_view_content(r, l->nshown);
		if (base == LT_K_CLICK && lt_in(body, ev->x, ev->y)) {
			int pos = l->offset + (ev->y - r.y);
			if (pos >= l->nshown) return LT_W_HANDLED;
			bool dbl = lt_ptr_double(&l->ptr, pos);
			l->cursor = pos;
			l->follow = true;
			return dbl ? LT_W_ACTIVATE : (pos != old ? LT_W_CHANGED : LT_W_HANDLED);
		}
		return LT_W_IGNORED;
	}
	if (ev->kind != LT_EV_KEY || (mods & ~LT_MOD_SHIFT)) return LT_W_IGNORED;
	l->follow = true;
	switch (base) {
	case LT_K_UP: l->cursor = lt_clampi(l->cursor - 1, 0, last < 0 ? 0 : last); break;
	case LT_K_DOWN: l->cursor = lt_clampi(l->cursor + 1, 0, last < 0 ? 0 : last); break;
	case LT_K_PGUP: l->cursor = lt_clampi(l->cursor - page, 0, last < 0 ? 0 : last); break;
	case LT_K_PGDN: l->cursor = lt_clampi(l->cursor + page, 0, last < 0 ? 0 : last); break;
	case LT_K_HOME: l->cursor = 0; break;
	case LT_K_END: l->cursor = last < 0 ? 0 : last; break;
	case LT_K_ENTER: return l->nshown ? LT_W_ACTIVATE : LT_W_HANDLED;
	case ' ':
		if (!l->multi || !l->nshown) return LT_W_IGNORED;
		l->it[l->shown[l->cursor]].marked = !l->it[l->shown[l->cursor]].marked;
		l->cursor = lt_clampi(l->cursor + 1, 0, last);
		l->offset = lt_scroll_to(l->offset, l->cursor, r.h, l->nshown);
		return LT_W_CHANGED;
	default: return LT_W_IGNORED;
	}
	l->offset = lt_scroll_to(l->offset, l->cursor, r.h, l->nshown);
	return l->cursor != old ? LT_W_CHANGED : LT_W_HANDLED;
}

int lt_list_item_at(const lt_list *l, lt_rect r, int x, int y)
{
	lt_rect body = lt_view_content(r, l->nshown);
	if (!lt_in(body, x, y)) return -1;
	int pos = l->offset + (y - r.y);
	return pos >= 0 && pos < l->nshown ? l->shown[pos] : -1;
}

/* ------------------------------------------------------------------ scroll view */
void lt_scrollview_init(lt_scrollview *sv) { memset(sv, 0, sizeof *sv); }

lt_rect lt_scrollview_begin(lt_scrollview *sv, lt_canvas *c, lt_rect view, int content_h)
{
	sv->view = view;
	sv->content_h = content_h;
	int maxoff = content_h > view.h ? content_h - view.h : 0;
	sv->offset = lt_clampi(sv->offset, 0, maxoff);
	int barw = content_h > view.h ? 1 : 0;
	lt_canvas_set_clip(c, (lt_rect){view.x, view.y, view.w - barw, view.h});
	return (lt_rect){view.x, view.y - sv->offset, view.w - barw, content_h};
}

void lt_scrollview_end(lt_scrollview *sv, lt_canvas *c, const lt_theme *t)
{
	lt_canvas_reset_clip(c);
	lt_scrollbar(c, (lt_rect){sv->view.x + sv->view.w - 1, sv->view.y, 1, sv->view.h}, t, sv->content_h, sv->offset, sv->view.h);
}

lt_wres lt_scrollview_event(lt_scrollview *sv, const lt_event *ev, bool keys)
{
	int maxoff = sv->content_h > sv->view.h ? sv->content_h - sv->view.h : 0, base = ev->key & 0xFFFFFF;
	if (ev->kind == LT_EV_MOUSE) {
		lt_ptr p = {0, -1, sv->dragging};
		bool bar;
		bool used = lt_view_mouse(&p, sv->view, sv->content_h, &sv->offset, ev, &bar);
		sv->dragging = p.dragging;
		return used ? LT_W_HANDLED : LT_W_IGNORED;
	}
	if (!keys || ev->kind != LT_EV_KEY || (ev->key & ~0xFFFFFF & ~LT_MOD_SHIFT)) return LT_W_IGNORED;
	int old = sv->offset, page = sv->view.h > 2 ? sv->view.h - 1 : 1;
	switch (base) {
	case LT_K_UP: sv->offset -= 1; break;
	case LT_K_DOWN: sv->offset += 1; break;
	case LT_K_PGUP: sv->offset -= page; break;
	case LT_K_PGDN: sv->offset += page; break;
	case LT_K_HOME: sv->offset = 0; break;
	case LT_K_END: sv->offset = maxoff; break;
	default: return LT_W_IGNORED;
	}
	sv->offset = lt_clampi(sv->offset, 0, maxoff);
	return sv->offset != old ? LT_W_CHANGED : LT_W_HANDLED;
}

void lt_scrollview_show(lt_scrollview *sv, int content_y, int h)
{
	int maxoff = sv->content_h > sv->view.h ? sv->content_h - sv->view.h : 0;
	if (content_y < sv->offset) sv->offset = content_y;
	else if (content_y + h > sv->offset + sv->view.h) sv->offset = content_y + h - sv->view.h;
	sv->offset = lt_clampi(sv->offset, 0, maxoff);
}

void lt_list_set_order(lt_list *l, const int *items, int n)
{
	free(l->shown);
	l->shown = malloc((size_t)(n + l->n + 1) * sizeof(int));
	l->nshown = 0;
	for (int i = 0; i < n; i++) if (items[i] >= 0 && items[i] < l->n) l->shown[l->nshown++] = items[i];
	l->cursor = 0; l->offset = 0; l->follow = true;
}
void lt_list_reset_order(lt_list *l) { free(l->filter); l->filter = NULL; refilter(l); l->offset = 0; }
