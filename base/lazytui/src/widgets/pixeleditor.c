/* pixeleditor.c - a small pixel-art canvas: one cell per pixel, painted with the mouse (pen/eraser/fill/colour-picker) or the keyboard
 * (arrow keys move a cursor, Enter/Space applies the tool). A flood fill walks the scratch arena as its queue, so it costs nothing once
 * the frame is over. Cells are LT_DEFAULT until painted, so an untouched sprite is transparent, not black. */
#include "internal.h"
#include <stdio.h>

struct lt_pixeleditor {
	int cols, rows;
	lt_color *cell;
	lt_color pen;
	lt_pixel_tool tool;
	int cx, cy;                       /* the keyboard cursor */
	bool painting;                    /* a pen/eraser stroke is in progress (mouse button down) */
	bool toolbar;                     /* draw+hit-test a strip of tool buttons alongside the canvas (off by default: existing callers keep their exact rect math) */
	lt_side toolbar_side;              /* LT_SIDE_TOP (default): a row above; BOTTOM: a row below; LEFT/RIGHT: a column beside, like a real toolbox */
	lt_rect tool_rect[4];              /* the last-drawn button rectangles, in tool order (pen/eraser/fill/picker), for hit-testing */
};

lt_pixeleditor *lt_pixeleditor_new(int cols, int rows)
{
	if (cols < 1) cols = 1;
	if (rows < 1) rows = 1;
	lt_pixeleditor *p = calloc(1, sizeof *p);
	p->cols = cols; p->rows = rows;
	p->cell = calloc((size_t)cols * (size_t)rows, sizeof *p->cell);
	p->pen = LT_RGB(255, 255, 255);
	return p;
}
void lt_pixeleditor_free(lt_pixeleditor *p) { if (p) { free(p->cell); free(p); } }
/* back to what lt_pixeleditor_new gives you: every pixel transparent, pen white, tool pen, cursor at the origin - also bound to 'r' */
void lt_pixeleditor_reset(lt_pixeleditor *p)
{
	for (int i = 0; i < p->cols * p->rows; i++) p->cell[i] = LT_DEFAULT;
	p->pen = LT_RGB(255, 255, 255);
	p->tool = LT_PIXEL_PEN;
	p->cx = p->cy = 0;
	p->painting = false;
}

static bool in_bounds(const lt_pixeleditor *p, int x, int y) { return x >= 0 && x < p->cols && y >= 0 && y < p->rows; }
lt_color lt_pixeleditor_get(const lt_pixeleditor *p, int x, int y) { return in_bounds(p, x, y) ? p->cell[y * p->cols + x] : LT_DEFAULT; }
void lt_pixeleditor_set(lt_pixeleditor *p, int x, int y, lt_color c) { if (in_bounds(p, x, y)) p->cell[y * p->cols + x] = c; }
void lt_pixeleditor_clear(lt_pixeleditor *p, lt_color fill) { for (int i = 0; i < p->cols * p->rows; i++) p->cell[i] = fill; }
void lt_pixeleditor_set_color(lt_pixeleditor *p, lt_color c) { p->pen = c; }
lt_color lt_pixeleditor_color(const lt_pixeleditor *p) { return p->pen; }
void lt_pixeleditor_set_tool(lt_pixeleditor *p, lt_pixel_tool tool) { p->tool = tool; }
lt_pixel_tool lt_pixeleditor_tool(const lt_pixeleditor *p) { return p->tool; }
void lt_pixeleditor_set_toolbar(lt_pixeleditor *p, bool on) { p->toolbar = on; }
bool lt_pixeleditor_toolbar(const lt_pixeleditor *p) { return p->toolbar; }
void lt_pixeleditor_set_toolbar_side(lt_pixeleditor *p, lt_side side) { p->toolbar_side = side; }
lt_side lt_pixeleditor_toolbar_side(const lt_pixeleditor *p) { return p->toolbar_side; }
int lt_pixeleditor_cursor_x(const lt_pixeleditor *p) { return p->cx; }
int lt_pixeleditor_cursor_y(const lt_pixeleditor *p) { return p->cy; }
int lt_pixeleditor_cols(const lt_pixeleditor *p) { return p->cols; }
int lt_pixeleditor_rows(const lt_pixeleditor *p) { return p->rows; }

/* 4-connected flood fill from (x,y): every same-coloured pixel reachable without crossing a different colour becomes `to`.
 * The frontier is a queue in the scratch arena - freed the moment the fill is done, nothing to leak. */
