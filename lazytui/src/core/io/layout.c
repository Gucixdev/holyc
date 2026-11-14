/* layout.c - a tree of boxes turned into Cassowary constraints. Each node owns four variables (x y w h). The tree is
 * rebuilt into a solver only when its structure changes; resizing the terminal is two edit-variable suggestions.
 *
 * Strength ladder (highest first), chosen so a small terminal degrades in a predictable order:
 *   root size (edit)  >  FIXED / PCT sizes  >  MIN / MAX bounds  >  "children fill the box"  >  FILL weight ratios
 * Positions inside a box (each child starts where the previous ends) and w,h >= 0 are REQUIRED - they are always
 * satisfiable, so the solver never reports a conflict however small the terminal is. */
#include "lazytui.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define S_EDIT (LT_REQUIRED / 2)
#define S_FIXED (LT_STRONG * 20)
#define S_BOUND (LT_STRONG * 5)
#define S_FIT (LT_STRONG)
#define S_RATIO (LT_MEDIUM)

struct lt_node {
	char *name;
	lt_dir dir;
	lt_size size;
	lt_node *parent, **kid;
	int nkid, capkid;
	int gap, pad[4];                 /* top right bottom left */
	lt_var x, y, w, h;
	lt_rect rect;
};

struct lt_layout {
	lt_node *root;
	lt_solver *s;
	bool dirty;
	int lw, lh;
};

static lt_layout *g_layouts[8];                  /* small registry: root node -> layout, so editing a node can flag its layout */
static lt_node *g_roots[8];

static lt_node *node_new(lt_node *parent, const char *name, lt_dir dir, lt_size size)
{
	lt_node *n = calloc(1, sizeof *n);
	n->name = name ? strdup(name) : NULL;
	n->dir = dir; n->size = size; n->parent = parent;
	return n;
}
static void node_free(lt_node *n)
{
	for (int i = 0; i < n->nkid; i++) node_free(n->kid[i]);
	free(n->kid); free(n->name); free(n);
}

lt_layout *lt_layout_new(void) { return calloc(1, sizeof(lt_layout)); }
void lt_layout_free(lt_layout *l)
{
	if (!l) return;
	for (int i = 0; i < 8; i++) if (g_layouts[i] == l) g_layouts[i] = NULL, g_roots[i] = NULL;
	if (l->root) node_free(l->root);
	lt_solver_free(l->s);
	free(l);
}

static lt_node *root_of(lt_node *n) { while (n->parent) n = n->parent; return n; }
lt_node *lt_layout_root(lt_layout *l, lt_dir dir)
{
	if (!l->root) {
		l->root = node_new(NULL, "root", dir, LT_FILL(1));
		l->dirty = true;
		for (int i = 0; i < 8; i++) if (!g_layouts[i]) { g_layouts[i] = l; g_roots[i] = l->root; break; }
	}
	l->root->dir = dir;
	return l->root;
}

static void mark_dirty(lt_node *n)
{
	lt_node *r = root_of(n);
	for (int i = 0; i < 8; i++) if (g_roots[i] == r && g_layouts[i]) g_layouts[i]->dirty = true;
}

lt_node *lt_node_add(lt_node *parent, const char *name, lt_dir dir, lt_size size)
{
	lt_node *n = node_new(parent, name, dir, size);
	if (parent->nkid == parent->capkid) { parent->capkid = parent->capkid ? parent->capkid * 2 : 4; parent->kid = realloc(parent->kid, (size_t)parent->capkid * sizeof(lt_node *)); }
	parent->kid[parent->nkid++] = n;
	mark_dirty(parent);
	return n;
}
void lt_node_gap(lt_node *n, int gap) { n->gap = gap < 0 ? 0 : gap; mark_dirty(n); }
void lt_node_pad(lt_node *n, int t, int r, int b, int l) { n->pad[0] = t; n->pad[1] = r; n->pad[2] = b; n->pad[3] = l; mark_dirty(n); }
lt_rect lt_node_rect(const lt_node *n) { return n->rect; }

