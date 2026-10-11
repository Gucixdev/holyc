/* kinds.c - the widget kinds of the spec language that live behind kind_ops (see appint.h). Each is: init (state), draw, event, report.
 * Model conventions: `bind=key` is the widget's value; events go out as (id, "change" | "activate" | "toggle" | "mode"). */
#include "appint.h"
#include <stdio.h>

/* ================================================================== treeview: one tree, two looks
 * Tab switches the look (that is the panel's mode; focus moves with Shift-Tab / F6 / the mouse): a plain tree, or a dropdown whose popup
 * is the tree. Fill the tree with lt_app_tree(app, id). For the breadcrumbs look (the path above as clickable segments, its children as
 * buttons below), use the standalone `breadcrumbs` kind just below instead - it used to be treeview's second mode, but with its own kind
 * to reach for directly, keeping it as a mode here too was just the same widget under two names. */
typedef struct { int mode; } tvst;
enum { TV_TREE, TV_DROP, TV_N };
static const char *TV_NAMES[TV_N] = {"tree", "dropdown"};

static void tv_init(lt_app *a, anode *n)
{
	(void)a;
	tvst *s = calloc(1, sizeof *s);
	n->tree = lt_tree_new();
	n->dd = lt_dropdown_from_tree(n->tree);
	const char *m = app_attr(n, "mode");
	for (int i = 0; m && i < TV_N; i++) if (!strcmp(m, TV_NAMES[i])) s->mode = i;
	n->st = s;
}
static void tv_destroy(anode *n) { free(n->st); }

static void tv_draw(lt_app *a, anode *n, lt_canvas *c, const lt_theme *t, lt_rect w, bool focused)
{
	tvst *s = n->st;
	lt_rect all = lt_full(c);
	bool foot = w.h > 4;
	lt_rect body = {w.x, w.y, w.w, foot ? w.h - 1 : w.h};
	const char *bind = app_attr(n, "bind");
	if (s->mode == TV_TREE) lt_tree_draw(n->tree, c, body, t, focused);
	else {
		if (bind) lt_dropdown_set_selected(n->dd, (int)lt_app_number(a, bind));
		body = (lt_rect){w.x, w.y, w.w, 1};
		lt_dropdown_draw(n->dd, c, body, t, focused);
		if (lt_dropdown_is_open(n->dd)) a->modal = n;
	}
	if (foot) {
		char b[80];
		snprintf(b, sizeof b, "%s view  (Tab: %s)", TV_NAMES[s->mode], TV_NAMES[(s->mode + 1) % TV_N]);
		lt_fill(c, (lt_rect){w.x, w.y + w.h - 1, w.w, 1}, ' ', t->s[LT_T_TEXT]);
		lt_text(c, all, w.x, w.y + w.h - 1, b, t->s[LT_T_DIM], w.w);
	}
	n->inner = body;
}

static lt_wres tv_event(lt_app *a, anode *n, const lt_event *ev)
{
	tvst *s = n->st;
	if (ev->kind == LT_EV_KEY && ev->key == LT_K_TAB && !(s->mode == TV_DROP && lt_dropdown_is_open(n->dd))) {
		s->mode = (s->mode + 1) % TV_N;
		lt_app_emit(a, n->id, "mode", TV_NAMES[s->mode], s->mode);
		return LT_W_HANDLED;
	}
	if (s->mode == TV_TREE) return lt_tree_event(n->tree, n->inner, ev);
	return lt_dropdown_event(n->dd, n->inner, ev, app_canvas_h(a));
}

static void tv_report(lt_app *a, anode *n, lt_wres r)
{
	tvst *s = n->st;
	const char *bind = app_attr(n, "bind");
	int node = s->mode == TV_TREE ? lt_tree_selected(n->tree) : lt_dropdown_selected_node(n->dd);
	if (node < 0) return;
	if (bind) lt_app_set(a, bind, node);
	lt_app_emit(a, n->id, r == LT_W_ACTIVATE ? "activate" : r == LT_W_TOGGLE ? "toggle" : "change", lt_tree_label(n->tree, node), node);
}

/* ================================================================== breadcrumbs: lt_treebar on its own, no mode switching
 * (the "breadcrumbs" look inside `treeview` above, as a plain spec kind for when a panel only ever wants that one look).
 * Fill the tree with lt_app_tree(app, id), same as treeview. */
typedef struct { lt_treebar *bar; } bcst;
static void bc_init(lt_app *a, anode *n) { (void)a; bcst *s = calloc(1, sizeof *s); n->tree = lt_tree_new(); s->bar = lt_treebar_new(n->tree); n->st = s; }
static void bc_destroy(anode *n) { bcst *s = n->st; if (s) lt_treebar_free(s->bar); free(s); }
static void bc_draw(lt_app *a, anode *n, lt_canvas *c, const lt_theme *t, lt_rect w, bool focused) { (void)a; bcst *s = n->st; lt_treebar_draw(s->bar, c, (lt_rect){w.x, w.y, w.w, 1}, t, focused); n->inner = (lt_rect){w.x, w.y, w.w, 1}; }
static lt_wres bc_event(lt_app *a, anode *n, const lt_event *ev) { (void)a; bcst *s = n->st; return lt_treebar_event(s->bar, n->inner, ev); }
static void bc_report(lt_app *a, anode *n, lt_wres r)
{
	bcst *s = n->st;
	const char *bind = app_attr(n, "bind");
	int node = lt_treebar_node(s->bar);
	if (node < 0) return;
	if (bind) lt_app_set(a, bind, node);
	lt_app_emit(a, n->id, r == LT_W_ACTIVATE ? "activate" : "change", lt_tree_label(n->tree, node), node);
}

/* ================================================================== the table */
static const kind_ops OPS[] = {
	{"treeview", true, tv_init, tv_destroy, tv_draw, tv_event, tv_report},
	{"breadcrumbs", true, bc_init, bc_destroy, bc_draw, bc_event, bc_report},
};

const kind_ops *app_kind_ops(const char *name)
{
	for (size_t i = 0; i < sizeof OPS / sizeof OPS[0]; i++) if (!strcmp(OPS[i].name, name)) return &OPS[i];
	return NULL;
}