static void flood_fill(lt_pixeleditor *p, int x, int y, lt_color to)
{
	if (!in_bounds(p, x, y)) return;
	lt_color from = lt_pixeleditor_get(p, x, y);
	if (from == to) return;
	lt_arena *sc = lt_scratch();
	lt_arena_mark m = lt_arena_save(sc);
	int cap = p->cols * p->rows;
	int (*q)[2] = lt_arena_alloc(sc, (size_t)cap * sizeof *q);
	int head = 0, tail = 0;
	q[tail][0] = x; q[tail][1] = y; tail++;
	lt_pixeleditor_set(p, x, y, to);
	while (head < tail) {
		int cx = q[head][0], cy = q[head][1]; head++;
		static const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
		for (int k = 0; k < 4; k++) {
			int nx = cx + dx[k], ny = cy + dy[k];
			if (!in_bounds(p, nx, ny) || lt_pixeleditor_get(p, nx, ny) != from) continue;
			lt_pixeleditor_set(p, nx, ny, to);
			q[tail][0] = nx; q[tail][1] = ny; tail++;
		}
	}
	lt_arena_restore(sc, m);
}

static void apply_tool(lt_pixeleditor *p, int x, int y)
{
	if (!in_bounds(p, x, y)) return;
	switch (p->tool) {
	case LT_PIXEL_PEN: lt_pixeleditor_set(p, x, y, p->pen); break;
	case LT_PIXEL_ERASER: lt_pixeleditor_set(p, x, y, LT_DEFAULT); break;
	case LT_PIXEL_PICKER: { lt_color c = lt_pixeleditor_get(p, x, y); if (c != LT_DEFAULT) p->pen = c; break; }             /* picking an empty
	                                                                              * cell leaves the pen alone instead of turning it transparent
	                                                                              * (which would silently turn Pen into another Eraser) */
	case LT_PIXEL_FILL: flood_fill(p, x, y, p->pen); break;
	}
}

static const char *const TOOLNAME[4] = {"Pen", "Eraser", "Fill", "Pick"};

/* the toolbar laid out as a row: one "[ Pen ]"-style button per tool, side by side, the active tool in the accent colour */
static void draw_toolbar_row(lt_pixeleditor *p, lt_canvas *c, lt_rect r, const lt_theme *t)
{
	lt_rect all = lt_full(c);
	lt_fill(c, (lt_rect){r.x, r.y, r.w, 1}, ' ', t->s[LT_T_TEXT]);
	int x = r.x;
	for (int i = 0; i < 4; i++) {
		int w = lt_utf8_width(TOOLNAME[i]) + 4;
		p->tool_rect[i] = (lt_rect){x, r.y, x + w <= r.x + r.w ? w : 0, 1};
		if (x + w > r.x + r.w) continue;
		bool active = (int)p->tool == i;
		lt_style st = active ? t->s[LT_T_SELECT] : t->s[LT_T_TEXT];
		lt_fill(c, (lt_rect){x, r.y, w, 1}, ' ', st);
		lt_put(c, all, x, r.y, '[', st); lt_put(c, all, x + w - 1, r.y, ']', st);
		lt_text(c, all, x + 2, r.y, TOOLNAME[i], st, w - 4);
		x += w + 1;
	}
}

/* the toolbar laid out as a column: one button per row, like a real paint-app toolbox docked beside the canvas */
static void draw_toolbar_col(lt_pixeleditor *p, lt_canvas *c, lt_rect r, const lt_theme *t)
{
	lt_rect all = lt_full(c);
	for (int i = 0; i < 4; i++) {
		if (i >= r.h || r.w < 1) { p->tool_rect[i] = (lt_rect){0, 0, 0, 0}; continue; }
		p->tool_rect[i] = (lt_rect){r.x, r.y + i, r.w, 1};
		bool active = (int)p->tool == i;
		lt_style st = active ? t->s[LT_T_SELECT] : t->s[LT_T_TEXT];
		lt_fill(c, (lt_rect){r.x, r.y + i, r.w, 1}, ' ', st);
		if (r.w >= 3) {
			lt_put(c, all, r.x, r.y + i, '[', st); lt_put(c, all, r.x + r.w - 1, r.y + i, ']', st);
			if (r.w > 4) lt_text(c, all, r.x + 1, r.y + i, TOOLNAME[i], st, r.w - 2);
		} else lt_text(c, all, r.x, r.y + i, TOOLNAME[i], st, r.w);
	}
}

/* splits r into the toolbar's own strip and the canvas rect left over, according to p->toolbar_side. Callers check p->toolbar first. */
static void split_toolbar(const lt_pixeleditor *p, lt_rect r, lt_rect *bar, lt_rect *canvas)
{
	int tw = 9;
	switch (p->toolbar_side) {
	case LT_SIDE_LEFT:
		if (tw > r.w - 1) tw = r.w > 1 ? r.w - 1 : 0;
		*bar = (lt_rect){r.x, r.y, tw, r.h};
		*canvas = (lt_rect){r.x + tw + (tw > 0 ? 1 : 0), r.y, r.w - tw - (tw > 0 ? 1 : 0), r.h};
		break;
	case LT_SIDE_RIGHT:
		if (tw > r.w - 1) tw = r.w > 1 ? r.w - 1 : 0;
		*bar = (lt_rect){r.x + r.w - tw, r.y, tw, r.h};
		*canvas = (lt_rect){r.x, r.y, r.w - tw - (tw > 0 ? 1 : 0), r.h};
		break;
	case LT_SIDE_BOTTOM:
		*bar = (lt_rect){r.x, r.y + r.h - 1, r.w, 1};
		*canvas = (lt_rect){r.x, r.y, r.w, r.h - 1};
		break;
	default:                                        /* LT_SIDE_TOP */
		*bar = (lt_rect){r.x, r.y, r.w, 1};
		*canvas = (lt_rect){r.x, r.y + 1, r.w, r.h - 1};
		break;
	}
}