static lt_node *find(lt_node *n, const char *name)
{
	if (n->name && !strcmp(n->name, name)) return n;
	for (int i = 0; i < n->nkid; i++) { lt_node *f = find(n->kid[i], name); if (f) return f; }
	return NULL;
}
lt_node *lt_layout_find(lt_layout *l, const char *name) { return l->root ? find(l->root, name) : NULL; }

/* ------------------------------------------------------------------ constraints */
static void ge0(lt_solver *s, lt_var v, double strength) { lt_term t = {v, 1}; lt_solver_add(s, &t, 1, 0, LT_GE, strength); }

static void make_vars(lt_solver *s, lt_node *n)
{
	n->x = lt_var_new(s); n->y = lt_var_new(s); n->w = lt_var_new(s); n->h = lt_var_new(s);
	ge0(s, n->w, LT_REQUIRED); ge0(s, n->h, LT_REQUIRED);
	for (int i = 0; i < n->nkid; i++) make_vars(s, n->kid[i]);
}

static void constrain(lt_solver *s, lt_node *n)
{
	int k = n->nkid;
	if (!k) return;
	/* the inner box: x + padL .. x + w - padR */
	int pt = n->pad[0], pr = n->pad[1], pb = n->pad[2], pl = n->pad[3];
	bool horiz = n->dir == LT_ROW, stack = n->dir == LT_STACK;
	for (int i = 0; i < k; i++) {
		lt_node *c = n->kid[i];
		/* cross axis: the child spans the inner box */
		if (horiz) {
			{ lt_term t[2] = {{c->y, 1}, {n->y, -1}}; lt_solver_add(s, t, 2, -pt, LT_EQ, LT_REQUIRED); }
			{ lt_term t[2] = {{c->h, 1}, {n->h, -1}}; lt_solver_add(s, t, 2, pt + pb, LT_EQ, LT_REQUIRED); }
		} else if (!stack) {
			{ lt_term t[2] = {{c->x, 1}, {n->x, -1}}; lt_solver_add(s, t, 2, -pl, LT_EQ, LT_REQUIRED); }
			{ lt_term t[2] = {{c->w, 1}, {n->w, -1}}; lt_solver_add(s, t, 2, pl + pr, LT_EQ, LT_REQUIRED); }
		}
		if (stack) {
			{ lt_term t[2] = {{c->x, 1}, {n->x, -1}}; lt_solver_add(s, t, 2, -pl, LT_EQ, LT_REQUIRED); }
			{ lt_term t[2] = {{c->w, 1}, {n->w, -1}}; lt_solver_add(s, t, 2, pl + pr, LT_EQ, LT_REQUIRED); }
			{ lt_term t[2] = {{c->y, 1}, {n->y, -1}}; lt_solver_add(s, t, 2, -pt, LT_EQ, LT_REQUIRED); }
			{ lt_term t[2] = {{c->h, 1}, {n->h, -1}}; lt_solver_add(s, t, 2, pt + pb, LT_EQ, LT_REQUIRED); }
		}
	}
	if (!stack) {
		lt_var np = horiz ? n->x : n->y, nl = horiz ? n->w : n->h;
		int p0 = horiz ? pl : pt, p1 = horiz ? pr : pb;
		/* main axis: chain the children */
		for (int i = 0; i < k; i++) {
			lt_node *c = n->kid[i];
			lt_var cp = horiz ? c->x : c->y, cl = horiz ? c->w : c->h;
			if (i == 0) { lt_term t[2] = {{cp, 1}, {np, -1}}; lt_solver_add(s, t, 2, -p0, LT_EQ, LT_REQUIRED); }
			else {
				lt_node *pv = n->kid[i - 1];
				lt_var pp = horiz ? pv->x : pv->y, pl_ = horiz ? pv->w : pv->h;
				lt_term t[3] = {{cp, 1}, {pp, -1}, {pl_, -1}};
				lt_solver_add(s, t, 3, -n->gap, LT_EQ, LT_REQUIRED);
			}
			/* sizes */
			switch (c->size.kind) {
			case LT_SZ_FIXED: { lt_term t = {cl, 1}; lt_solver_add(s, &t, 1, -c->size.v, LT_EQ, S_FIXED); break; }
			case LT_SZ_PCT: {
				double q = c->size.v / 100.0;
				lt_term t[2] = {{cl, 1}, {nl, -q}};
				lt_solver_add(s, t, 2, q * (p0 + p1), LT_EQ, S_FIXED);
				break;
			}
			case LT_SZ_MIN: { lt_term t = {cl, 1}; lt_solver_add(s, &t, 1, -c->size.v, LT_GE, S_BOUND); break; }
			case LT_SZ_MAX: { lt_term t = {cl, 1}; lt_solver_add(s, &t, 1, -c->size.v, LT_LE, S_BOUND); break; }
			default: break;
			}
		}
		/* the last child ends at the inner edge: "the children fill the box" */
		lt_node *last = n->kid[k - 1];
		lt_var lp = horiz ? last->x : last->y, ll = horiz ? last->w : last->h;
		lt_term t[4] = {{lp, 1}, {ll, 1}, {np, -1}, {nl, -1}};
		lt_solver_add(s, t, 4, p1, LT_EQ, S_FIT);
		/* fill weights: w_i * weight_0 == w_0 * weight_i among the children that share the rest */
		lt_node *f0 = NULL;
		for (int i = 0; i < k; i++) {
			lt_node *c = n->kid[i];
			bool shares = c->size.kind == LT_SZ_FILL || c->size.kind == LT_SZ_MIN || c->size.kind == LT_SZ_MAX;
			if (!shares) continue;
			double wt = c->size.kind == LT_SZ_FILL && c->size.weight > 0 ? c->size.weight : 1;
			if (!f0) { f0 = c; continue; }
			double w0 = f0->size.kind == LT_SZ_FILL && f0->size.weight > 0 ? f0->size.weight : 1;
			lt_var a = horiz ? c->w : c->h, b = horiz ? f0->w : f0->h;
			lt_term r[2] = {{a, w0}, {b, -wt}};
			lt_solver_add(s, r, 2, 0, LT_EQ, S_RATIO);
		}
	}
	for (int i = 0; i < k; i++) constrain(s, n->kid[i]);
}

