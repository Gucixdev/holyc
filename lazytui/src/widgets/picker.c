/* picker.c - two widgets for picking a colour, in one file: a gradient bar with draggable stops (lt_gradientpicker - owns positions, not
 * colour entry) and an HSV colour picker (lt_colorpicker - a shaded saturation/value plane + hue bar + hex readout) that naturally feeds
 * a stop's colour via lt_gradientpicker_set_color. */
#include "internal.h"
#include <stdio.h>


/* ================================================================== a gradient bar with draggable stops */
typedef struct { double pos; lt_color color; } stop;
struct lt_gradientpicker {
	stop *s; int n, cap;
	int sel;
	bool dragging;
	lt_rect bar;                      /* the gradient row, as last drawn */
};

static int cmp_stop(const void *a, const void *b) { double d = ((const stop *)a)->pos - ((const stop *)b)->pos; return d < 0 ? -1 : d > 0 ? 1 : 0; }
static void resort(lt_gradientpicker *g, int keep_pos_of_sel)
{
	lt_color selc = g->sel >= 0 ? g->s[g->sel].color : 0;
	double selp = g->sel >= 0 ? g->s[g->sel].pos : 0;
	(void)keep_pos_of_sel;
	qsort(g->s, (size_t)g->n, sizeof *g->s, cmp_stop);
	if (g->sel >= 0) for (int i = 0; i < g->n; i++) if (g->s[i].pos == selp && g->s[i].color == selc) { g->sel = i; break; }
}

/* the two-stop black -> white gradient lt_gradientpicker_new starts with, and what 'r'/lt_gradientpicker_reset restores */
void lt_gradientpicker_reset(lt_gradientpicker *g)
{
	if (g->cap < 2) { g->cap = 2; g->s = realloc(g->s, (size_t)g->cap * sizeof *g->s); }
	g->s[0] = (stop){0.0, LT_RGB(0, 0, 0)}; g->s[1] = (stop){1.0, LT_RGB(255, 255, 255)};
	g->n = 2; g->sel = 0; g->dragging = false;
}

lt_gradientpicker *lt_gradientpicker_new(void)
{
	lt_gradientpicker *g = calloc(1, sizeof *g);
	g->cap = 4; g->s = calloc((size_t)g->cap, sizeof *g->s);
	lt_gradientpicker_reset(g);
	return g;
}
void lt_gradientpicker_free(lt_gradientpicker *g) { if (g) { free(g->s); free(g); } }

void lt_gradientpicker_set_stops(lt_gradientpicker *g, const lt_gradstop *stops, int n)
{
	if (n < 2 || !stops) return;
	if (n > g->cap) { g->cap = n; g->s = realloc(g->s, (size_t)g->cap * sizeof *g->s); }
	for (int i = 0; i < n; i++) g->s[i] = (stop){stops[i].pos < 0 ? 0 : stops[i].pos > 1 ? 1 : stops[i].pos, stops[i].color};
	g->n = n;
	g->sel = 0;
	resort(g, 0);
}
int lt_gradientpicker_count(const lt_gradientpicker *g) { return g->n; }
lt_gradstop lt_gradientpicker_stop(const lt_gradientpicker *g, int i) { if (i < 0 || i >= g->n) return (lt_gradstop){0, LT_DEFAULT}; return (lt_gradstop){g->s[i].pos, g->s[i].color}; }
int lt_gradientpicker_selected(const lt_gradientpicker *g) { return g->sel; }
void lt_gradientpicker_select(lt_gradientpicker *g, int i) { if (i >= 0 && i < g->n) g->sel = i; }

void lt_gradientpicker_set_color(lt_gradientpicker *g, lt_color c) { if (g->sel >= 0) g->s[g->sel].color = c; }

lt_color lt_gradientpicker_sample(const lt_gradientpicker *g, double f)
{
	if (g->n == 0) return LT_DEFAULT;
	if (!(f > 0)) f = 0;
	if (f > 1) f = 1;
	if (f <= g->s[0].pos) return g->s[0].color;
	if (f >= g->s[g->n - 1].pos) return g->s[g->n - 1].color;
	for (int i = 0; i < g->n - 1; i++) {
		if (f < g->s[i].pos || f > g->s[i + 1].pos) continue;
		double span = g->s[i + 1].pos - g->s[i].pos;
		double t = span > 0 ? (f - g->s[i].pos) / span : 0;
		return lt_color_mix(g->s[i].color, g->s[i + 1].color, t);
	}
	return g->s[g->n - 1].color;
}
void lt_gradientpicker_to_theme_grad(const lt_gradientpicker *g, lt_color *out, int n)
{
	for (int i = 0; i < n; i++) out[i] = lt_gradientpicker_sample(g, n > 1 ? (double)i / (n - 1) : 0);
}

