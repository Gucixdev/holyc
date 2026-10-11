/* nodeeditor.c - a drawio-like canvas of boxes and arrows: nodes are small titled panels at world coordinates, edges are wire lines
 * between their borders. Drag a node to move it, drag empty space to pan, 'c' arms connect mode (click a source then a target), 'd'
 * deletes the selected node and every edge touching it. Nodes/edges keep their id across removal of others (a free-list, not a compaction),
 * so a caller's own references (e.g. a graph model) stay valid. */
#include "internal.h"
#include <stdio.h>

typedef struct { bool alive; char *label; int x, y, w, h; } nenode;
typedef struct { bool alive; int from, to; } needge;

struct lt_nodeeditor {
	nenode *node; int nnode, capnode;
	int *nodefree, nnodefree;
	needge *edge; int nedge, capedge;
	int *edgefree, nedgefree;
	int selected;                                    /* node id, -1 none */
	int ox, oy;                                       /* pan, world cells at the pane's top-left */
	double zoom;                                       /* >= 1 spreads nodes further apart on screen, < 1 draws them closer together; box glyphs keep their own size (text cannot sub-scale) */
	int drag_node;                                    /* node id being dragged, -1 none */
	int drag_dx, drag_dy;                             /* mouse - the node's screen origin at the moment the drag started */
	bool panning; int pan_start_x, pan_start_y, pan_start_ox, pan_start_oy;
	bool connecting; int connect_from;
};

static int w2sx(const lt_nodeeditor *ne, lt_rect r, int wx) { return r.x + (int)floor((wx - ne->ox) * ne->zoom + 0.5); }
static int w2sy(const lt_nodeeditor *ne, lt_rect r, int wy) { return r.y + (int)floor((wy - ne->oy) * ne->zoom + 0.5); }
static int s2wx(const lt_nodeeditor *ne, lt_rect r, int sx) { return ne->ox + (int)floor((sx - r.x) / ne->zoom + 0.5); }
static int s2wy(const lt_nodeeditor *ne, lt_rect r, int sy) { return ne->oy + (int)floor((sy - r.y) / ne->zoom + 0.5); }
static void clamp_zoom(lt_nodeeditor *ne) { if (!(ne->zoom >= 0.25)) ne->zoom = 0.25; if (ne->zoom > 4) ne->zoom = 4; }

lt_nodeeditor *lt_nodeeditor_new(void) { lt_nodeeditor *ne = calloc(1, sizeof *ne); ne->selected = -1; ne->drag_node = -1; ne->connect_from = -1; ne->zoom = 1.0; return ne; }
void lt_nodeeditor_free(lt_nodeeditor *ne)
{
	if (!ne) return;
	for (int i = 0; i < ne->nnode; i++) free(ne->node[i].label);
	free(ne->node); free(ne->nodefree); free(ne->edge); free(ne->edgefree);
	free(ne);
}

static int node_w(const char *label) { int w = lt_utf8_width(label) + 6; return w < 10 ? 10 : w; }

int lt_nodeeditor_add(lt_nodeeditor *ne, const char *label, int x, int y)
{
	int id;
	if (ne->nnodefree) { id = ne->nodefree[--ne->nnodefree]; }
	else {
		if (ne->nnode == ne->capnode) { ne->capnode = ne->capnode ? ne->capnode * 2 : 8; ne->node = realloc(ne->node, (size_t)ne->capnode * sizeof *ne->node); }
		id = ne->nnode++;
	}
	ne->node[id] = (nenode){true, strdup(label ? label : ""), x, y, node_w(label ? label : ""), 3};
	return id;
}
static bool node_ok(const lt_nodeeditor *ne, int id) { return id >= 0 && id < ne->nnode && ne->node[id].alive; }
static bool edge_ok(const lt_nodeeditor *ne, int id) { return id >= 0 && id < ne->nedge && ne->edge[id].alive; }

