/* tree.c - a tree of nodes drawn like `tree(1)` with expand/collapse arrows, keyboard and mouse. The visible rows are
 * rebuilt (depth-first over the expanded nodes) whenever the shape changes; the row layout is a pure function of the depth,
 * so hit testing needs no drawing first:  [3 columns per level: "|  " or "   "; the node's own level "|- " / "`- "][arrow][space][label] */
#include "internal.h"

typedef struct { char *label; int id, parent, depth; bool expanded, has_kids, last; } node;
struct lt_tree {
	node *n; int count, cap;
	int *vis; int nvis;                  /* visible node indexes, top to bottom */
	bool dirty;
	int cursor;                          /* position within vis */
	int offset;
	bool follow;
	lt_ptr ptr;
	int rows;
};

lt_tree *lt_tree_new(void) { lt_tree *t = calloc(1, sizeof *t); t->dirty = true; t->follow = true; return t; }
void lt_tree_free(lt_tree *t)
{
	if (!t) return;
	for (int i = 0; i < t->count; i++) free(t->n[i].label);
	free(t->n); free(t->vis); free(t);
}

int lt_tree_add(lt_tree *t, int parent, const char *label, int id)
{
	if (parent >= t->count) parent = -1;
	if (t->count == t->cap) { t->cap = t->cap ? t->cap * 2 : 16; t->n = realloc(t->n, (size_t)t->cap * sizeof *t->n); }
	int d = parent >= 0 ? t->n[parent].depth + 1 : 0;
	t->n[t->count] = (node){strdup(label ? label : ""), id, parent, d, false, false, true};
	if (parent >= 0) t->n[parent].has_kids = true;
	t->dirty = true;
	return t->count++;
}

/* children of `parent` are the nodes whose parent field matches, in insertion order */
static void walk(lt_tree *t, int parent)
{
	int lastchild = -1;
	for (int i = 0; i < t->count; i++) if (t->n[i].parent == parent) lastchild = i;
	for (int i = 0; i < t->count; i++) {
		if (t->n[i].parent != parent) continue;
		t->n[i].last = i == lastchild;
		t->vis[t->nvis++] = i;
		if (t->n[i].has_kids && t->n[i].expanded) walk(t, i);
	}
}
static void rebuild(lt_tree *t)
{
	if (!t->dirty) return;
	int keep = t->nvis && t->cursor < t->nvis ? t->vis[t->cursor] : -1;
	free(t->vis);
	t->vis = malloc((size_t)(t->count + 1) * sizeof(int));
	t->nvis = 0;
	walk(t, -1);
	t->cursor = 0;
	for (int i = 0; i < t->nvis; i++) if (t->vis[i] == keep) t->cursor = i;
	t->dirty = false;
}

void lt_tree_set_expanded(lt_tree *t, int node_i, bool e) { if (node_i >= 0 && node_i < t->count && t->n[node_i].has_kids) { t->n[node_i].expanded = e; t->dirty = true; } }
void lt_tree_expand_all(lt_tree *t, bool e) { for (int i = 0; i < t->count; i++) if (t->n[i].has_kids) t->n[i].expanded = e; t->dirty = true; }
int lt_tree_count(const lt_tree *t) { return t->count; }
int lt_tree_id(const lt_tree *t, int i) { return i >= 0 && i < t->count ? t->n[i].id : -1; }
const char *lt_tree_label(const lt_tree *t, int i) { return i >= 0 && i < t->count ? t->n[i].label : ""; }
bool lt_tree_is_expanded(const lt_tree *t, int i) { return i >= 0 && i < t->count && t->n[i].expanded; }
int lt_tree_selected(const lt_tree *t) { rebuild((lt_tree *)t); return t->nvis ? t->vis[lt_clampi(t->cursor, 0, t->nvis - 1)] : -1; }
void lt_tree_select(lt_tree *t, int i)
{
	if (i < 0 || i >= t->count) return;
	for (int p = t->n[i].parent; p >= 0; p = t->n[p].parent) t->n[p].expanded = true;
	t->dirty = true;
	rebuild(t);
	for (int k = 0; k < t->nvis; k++) if (t->vis[k] == i) { t->cursor = k; t->follow = true; }
}