int lt_gradientpicker_add_stop(lt_gradientpicker *g, double pos, lt_color c)
{
	if (pos < 0) pos = 0;
	if (pos > 1) pos = 1;
	if (g->n == g->cap) { g->cap *= 2; g->s = realloc(g->s, (size_t)g->cap * sizeof *g->s); }
	g->s[g->n] = (stop){pos, c};
	g->n++;
	resort(g, 0);
	for (int i = 0; i < g->n; i++) if (g->s[i].pos == pos && g->s[i].color == c) return i;
	return g->n - 1;
}
void lt_gradientpicker_remove_stop(lt_gradientpicker *g, int i)
{
	if (g->n <= 2 || i < 0 || i >= g->n) return;
	memmove(&g->s[i], &g->s[i + 1], (size_t)(g->n - i - 1) * sizeof *g->s);
	g->n--;
	if (g->sel >= g->n) g->sel = g->n - 1;
}

void lt_gradientpicker_draw(lt_gradientpicker *g, lt_canvas *c, lt_rect r, const lt_theme *t, bool focused)
{
	if (r.w < 3 || r.h < 1) return;
	lt_rect all = lt_full(c);
	bool uni = lt_canvas_caps(c)->unicode;
	lt_rect bar = {r.x, r.y, r.w, 1};
	g->bar = bar;
	for (int x = 0; x < bar.w; x++) {
		double f = bar.w > 1 ? (double)x / (bar.w - 1) : 0;
		lt_put(c, all, bar.x + x, bar.y, ' ', LT_STYLE(LT_DEFAULT, lt_gradientpicker_sample(g, f), 0));
	}
	if (r.h < 2) return;
	int mrow = r.y + 1;
	lt_fill(c, (lt_rect){r.x, mrow, r.w, 1}, ' ', t->s[LT_T_TEXT]);
	for (int i = 0; i < g->n; i++) {
		int x = bar.x + (int)floor(g->s[i].pos * (bar.w - 1) + 0.5);
		bool sel = i == g->sel;
		lt_style st = sel ? (focused ? t->s[LT_T_SELECT] : t->s[LT_T_ACCENT]) : t->s[LT_T_DIM];
		lt_put(c, all, x, mrow, uni ? 0x25C6 : (sel ? '#' : '^'), st);
	}
	if (r.h >= 3 && g->sel >= 0) {
		char lbl[48];
		int rr = 0, gg = 0, bb = 0;
		lt_color_rgb(g->s[g->sel].color, &rr, &gg, &bb);
		snprintf(lbl, sizeof lbl, "stop %d/%d  pos %.2f  #%02x%02x%02x", g->sel + 1, g->n, g->s[g->sel].pos, rr, gg, bb);
		lt_text(c, all, r.x, r.y + 2, lbl, t->s[LT_T_DIM], r.w);
	}
}

static int nearest_stop(const lt_gradientpicker *g, int x)
{
	if (g->bar.w < 2) return g->n ? 0 : -1;
	double f = (double)(x - g->bar.x) / (g->bar.w - 1);
	int best = -1; double bd = 1e9;
	for (int i = 0; i < g->n; i++) { double d = fabs(g->s[i].pos - f); if (d < bd) { bd = d; best = i; } }
	return bd * (g->bar.w - 1) <= 1.5 ? best : -1;                            /* within ~1.5 cells */
}

