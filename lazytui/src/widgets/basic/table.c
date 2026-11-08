/* table.c - `table x=3 y=3 rows="5,9,3" title=...`: a container that lays its children out in x columns by y rows, a label line above each
 * cell (the child's label=...), box-drawing rules between them, the frame in the theme's border colour (purple by default).
 * Needs: lt_app (the app layer: appint.h) - it is a kind of the spec language, so it only makes sense with app.c, not alone.
 * One file, one kind: delete it and the spec language loses `table`, nothing else changes. */
#include "appint.h"
#include <stdlib.h>
#include <string.h>

static int cols_of(const anode *n) { int c = (int)app_attr_num(n, "x", 2); return c < 1 ? 1 : c; }

/* each child gets: a row node (fixed height from rows="5,9,3" + the label line, else a share of the height) holding a cell node
 * (top padding 1 = the label line) holding the child's own layout node */
static bool table_build(lt_app *a, anode *n, lt_layout *lay, lt_node *ln, anode *scroll, char *err, size_t errn)
{
	int cols = cols_of(n), rows = (int)app_attr_num(n, "y", 0);
	if (rows < 1) rows = (n->nkid + cols - 1) / cols;
	const char *rh = app_attr(n, "rows");
	lt_node_pad(ln, 1, 1, 1, 1);
	lt_node_gap(ln, 1);
	for (int r = 0; r < rows; r++) {
		lt_size rs = LT_FILL(1);
		const char *p = rh;
		for (int q = 0; q < r && p; q++) { p = strchr(p, ','); if (p) p++; }
		if (p && atoi(p) > 0) rs = LT_FIXED(atoi(p) + 1);
		lt_node *rn = lt_node_add(ln, NULL, LT_ROW, rs);
		lt_node_gap(rn, 1);
		for (int c = 0; c < cols && r * cols + c < n->nkid; c++) {
			anode *k = n->kid[r * cols + c];
			lt_node *cell = lt_node_add(rn, NULL, LT_COL, LT_FILL(1));
			lt_node_pad(cell, 1, 0, 0, 0);
			lt_node *kn = lt_node_add(cell, k->id, !strcmp(k->kind, "row") ? LT_ROW : LT_COL, LT_FILL(1));
			if (!app_build(a, k, lay, kn, scroll, err, errn)) return false;
		}
	}
	return true;
}

static void table_draw(lt_app *a, anode *n, lt_canvas *c, const lt_theme *t, lt_rect w, bool focused)
{
	(void)a; (void)focused;
	lt_panel_ex(c, w, t, app_attr(n, "title"), LT_PANEL_ROUND);
	n->inner = lt_panel_inner(w);
}

/* after the kids: their labels, and the rules between the cells (tees where they meet the frame and each other) */
static void table_over(lt_app *a, anode *n, lt_canvas *c, const lt_theme *t, lt_rect view)
{
	(void)a;
	lt_rect r = n->rect;
	int cols = cols_of(n), rows = (n->nkid + cols - 1) / cols;
	bool uni = lt_canvas_caps(c)->unicode;
	lt_style rule = t->s[LT_T_BORDER];
	for (int i = 0; i < n->nkid; i++) {
		anode *k = n->kid[i];
		const char *lab = app_attr(k, "label");
		if (lab && k->drawn) lt_text(c, view, k->rect.x, k->rect.y - 1, lab, t->s[LT_T_DIM], k->rect.w);
	}
	for (int cx = 0; cx < cols - 1 && cx < n->nkid; cx++) {
		int x = n->kid[cx]->rect.x + n->kid[cx]->rect.w;
		for (int y = r.y + 1; y < r.y + r.h - 1; y++) lt_put(c, view, x, y, uni ? 0x2502 : '|', rule);
		if (!app_attr(n, "title")) lt_put(c, view, x, r.y, uni ? 0x252C : '+', rule);      /* a title owns the top edge */
		lt_put(c, view, x, r.y + r.h - 1, uni ? 0x2534 : '+', rule);
	}
	for (int ry = 0; ry < rows - 1; ry++) {
		const anode *first = n->kid[ry * cols];
		int y = first->rect.y + first->rect.h;
		for (int x = r.x + 1; x < r.x + r.w - 1; x++) lt_put(c, view, x, y, uni ? 0x2500 : '-', rule);
		lt_put(c, view, r.x, y, uni ? 0x251C : '+', rule);
		lt_put(c, view, r.x + r.w - 1, y, uni ? 0x2524 : '+', rule);
		for (int cx = 0; cx < cols - 1 && ry * cols + cx < n->nkid; cx++) {
			const anode *q = n->kid[ry * cols + cx];
			lt_put(c, view, q->rect.x + q->rect.w, y, uni ? 0x253C : '+', rule);
		}
	}
}

const kind_ops kind_table = {"table", false, NULL, NULL, table_draw, NULL, NULL, true, table_build, table_over};