void lt_nodeeditor_remove(lt_nodeeditor *ne, int id)
{
	if (!node_ok(ne, id)) return;
	for (int i = 0; i < ne->nedge; i++) if (ne->edge[i].alive && (ne->edge[i].from == id || ne->edge[i].to == id)) lt_nodeeditor_disconnect(ne, i);
	free(ne->node[id].label); ne->node[id].label = NULL;
	ne->node[id].alive = false;
	ne->nodefree = realloc(ne->nodefree, (size_t)(ne->nnodefree + 1) * sizeof *ne->nodefree);
	ne->nodefree[ne->nnodefree++] = id;
	if (ne->selected == id) ne->selected = -1;
	if (ne->connect_from == id) { ne->connect_from = -1; ne->connecting = false; }
}
void lt_nodeeditor_move(lt_nodeeditor *ne, int id, int x, int y) { if (node_ok(ne, id)) { ne->node[id].x = x; ne->node[id].y = y; } }
void lt_nodeeditor_get_pos(const lt_nodeeditor *ne, int id, int *x, int *y) { if (!node_ok(ne, id)) return; if (x) *x = ne->node[id].x; if (y) *y = ne->node[id].y; }
void lt_nodeeditor_set_label(lt_nodeeditor *ne, int id, const char *label)
{
	if (!node_ok(ne, id)) return;
	free(ne->node[id].label); ne->node[id].label = strdup(label ? label : "");
	ne->node[id].w = node_w(ne->node[id].label);
}
const char *lt_nodeeditor_label(const lt_nodeeditor *ne, int id) { return node_ok(ne, id) ? ne->node[id].label : ""; }
int lt_nodeeditor_selected(const lt_nodeeditor *ne) { return ne->selected; }
void lt_nodeeditor_select(lt_nodeeditor *ne, int id) { ne->selected = node_ok(ne, id) ? id : -1; }

int lt_nodeeditor_node_count(const lt_nodeeditor *ne) { int n = 0; for (int i = 0; i < ne->nnode; i++) if (ne->node[i].alive) n++; return n; }
int lt_nodeeditor_node_id(const lt_nodeeditor *ne, int i) { for (int k = 0; k < ne->nnode; k++) if (ne->node[k].alive && i-- == 0) return k; return -1; }

int lt_nodeeditor_connect(lt_nodeeditor *ne, int from, int to)
{
	if (!node_ok(ne, from) || !node_ok(ne, to) || from == to) return -1;
	int id;
	if (ne->nedgefree) id = ne->edgefree[--ne->nedgefree];
	else {
		if (ne->nedge == ne->capedge) { ne->capedge = ne->capedge ? ne->capedge * 2 : 8; ne->edge = realloc(ne->edge, (size_t)ne->capedge * sizeof *ne->edge); }
		id = ne->nedge++;
	}
	ne->edge[id] = (needge){true, from, to};
	return id;
}
void lt_nodeeditor_disconnect(lt_nodeeditor *ne, int id)
{
	if (!edge_ok(ne, id)) return;
	ne->edge[id].alive = false;
	ne->edgefree = realloc(ne->edgefree, (size_t)(ne->nedgefree + 1) * sizeof *ne->edgefree);
	ne->edgefree[ne->nedgefree++] = id;
}
int lt_nodeeditor_edge_count(const lt_nodeeditor *ne) { int n = 0; for (int i = 0; i < ne->nedge; i++) if (ne->edge[i].alive) n++; return n; }
bool lt_nodeeditor_edge_at(const lt_nodeeditor *ne, int i, int *from, int *to)
{
	for (int k = 0; k < ne->nedge; k++)
		if (ne->edge[k].alive && i-- == 0) { if (from) *from = ne->edge[k].from; if (to) *to = ne->edge[k].to; return true; }
	return false;
}

static lt_rect screen_rect(const lt_nodeeditor *ne, lt_rect r, int id) { const nenode *n = &ne->node[id]; return (lt_rect){w2sx(ne, r, n->x), w2sy(ne, r, n->y), n->w, n->h}; }