lt_wres lt_gradientpicker_event(lt_gradientpicker *g, lt_rect r, const lt_event *ev)
{
	int base = ev->key & 0xFFFFFF;
	lt_rect click_area = {r.x, r.y, r.w, r.h >= 2 ? 2 : 1};
	if (ev->kind == LT_EV_MOUSE) {
		if (base == LT_K_CLICK && lt_in(click_area, ev->x, ev->y)) {
			int near = nearest_stop(g, ev->x);
			if (near >= 0) { g->sel = near; g->dragging = true; return LT_W_CHANGED; }
			double f = g->bar.w > 1 ? (double)(ev->x - g->bar.x) / (g->bar.w - 1) : 0;
			int id = lt_gradientpicker_add_stop(g, f, lt_gradientpicker_sample(g, f));
			g->sel = id; g->dragging = true;
			return LT_W_CHANGED;
		}
		if (base == LT_K_DRAG && g->dragging && g->sel >= 0) {
			double f = g->bar.w > 1 ? (double)(ev->x - g->bar.x) / (g->bar.w - 1) : 0;
			g->s[g->sel].pos = f < 0 ? 0 : f > 1 ? 1 : f;
			resort(g, 0);
			return LT_W_CHANGED;
		}
		if (base == LT_K_RELEASE) { bool was = g->dragging; g->dragging = false; return was ? LT_W_HANDLED : LT_W_IGNORED; }
		return LT_W_IGNORED;
	}
	if (ev->kind != LT_EV_KEY || (ev->key & (LT_MOD_CTRL | LT_MOD_ALT))) return LT_W_IGNORED;
	if (g->sel < 0 && g->n > 0) g->sel = 0;
	switch (base) {
	case LT_K_LEFT: if (g->sel < 0) return LT_W_IGNORED; g->s[g->sel].pos = fmax(0, g->s[g->sel].pos - 0.02); resort(g, 0); return LT_W_CHANGED;
	case LT_K_RIGHT: if (g->sel < 0) return LT_W_IGNORED; g->s[g->sel].pos = fmin(1, g->s[g->sel].pos + 0.02); resort(g, 0); return LT_W_CHANGED;
	case LT_K_TAB: g->sel = g->n ? (g->sel + 1) % g->n : -1; return LT_W_HANDLED;
	case LT_K_DEL: case LT_K_BSPACE: if (g->sel < 0 || g->n <= 2) return LT_W_IGNORED; lt_gradientpicker_remove_stop(g, g->sel); return LT_W_CHANGED;
	case 'r': lt_gradientpicker_reset(g); return LT_W_CHANGED;
	}
	return LT_W_IGNORED;
}

/* ================================================================== an HSV colour picker (plane + hue bar + swatch/hex readout) */
struct lt_colorpicker {
	double h, s, v;                   /* h: 0..360, s/v: 0..1 */
	lt_color initial;                  /* what 'r'/lt_colorpicker_reset restores */
	lt_rect plane, huebar;             /* the last-drawn rectangles, for hit-testing */
	bool dragging_plane, dragging_hue;
};

static void hsv_to_rgb(double h, double s, double v, double *r, double *g, double *b)
{
	h = fmod(h, 360.0);
	if (h < 0) h += 360.0;
	double c = v * s, x = c * (1 - fabs(fmod(h / 60.0, 2) - 1)), m = v - c;
	double rp, gp, bp;
	int i = (int)(h / 60.0);
	switch (i) {
	case 0: rp = c; gp = x; bp = 0; break;
	case 1: rp = x; gp = c; bp = 0; break;
	case 2: rp = 0; gp = c; bp = x; break;
	case 3: rp = 0; gp = x; bp = c; break;
	case 4: rp = x; gp = 0; bp = c; break;
	default: rp = c; gp = 0; bp = x; break;
	}
	*r = rp + m; *g = gp + m; *b = bp + m;
}

static void rgb_to_hsv(double r, double g, double b, double *h, double *s, double *v)
{
	double mx = fmax(r, fmax(g, b)), mn = fmin(r, fmin(g, b)), d = mx - mn;
	*v = mx;
	*s = mx > 0 ? d / mx : 0;
	if (d <= 0) { *h = 0; return; }
	double hh;
	if (mx == r) hh = fmod((g - b) / d, 6.0);
	else if (mx == g) hh = (b - r) / d + 2.0;
	else hh = (r - g) / d + 4.0;
	hh *= 60.0;
	*h = hh < 0 ? hh + 360.0 : hh;
}

lt_colorpicker *lt_colorpicker_new(lt_color initial)
{
	lt_colorpicker *p = calloc(1, sizeof *p);
	p->s = 1; p->v = 1;
	p->initial = initial;
	lt_colorpicker_set_color(p, initial);
	return p;
}
void lt_colorpicker_free(lt_colorpicker *p) { free(p); }
void lt_colorpicker_reset(lt_colorpicker *p) { lt_colorpicker_set_color(p, p->initial); }