void lt_pixeleditor_draw(lt_pixeleditor *p, lt_canvas *c, lt_rect r, const lt_theme *t, bool focused)
{
	if (r.w < 1 || r.h < 1) return;
	lt_rect all = lt_full(c);
	bool uni = lt_canvas_caps(c)->unicode;
	if (p->toolbar) {
		lt_rect bar, canvas;
		split_toolbar(p, r, &bar, &canvas);
		if (p->toolbar_side == LT_SIDE_LEFT || p->toolbar_side == LT_SIDE_RIGHT) draw_toolbar_col(p, c, bar, t);
		else draw_toolbar_row(p, c, bar, t);
		r = canvas;
		if (r.w < 1 || r.h < 1) return;
	}
	int w = p->cols < r.w ? p->cols : r.w, h = p->rows < r.h ? p->rows : r.h;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			lt_color pix = p->cell[y * p->cols + x];
			bool cursor = focused && x == p->cx && y == p->cy;
			if (pix == LT_DEFAULT) lt_put(c, all, r.x + x, r.y + y, uni ? 0xB7 : '.', cursor ? t->s[LT_T_SELECT] : t->s[LT_T_DIM]);
			else lt_put(c, all, r.x + x, r.y + y, ' ', LT_STYLE(LT_DEFAULT, cursor ? lt_color_mix(pix, LT_RGB(255, 255, 255), 0.35) : pix, 0));
		}
}

static lt_wres pos_apply(lt_pixeleditor *p, lt_rect r, int x, int y)
{
	p->cx = lt_clampi(x - r.x, 0, p->cols - 1); p->cy = lt_clampi(y - r.y, 0, p->rows - 1);
	if (x < r.x || x >= r.x + p->cols || y < r.y || y >= r.y + p->rows) return LT_W_IGNORED;
	apply_tool(p, p->cx, p->cy);
	return LT_W_CHANGED;
}

lt_wres lt_pixeleditor_event(lt_pixeleditor *p, lt_rect r, const lt_event *ev)
{
	int base = ev->key & 0xFFFFFF;
	if (p->toolbar) {
		if (ev->kind == LT_EV_MOUSE && base == LT_K_CLICK)
			for (int i = 0; i < 4; i++) if (p->tool_rect[i].w > 0 && lt_in(p->tool_rect[i], ev->x, ev->y)) { p->tool = (lt_pixel_tool)i; return LT_W_CHANGED; }
		lt_rect bar, canvas;
		split_toolbar(p, r, &bar, &canvas);
		(void)bar;
		r = canvas;
		if (r.w < 1 || r.h < 1) return LT_W_IGNORED;
	}
	if (ev->kind == LT_EV_MOUSE) {
		bool strokes = p->tool == LT_PIXEL_PEN || p->tool == LT_PIXEL_ERASER;                    /* fill/picker are one-shot: a real click
		                                                                                            * always has a little drift in it, and
		                                                                                            * re-firing them on every drag tick meant
		                                                                                            * a fill could jump to a second region, or
		                                                                                            * the picked colour could end up being
		                                                                                            * whatever the pointer drifted onto last */
		if (base == LT_K_CLICK) { p->painting = true; return pos_apply(p, r, ev->x, ev->y); }
		if (base == LT_K_DRAG && p->painting && strokes) return pos_apply(p, r, ev->x, ev->y);
		if (base == LT_K_RELEASE) { bool was = p->painting; p->painting = false; return was ? LT_W_HANDLED : LT_W_IGNORED; }
		return LT_W_IGNORED;
	}
	if (ev->kind != LT_EV_KEY || (ev->key & (LT_MOD_CTRL | LT_MOD_ALT))) return LT_W_IGNORED;
	switch (base) {
	case LT_K_LEFT: p->cx = lt_clampi(p->cx - 1, 0, p->cols - 1); return LT_W_HANDLED;
	case LT_K_RIGHT: p->cx = lt_clampi(p->cx + 1, 0, p->cols - 1); return LT_W_HANDLED;
	case LT_K_UP: p->cy = lt_clampi(p->cy - 1, 0, p->rows - 1); return LT_W_HANDLED;
	case LT_K_DOWN: p->cy = lt_clampi(p->cy + 1, 0, p->rows - 1); return LT_W_HANDLED;
	case LT_K_HOME: p->cx = 0; return LT_W_HANDLED;
	case LT_K_END: p->cx = p->cols - 1; return LT_W_HANDLED;
	case ' ': case LT_K_ENTER: apply_tool(p, p->cx, p->cy); return LT_W_CHANGED;
	case 'r': lt_pixeleditor_reset(p); return LT_W_CHANGED;
	}
	return LT_W_IGNORED;
}
