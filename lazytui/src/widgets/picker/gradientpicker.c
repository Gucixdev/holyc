/* gradientpicker.c - A gradient bar with draggable stops
 * Needs: lazytui.h (the lib core). One widget, one file: copy it into any project that links the core. */
#include "internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