lt_color lt_colorpicker_color(const lt_colorpicker *p)
{
	double r, g, b;
	hsv_to_rgb(p->h, p->s, p->v, &r, &g, &b);
	return LT_RGB((int)(r * 255 + 0.5), (int)(g * 255 + 0.5), (int)(b * 255 + 0.5));
}
void lt_colorpicker_set_color(lt_colorpicker *p, lt_color c)
{
	int r, g, b;
	if (!lt_color_rgb(c, &r, &g, &b)) return;
	rgb_to_hsv(r / 255.0, g / 255.0, b / 255.0, &p->h, &p->s, &p->v);
}
void lt_colorpicker_get_hsv(const lt_colorpicker *p, double *h, double *s, double *v) { if (h) *h = p->h; if (s) *s = p->s; if (v) *v = p->v; }
void lt_colorpicker_set_hsv(lt_colorpicker *p, double h, double s, double v)
{
	if (isfinite(h)) { p->h = fmod(h, 360.0); if (p->h < 0) p->h += 360.0; }
	if (isfinite(s)) p->s = s < 0 ? 0 : s > 1 ? 1 : s;
	if (isfinite(v)) p->v = v < 0 ? 0 : v > 1 ? 1 : v;
}

/* white or black text/marker, whichever reads on an RGB triple already in 0..1 */
static lt_color ink_for01(double r, double g, double b) { return (0.299 * r + 0.587 * g + 0.114 * b) > 0.5 ? LT_RGB(0, 0, 0) : LT_RGB(255, 255, 255); }

typedef struct { double h; lt_rect plane; } planectx;
static double plane_shader(double x, double y, double rgb[3], void *ctx)
{
	const planectx *pc = ctx;
	double s = pc->plane.w > 1 ? x / (pc->plane.w - 1) : 0, v = pc->plane.h > 0 ? 1.0 - y / (2.0 * pc->plane.h) : 1;
	s = s < 0 ? 0 : s > 1 ? 1 : s;
	v = v < 0 ? 0 : v > 1 ? 1 : v;
	double r, g, b;
	hsv_to_rgb(pc->h, s, v, &r, &g, &b);
	rgb[0] = r * 255; rgb[1] = g * 255; rgb[2] = b * 255;
	return 1;
}

void lt_colorpicker_draw(lt_colorpicker *p, lt_canvas *c, lt_rect r, const lt_theme *t, bool focused)
{
	if (r.w < 10 || r.h < 4) return;
	lt_rect all = lt_full(c);
	bool uni = lt_canvas_caps(c)->unicode;
	int hue_w = 3;
	p->plane = (lt_rect){r.x, r.y, r.w - hue_w - 1, r.h - 2};
	p->huebar = (lt_rect){r.x + p->plane.w + 1, r.y, hue_w, p->plane.h};
	planectx pc = {p->h, p->plane};
	lt_shade(c, p->plane, plane_shader, &pc);
	for (int y = 0; y < p->huebar.h; y++) {
		double hue = p->huebar.h > 1 ? (double)y / (p->huebar.h - 1) * 360.0 : 0, r1, g1, b1;
		hsv_to_rgb(hue, 1, 1, &r1, &g1, &b1);
		lt_fill(c, (lt_rect){p->huebar.x, p->huebar.y + y, p->huebar.w, 1}, ' ', LT_STYLE(LT_DEFAULT, LT_RGB((int)(r1 * 255), (int)(g1 * 255), (int)(b1 * 255)), 0));
	}
	int hy = p->huebar.h > 1 ? (int)floor(p->h / 360.0 * (p->huebar.h - 1) + 0.5) : 0;
	lt_put(c, all, p->huebar.x, p->huebar.y + hy, uni ? 0x25C4 : '<', LT_STYLE(focused ? t->s[LT_T_SELECT].fg : LT_RGB(255, 255, 255), LT_DEFAULT, LT_BOLD));
	lt_put(c, all, p->huebar.x + p->huebar.w - 1, p->huebar.y + hy, uni ? 0x25BA : '>', LT_STYLE(focused ? t->s[LT_T_SELECT].fg : LT_RGB(255, 255, 255), LT_DEFAULT, LT_BOLD));
	double mr, mg, mb;
	hsv_to_rgb(p->h, p->s, p->v, &mr, &mg, &mb);
	int mx = p->plane.x + (int)floor(p->s * (p->plane.w > 1 ? p->plane.w - 1 : 0) + 0.5), my = p->plane.y + (int)floor((1 - p->v) * (p->plane.h > 0 ? p->plane.h - 1 : 0) + 0.5);
	lt_put(c, all, mx, my, uni ? 0x25CE : 'o', LT_STYLE(ink_for01(mr, mg, mb), LT_RGB((int)(mr * 255), (int)(mg * 255), (int)(mb * 255)), LT_BOLD));
	lt_color cur = lt_colorpicker_color(p);
	int cr, cg, cb;
	lt_color_rgb(cur, &cr, &cg, &cb);
	char hex[32];
	snprintf(hex, sizeof hex, " #%02x%02x%02x  r:%3d g:%3d b:%3d", cr, cg, cb, cr, cg, cb);
	lt_fill(c, (lt_rect){r.x, r.y + r.h - 1, r.w, 1}, ' ', t->s[LT_T_TEXT]);
	lt_fill(c, (lt_rect){r.x, r.y + r.h - 1, 3, 1}, ' ', LT_STYLE(LT_DEFAULT, cur, 0));
	lt_text(c, all, r.x + 3, r.y + r.h - 1, hex, t->s[LT_T_TEXT], r.w - 3);
}