int lt_nodeeditor_node_at(const lt_nodeeditor *ne, lt_rect r, int x, int y)
{
	for (int i = ne->nnode - 1; i >= 0; i--) {
		if (!ne->node[i].alive) continue;
		if (lt_in(screen_rect(ne, r, i), x, y)) return i;
	}
	return -1;
}

void lt_nodeeditor_pan(lt_nodeeditor *ne, int ox, int oy) { ne->ox = ox; ne->oy = oy; }
void lt_nodeeditor_get_pan(const lt_nodeeditor *ne, int *ox, int *oy) { if (ox) *ox = ne->ox; if (oy) *oy = ne->oy; }
bool lt_nodeeditor_connecting(const lt_nodeeditor *ne) { return ne->connecting; }
double lt_nodeeditor_zoom(const lt_nodeeditor *ne) { return ne->zoom; }
void lt_nodeeditor_set_zoom(lt_nodeeditor *ne, double zoom) { if (isfinite(zoom)) ne->zoom = zoom; clamp_zoom(ne); }

/* zoom by `factor`, keeping the world point under screen cell (x,y) stationary - the same trick lt_mapview uses. */
void lt_nodeeditor_zoom_at(lt_nodeeditor *ne, lt_rect r, double factor, int x, int y)
{
	int wx = s2wx(ne, r, x), wy = s2wy(ne, r, y);
	ne->zoom *= factor;
	clamp_zoom(ne);
	ne->ox += wx - s2wx(ne, r, x);
	ne->oy += wy - s2wy(ne, r, y);
}

void lt_nodeeditor_draw(lt_nodeeditor *ne, lt_canvas *c, lt_rect r, const lt_theme *t, bool focused)
{
	if (r.w < 1 || r.h < 1) return;
	lt_canvas_set_clip(c, r);
	lt_color line = t->s[LT_T_DIM].fg != LT_DEFAULT ? t->s[LT_T_DIM].fg : LT_RGB(120, 120, 120);
	for (int i = 0; i < ne->nedge; i++) {
		if (!ne->edge[i].alive || !ne->node[ne->edge[i].from].alive || !ne->node[ne->edge[i].to].alive) continue;
		lt_rect a = screen_rect(ne, r, ne->edge[i].from), b = screen_rect(ne, r, ne->edge[i].to);
		double ax = a.x + a.w / 2.0, ay = a.y + a.h / 2.0, bx = b.x + b.w / 2.0, by = b.y + b.h / 2.0;
		bool right = bx >= ax;
		double sx = right ? a.x + a.w : a.x, ex = right ? b.x : b.x + b.w;
		lt_wire_line(c, sx, ay, ex, by, line);
		double hx = ex + (right ? -0.6 : 0.6);                                /* a small arrowhead at the target */
		lt_wire_line(c, ex, by, hx, by - 0.4, line);
		lt_wire_line(c, ex, by, hx, by + 0.4, line);
	}
	for (int i = 0; i < ne->nnode; i++) {
		if (!ne->node[i].alive) continue;
		lt_rect nr = screen_rect(ne, r, i);
		if (!(nr.x + nr.w > r.x && nr.x < r.x + r.w && nr.y + nr.h > r.y && nr.y < r.y + r.h)) continue;
		lt_theme nt = *t;
		bool sel = i == ne->selected;
		bool arming = ne->connecting && ne->connect_from == i;
		if (sel || arming) nt.s[LT_T_BORDER] = t->s[arming ? LT_T_WARN : LT_T_ACCENT];
		lt_panel_ex(c, nr, &nt, ne->node[i].label, LT_PANEL_ROUND);   /* HUD needs more chrome width than node_w() reserves; the accent border already marks the selection */
	}
	lt_canvas_reset_clip(c);
	(void)focused;
}

