/* dropdown.c - a one-row box that opens a popup list. The popup opens below the box, or above when there is no room, and is
 * drawn by a separate call so it can go over everything else. While it is open it is modal: every mouse event is consumed
 * (a click outside closes it) and keys only drive the popup. */
#include "internal.h"

struct lt_dropdown { lt_list *list; lt_tree *tree; bool open; int selected; int n; int maxw; };

lt_dropdown *lt_dropdown_new(const char *const *items, int n)
{
	lt_dropdown *d = calloc(1, sizeof *d);
	d->list = lt_list_new();
	for (int i = 0; i < n; i++) {
		lt_list_add(d->list, items[i], i);
		int w = lt_utf8_width(items[i]);
		if (w > d->maxw) d->maxw = w;
	}
	d->n = n;
	return d;
}
lt_dropdown *lt_dropdown_from_tree(lt_tree *tree)
{
	lt_dropdown *d = calloc(1, sizeof *d);
	d->tree = tree;
	d->selected = -1;
	d->maxw = 28;
	return d;
}
int lt_dropdown_selected_node(const lt_dropdown *d) { return d->tree ? d->selected : -1; }
void lt_dropdown_free(lt_dropdown *d) { if (d) { lt_list_free(d->list); free(d); } }
int lt_dropdown_selected(const lt_dropdown *d) { return d->selected; }
void lt_dropdown_set_selected(lt_dropdown *d, int i) { if (d->tree) { if (i >= 0 && i < lt_tree_count(d->tree)) d->selected = i; } else if (i >= 0 && i < d->n) d->selected = i; }
bool lt_dropdown_is_open(const lt_dropdown *d) { return d->open; }
void lt_dropdown_close(lt_dropdown *d) { d->open = false; }

/* outer = the popup with its border, inner = the list area */
static void geometry(const lt_dropdown *d, lt_rect box, int canvas_h, lt_rect *outer, lt_rect *inner)
{
	int total = d->tree ? lt_tree_visible(d->tree) : d->n, rows = total < 8 ? total : (d->tree ? 12 : 8);
	if (rows > total) rows = total;
	int below = canvas_h - (box.y + 1) - 2, above = box.y - 2;
	bool up = rows > below && above > below;
	int room = up ? above : below;
	if (rows > room) rows = room > 1 ? room : 1;
	int w = d->maxw + 4 + (total > rows ? 1 : 0);
	if (w < box.w) w = box.w;
	outer->x = box.x; outer->w = w; outer->h = rows + 2;
	outer->y = up ? box.y - outer->h : box.y + 1;
	*inner = (lt_rect){outer->x + 1, outer->y + 1, outer->w - 2, rows};
}

void lt_dropdown_draw(lt_dropdown *d, lt_canvas *c, lt_rect box, const lt_theme *t, bool focused)
{
	if (box.w < 3 || box.h < 1) return;
	box.h = 1;
	lt_rect all = lt_full(c);
	lt_style st = focused || d->open ? t->s[LT_T_SELECT] : t->s[LT_T_TEXT];
	lt_fill(c, box, ' ', st);
	bool uni = lt_canvas_caps(c)->unicode;
	lt_put(c, all, box.x, box.y, '[', focused ? st : t->s[LT_T_BORDER]);
	lt_put(c, all, box.x + box.w - 1, box.y, ']', focused ? st : t->s[LT_T_BORDER]);
	const char *label = d->tree ? (d->selected >= 0 ? lt_tree_label(d->tree, d->selected) : "(choose)") : d->n ? lt_list_label(d->list, d->selected) : "";
	lt_text(c, all, box.x + 2, box.y, label, st, box.w - 6 > 0 ? box.w - 6 : 0);
	lt_put(c, all, box.x + box.w - 3, box.y, d->open ? (uni ? 0x25B4 : '^') : (uni ? 0x25BE : 'v'), focused || d->open ? st : t->s[LT_T_ACCENT]);
}

void lt_dropdown_draw_popup(lt_dropdown *d, lt_canvas *c, lt_rect box, const lt_theme *t)
{
	if (!d->open || (d->n == 0 && !d->tree)) return;
	lt_rect outer, inner;
	geometry(d, box, lt_canvas_h(c), &outer, &inner);
	lt_fill(c, outer, ' ', t->s[LT_T_TEXT]);                      /* opaque: hides what is underneath */
	lt_panel(c, outer, t, NULL);
	if (d->tree) lt_tree_draw(d->tree, c, inner, t, true); else lt_list_draw(d->list, c, inner, t, true);
}