static void collect(lt_solver *s, lt_node *n)
{
	double x = lt_solver_value(s, n->x), y = lt_solver_value(s, n->y), w = lt_solver_value(s, n->w), h = lt_solver_value(s, n->h);
	/* cumulative rounding: both edges are rounded, never the size, so neighbours meet exactly and sizes sum to the whole */
	int x0 = (int)floor(x + 0.5), y0 = (int)floor(y + 0.5), x1 = (int)floor(x + w + 0.5), y1 = (int)floor(y + h + 0.5);
	n->rect = (lt_rect){x0, y0, x1 - x0, y1 - y0};
	for (int i = 0; i < n->nkid; i++) collect(s, n->kid[i]);
}

bool lt_layout_solve(lt_layout *l, int w, int h)
{
	if (!l->root) return false;
	if (l->dirty || !l->s) {
		lt_solver_free(l->s);
		l->s = lt_solver_new();
		lt_node *r = l->root;
		make_vars(l->s, r);
		{ lt_term t = {r->x, 1}; lt_solver_add(l->s, &t, 1, 0, LT_EQ, LT_REQUIRED); }
		{ lt_term t = {r->y, 1}; lt_solver_add(l->s, &t, 1, 0, LT_EQ, LT_REQUIRED); }
		constrain(l->s, r);
		lt_solver_edit(l->s, r->w, S_EDIT);
		lt_solver_edit(l->s, r->h, S_EDIT);
		l->dirty = false;
		l->lw = l->lh = -1;
	}
	if (w != l->lw) { lt_solver_suggest(l->s, l->root->w, w); l->lw = w; }
	if (h != l->lh) { lt_solver_suggest(l->s, l->root->h, h); l->lh = h; }
	collect(l->s, l->root);
	return true;
}