/* is the ancestor of `i` at depth d the last child of its parent? (decides "|  " or "   " for that level) */
static bool ancestor_last(const lt_tree *t, int i, int d)
{
	while (t->n[i].depth > d) i = t->n[i].parent;
	return t->n[i].last;
}

void lt_tree_draw(lt_tree *t, lt_canvas *c, lt_rect r, const lt_theme *th, bool focused)
{
	if (r.w < 1 || r.h < 1) return;
	rebuild(t);
	lt_rect all = lt_full(c);
	t->rows = r.h;
	t->offset = t->follow ? lt_scroll_to(t->offset, t->cursor, r.h, t->nvis) : lt_clampi(t->offset, 0, t->nvis > r.h ? t->nvis - r.h : 0);
	lt_rect body = lt_view_content(r, t->nvis);
	bool uni = lt_canvas_caps(c)->unicode;
	for (int row = 0; row < r.h; row++) {
		int pos = t->offset + row;
		if (pos >= t->nvis) break;
		int i = t->vis[pos];
		const node *nd = &t->n[i];
		bool cur = pos == t->cursor;
		lt_style st = cur && focused ? th->s[LT_T_SELECT] : th->s[LT_T_TEXT];
		lt_style guide = th->s[LT_T_DIM];
		if (cur && focused) guide = st;
		lt_fill(c, (lt_rect){body.x, body.y + row, body.w, 1}, ' ', st);
		int y = body.y + row, x = body.x;
		for (int d = 0; d < nd->depth; d++) {
			const char *seg;
			if (d == nd->depth - 1) seg = nd->last ? (uni ? "\xE2\x94\x94\xE2\x94\x80 " : "`- ") : (uni ? "\xE2\x94\x9C\xE2\x94\x80 " : "|- ");
			else seg = ancestor_last(t, i, d + 1) ? "   " : (uni ? "\xE2\x94\x82  " : "|  ");
			lt_text(c, all, x, y, seg, guide, 3);
			x += 3;
		}
		lt_style ar = cur && focused ? st : th->s[LT_T_ACCENT];
		if (nd->has_kids) lt_put(c, all, x, y, nd->expanded ? (uni ? 0x25BE : 'v') : (uni ? 0x25B8 : '>'), ar);
		else lt_put(c, all, x, y, uni ? 0xB7 : '-', guide);
		int lw = body.x + body.w - (x + 2);
		if (lw > 0) {
			int w = lt_utf8_width(nd->label);
			if (w <= lw) lt_text(c, all, x + 2, y, nd->label, st, lw);
			else { int u = lt_text(c, all, x + 2, y, nd->label, st, lw > 1 ? lw - 1 : 0); lt_put(c, all, x + 2 + u, y, uni ? 0x2026 : '.', st); }
		}
	}
	lt_scrollbar(c, (lt_rect){r.x + r.w - 1, r.y, 1, r.h}, th, t->nvis, t->offset, r.h);
}

static lt_wres toggle(lt_tree *t, int i)
{
	if (!t->n[i].has_kids) return LT_W_ACTIVATE;
	t->n[i].expanded = !t->n[i].expanded;
	t->dirty = true;
	return LT_W_TOGGLE;
}