lt_wres lt_colorpicker_event(lt_colorpicker *p, lt_rect r, const lt_event *ev)
{
	int base = ev->key & 0xFFFFFF;
	if (ev->kind == LT_EV_MOUSE) {
		if (base == LT_K_CLICK) {
			if (lt_in(p->plane, ev->x, ev->y)) { p->dragging_plane = true; }
			else if (lt_in(p->huebar, ev->x, ev->y)) { p->dragging_hue = true; }
			else return LT_W_IGNORED;
		} else if (base == LT_K_RELEASE) { bool was = p->dragging_plane || p->dragging_hue; p->dragging_plane = p->dragging_hue = false; return was ? LT_W_HANDLED : LT_W_IGNORED; }
		else if (base != LT_K_DRAG) return LT_W_IGNORED;
		if (p->dragging_plane && p->plane.w > 0 && p->plane.h > 0) {
			double s = (double)(ev->x - p->plane.x) / (p->plane.w > 1 ? p->plane.w - 1 : 1), v = 1.0 - (double)(ev->y - p->plane.y) / (p->plane.h > 1 ? p->plane.h - 1 : 1);
			lt_colorpicker_set_hsv(p, p->h, s, v);
			return LT_W_CHANGED;
		}
		if (p->dragging_hue && p->huebar.h > 0) {
			double h = (double)(ev->y - p->huebar.y) / (p->huebar.h > 1 ? p->huebar.h - 1 : 1) * 360.0;
			lt_colorpicker_set_hsv(p, h, p->s, p->v);
			return LT_W_CHANGED;
		}
		return LT_W_HANDLED;
	}
	if (ev->kind != LT_EV_KEY || (ev->key & (LT_MOD_CTRL | LT_MOD_ALT))) return LT_W_IGNORED;
	switch (base) {
	case LT_K_LEFT: lt_colorpicker_set_hsv(p, p->h, p->s - 0.02, p->v); return LT_W_CHANGED;
	case LT_K_RIGHT: lt_colorpicker_set_hsv(p, p->h, p->s + 0.02, p->v); return LT_W_CHANGED;
	case LT_K_UP: lt_colorpicker_set_hsv(p, p->h, p->s, p->v + 0.02); return LT_W_CHANGED;
	case LT_K_DOWN: lt_colorpicker_set_hsv(p, p->h, p->s, p->v - 0.02); return LT_W_CHANGED;
	case '[': lt_colorpicker_set_hsv(p, p->h - 5, p->s, p->v); return LT_W_CHANGED;
	case ']': lt_colorpicker_set_hsv(p, p->h + 5, p->s, p->v); return LT_W_CHANGED;
	case 'r': lt_colorpicker_reset(p); return LT_W_CHANGED;
	}
	return LT_W_IGNORED;
}