static lt_wres choose(lt_dropdown *d)
{
	int cur = lt_list_cursor(d->list);
	d->open = false;
	if (cur >= 0 && cur != d->selected) { d->selected = cur; return LT_W_CHANGED; }
	return LT_W_CLOSED;
}

static lt_wres tree_event(lt_dropdown *d, lt_rect box, const lt_event *ev, int canvas_h)
{
	int base = ev->key & 0xFFFFFF;
	lt_rect outer, inner;
	if (!d->open) {
		if ((ev->kind == LT_EV_MOUSE && base == LT_K_CLICK && lt_in(box, ev->x, ev->y)) ||
		    (ev->kind == LT_EV_KEY && (base == LT_K_ENTER || base == ' ' || base == LT_K_DOWN) && !(ev->key & (LT_MOD_CTRL | LT_MOD_ALT)))) {
			d->open = true;
			if (d->selected >= 0) lt_tree_select(d->tree, d->selected);
			return LT_W_HANDLED;
		}
		return LT_W_IGNORED;
	}
	geometry(d, box, canvas_h, &outer, &inner);
	if (ev->kind == LT_EV_MOUSE) {
		if (base == LT_K_CLICK && !lt_in(outer, ev->x, ev->y)) { d->open = false; return LT_W_CLOSED; }
		if (base == LT_K_CLICK) {
			int node = lt_tree_node_at(d->tree, inner, ev->x, ev->y);
			if (node >= 0 && !lt_tree_has_children(d->tree, node)) {              /* a leaf: choose it */
				lt_tree_select(d->tree, node);
				d->open = false;
				if (node != d->selected) { d->selected = node; return LT_W_CHANGED; }
				return LT_W_CLOSED;
			}
		}
		lt_tree_event(d->tree, inner, ev);                                          /* arrows toggle, wheel, scrollbar */
		return LT_W_HANDLED;
	}
	if (ev->kind != LT_EV_KEY) return LT_W_HANDLED;
	if (base == LT_K_ESC) { d->open = false; return LT_W_CLOSED; }
	lt_wres r = lt_tree_event(d->tree, inner, ev);
	if (r == LT_W_ACTIVATE) {                                                       /* Enter on a leaf */
		int node = lt_tree_selected(d->tree);
		d->open = false;
		if (node >= 0 && node != d->selected) { d->selected = node; return LT_W_CHANGED; }
		return LT_W_CLOSED;
	}
	return LT_W_HANDLED;
}

lt_wres lt_dropdown_event(lt_dropdown *d, lt_rect box, const lt_event *ev, int canvas_h)
{
	int base = ev->key & 0xFFFFFF;
	box.h = 1;
	if (d->tree) return tree_event(d, box, ev, canvas_h);
	if (!d->open) {
		if (ev->kind == LT_EV_MOUSE && base == LT_K_CLICK && lt_in(box, ev->x, ev->y) && d->n) { d->open = true; lt_list_set_cursor(d->list, d->selected); return LT_W_HANDLED; }
		if (ev->kind == LT_EV_KEY && d->n && (base == LT_K_ENTER || base == ' ' || base == LT_K_DOWN || base == LT_K_UP) && !(ev->key & (LT_MOD_CTRL | LT_MOD_ALT))) {
			d->open = true;
			lt_list_set_cursor(d->list, d->selected);
			return LT_W_HANDLED;
		}
		return LT_W_IGNORED;
	}
	lt_rect outer, inner;
	geometry(d, box, canvas_h, &outer, &inner);
	if (ev->kind == LT_EV_MOUSE) {
		if (base == LT_K_CLICK && !lt_in(outer, ev->x, ev->y)) { d->open = false; return LT_W_CLOSED; }
		if (base == LT_K_CLICK) {
			int item = lt_list_item_at(d->list, inner, ev->x, ev->y);
			if (item >= 0) { lt_list_set_cursor(d->list, item); return choose(d); }
		}
		lt_list_event(d->list, inner, ev);                          /* wheel, scrollbar */
		return LT_W_HANDLED;
	}
	if (ev->kind != LT_EV_KEY) return LT_W_HANDLED;
	if (base == LT_K_ESC) { d->open = false; return LT_W_CLOSED; }
	if (base == LT_K_ENTER) return choose(d);
	lt_list_event(d->list, inner, ev);
	return LT_W_HANDLED;
}