lt_wres lt_tree_event(lt_tree *t, lt_rect r, const lt_event *ev)
{
	if (r.w < 1 || r.h < 1) return LT_W_IGNORED;
	rebuild(t);
	int base = ev->key & 0xFFFFFF, mods = ev->key & ~0xFFFFFF;
	int page = r.h > 1 ? r.h - 1 : 1, last = t->nvis - 1, old = t->cursor;
	if (ev->kind == LT_EV_MOUSE) {
		bool bar;
		if (lt_view_mouse(&t->ptr, r, t->nvis, &t->offset, ev, &bar)) { t->follow = false; return LT_W_HANDLED; }
		lt_rect body = lt_view_content(r, t->nvis);
		if (base == LT_K_CLICK && lt_in(body, ev->x, ev->y)) {
			int pos = t->offset + (ev->y - r.y);
			if (pos >= t->nvis) return LT_W_HANDLED;
			int i = t->vis[pos];
			bool dbl = lt_ptr_double(&t->ptr, pos);
			t->cursor = pos;
			t->follow = true;
			int arrow_x = body.x + t->n[i].depth * 3;
			if (t->n[i].has_kids && ev->x >= arrow_x && ev->x <= arrow_x + 1) return toggle(t, i);       /* the arrow (and the gap after it) */
			if (dbl) return toggle(t, i);
			return pos != old ? LT_W_CHANGED : LT_W_HANDLED;
		}
		return LT_W_IGNORED;
	}
	if (ev->kind != LT_EV_KEY || (mods & ~LT_MOD_SHIFT) || t->nvis == 0) return LT_W_IGNORED;
	t->follow = true;
	int i = t->vis[t->cursor];
	switch (base) {
	case LT_K_UP: t->cursor = lt_clampi(t->cursor - 1, 0, last); break;
	case LT_K_DOWN: t->cursor = lt_clampi(t->cursor + 1, 0, last); break;
	case LT_K_PGUP: t->cursor = lt_clampi(t->cursor - page, 0, last); break;
	case LT_K_PGDN: t->cursor = lt_clampi(t->cursor + page, 0, last); break;
	case LT_K_HOME: t->cursor = 0; break;
	case LT_K_END: t->cursor = last; break;
	case LT_K_RIGHT:
		if (t->n[i].has_kids && !t->n[i].expanded) return toggle(t, i);
		if (t->n[i].has_kids && t->cursor < last) t->cursor++;                    /* already open: step into the first child */
		break;
	case LT_K_LEFT:
		if (t->n[i].has_kids && t->n[i].expanded) return toggle(t, i);
		if (t->n[i].parent >= 0) for (int k = 0; k < t->nvis; k++) if (t->vis[k] == t->n[i].parent) t->cursor = k;
		break;
	case LT_K_ENTER: case ' ': return toggle(t, i);
	default: return LT_W_IGNORED;
	}
	t->offset = lt_scroll_to(t->offset, t->cursor, r.h, t->nvis);
	return t->cursor != old ? LT_W_CHANGED : LT_W_HANDLED;
}

int lt_tree_visible(const lt_tree *t) { rebuild((lt_tree *)t); return t->nvis; }
bool lt_tree_has_children(const lt_tree *t, int node) { return node >= 0 && node < t->count && t->n[node].has_kids; }
int lt_tree_node_at(const lt_tree *t, lt_rect r, int x, int y)
{
	rebuild((lt_tree *)t);
	lt_rect body = lt_view_content(r, t->nvis);
	if (!lt_in(body, x, y)) return -1;
	int pos = t->offset + (y - r.y);
	return pos >= 0 && pos < t->nvis ? t->vis[pos] : -1;
}

int lt_tree_parent(const lt_tree *t, int node) { return node >= 0 && node < t->count ? t->n[node].parent : -1; }
int lt_tree_child_count(const lt_tree *t, int node)
{
	int k = 0;
	for (int i = 0; i < t->count; i++) if (t->n[i].parent == node) k++;
	return k;
}
int lt_tree_child(const lt_tree *t, int node, int idx)
{
	int k = 0;
	for (int i = 0; i < t->count; i++) if (t->n[i].parent == node) { if (k == idx) return i; k++; }
	return -1;
}