static lt_wres begin_press(lt_nodeeditor *ne, lt_rect r, int x, int y)
{
	int hit = lt_nodeeditor_node_at(ne, r, x, y);
	if (ne->connecting) {
		if (hit < 0) return LT_W_HANDLED;
		if (ne->connect_from < 0) { ne->connect_from = hit; return LT_W_HANDLED; }
		int from = ne->connect_from;
		ne->connecting = false; ne->connect_from = -1;
		if (hit == from) return LT_W_HANDLED;
		return lt_nodeeditor_connect(ne, from, hit) >= 0 ? LT_W_ACTIVATE : LT_W_HANDLED;
	}
	if (hit >= 0) {
		ne->selected = hit;
		ne->drag_node = hit;
		ne->drag_dx = x - w2sx(ne, r, ne->node[hit].x);
		ne->drag_dy = y - w2sy(ne, r, ne->node[hit].y);
		return LT_W_CHANGED;
	}
	ne->selected = -1;
	ne->panning = true; ne->pan_start_x = x; ne->pan_start_y = y; ne->pan_start_ox = ne->ox; ne->pan_start_oy = ne->oy;
	return LT_W_CHANGED;
}

lt_wres lt_nodeeditor_event(lt_nodeeditor *ne, lt_rect r, const lt_event *ev)
{
	int base = ev->key & 0xFFFFFF;
	if (ev->kind == LT_EV_MOUSE) {
		if (base == LT_K_CLICK) return begin_press(ne, r, ev->x, ev->y);
		if (base == LT_K_DRAG && ne->drag_node >= 0 && node_ok(ne, ne->drag_node)) {
			lt_nodeeditor_move(ne, ne->drag_node, s2wx(ne, r, ev->x - ne->drag_dx), s2wy(ne, r, ev->y - ne->drag_dy));
			return LT_W_CHANGED;
		}
		if (base == LT_K_DRAG && ne->panning) { ne->ox = ne->pan_start_ox - (int)floor((ev->x - ne->pan_start_x) / ne->zoom + 0.5); ne->oy = ne->pan_start_oy - (int)floor((ev->y - ne->pan_start_y) / ne->zoom + 0.5); return LT_W_CHANGED; }
		if (base == LT_K_RELEASE) { bool was = ne->drag_node >= 0 || ne->panning; ne->drag_node = -1; ne->panning = false; return was ? LT_W_HANDLED : LT_W_IGNORED; }
		if ((base == LT_K_WHEEL_UP || base == LT_K_WHEEL_DOWN) && lt_in(r, ev->x, ev->y)) { lt_nodeeditor_zoom_at(ne, r, base == LT_K_WHEEL_UP ? 1.15 : 1 / 1.15, ev->x, ev->y); return LT_W_CHANGED; }
		return LT_W_IGNORED;
	}
	if (ev->kind != LT_EV_KEY || (ev->key & (LT_MOD_CTRL | LT_MOD_ALT))) return LT_W_IGNORED;
	switch (base) {
	case 'c': ne->connecting = !ne->connecting; ne->connect_from = -1; return LT_W_HANDLED;
	case LT_K_ESC: if (ne->connecting) { ne->connecting = false; ne->connect_from = -1; return LT_W_HANDLED; } return LT_W_IGNORED;
	case 'd': if (ne->selected < 0) return LT_W_IGNORED; lt_nodeeditor_remove(ne, ne->selected); return LT_W_CHANGED;
	case LT_K_LEFT: ne->ox -= (int)fmax(1, 1 / ne->zoom); return LT_W_HANDLED;
	case LT_K_RIGHT: ne->ox += (int)fmax(1, 1 / ne->zoom); return LT_W_HANDLED;
	case LT_K_UP: ne->oy -= (int)fmax(1, 1 / ne->zoom); return LT_W_HANDLED;
	case LT_K_DOWN: ne->oy += (int)fmax(1, 1 / ne->zoom); return LT_W_HANDLED;
	case '+': case '=': lt_nodeeditor_zoom_at(ne, r, 1.15, r.x + r.w / 2, r.y + r.h / 2); return LT_W_CHANGED;
	case '-': case '_': lt_nodeeditor_zoom_at(ne, r, 1 / 1.15, r.x + r.w / 2, r.y + r.h / 2); return LT_W_CHANGED;
	case '0': ne->zoom = 1; ne->ox = 0; ne->oy = 0; return LT_W_CHANGED;
	}
	return LT_W_IGNORED;
}
