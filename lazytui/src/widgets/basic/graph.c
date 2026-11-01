/* graph.c - lt_sparkline (one row) and lt_graph (many rows, eighth-block resolution); a graph of height 1 is just a sparkline.
 * Colour follows the theme's gradient when it has one, else fades from the slot's colour towards the accent. */
#include "internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static lt_color level_color(const lt_theme *t, lt_slot slot, double f)          /* f 0..1: the slot colour towards the accent */
{
	if (t->ngrad > 0) return lt_theme_ramp(t, f);
	lt_color a = lt_slot_style(t, slot).fg, b = lt_slot_style(t, LT_T_ACCENT).fg;
	return (a == LT_DEFAULT || b == LT_DEFAULT) ? a : lt_color_mix(a, b, lt_clamp01(f) * 0.7);
}
static double norm(double v, double lo, double hi) { return hi > lo ? lt_clamp01((v - lo) / (hi - lo)) : 0; }

void lt_sparkline(lt_canvas *c, lt_rect r, const lt_theme *t, const double *v, int n, double lo, double hi, lt_slot slot)
{
	lt_rect all = lt_full(c);
	r = lt_one_row(r);
	if (r.w < 1 || n <= 0 || !v) return;
	static const uint32_t blocks[8] = {0x2581, 0x2582, 0x2583, 0x2584, 0x2585, 0x2586, 0x2587, 0x2588};
	static const char ascii[8] = {'_', '.', ':', '-', '=', '+', '*', '#'};
	bool uni = lt_canvas_caps(c)->unicode;
	int first = n > r.w ? n - r.w : 0;                        /* once full, the oldest samples scroll off to the left */
	for (int i = first; i < n; i++) {
		if (!isfinite(v[i])) continue;
		double f = norm(v[i], lo, hi);
		int lv = (int)floor(f * 7 + 0.5);
		lt_style st = lt_slot_style(t, slot);
		st.fg = level_color(t, slot, f);
		lt_put(c, all, r.x + (i - first), r.y, uni ? blocks[lv] : (uint32_t)ascii[lv], st);
	}
}

void lt_graph(lt_canvas *c, lt_rect r, const lt_theme *t, const double *v, int n, double lo, double hi, lt_slot slot)
{
	lt_rect all = lt_full(c);
	if (r.w < 1 || r.h < 1 || n <= 0 || !v) return;
	if (r.h == 1) { lt_sparkline(c, r, t, v, n, lo, hi, slot); return; }
	static const uint32_t part[8] = {' ', 0x2581, 0x2582, 0x2583, 0x2584, 0x2585, 0x2586, 0x2587};
	bool uni = lt_canvas_caps(c)->unicode;
	int first = n > r.w ? n - r.w : 0;
	for (int i = first; i < n; i++) {
		if (!isfinite(v[i])) continue;
		double f = norm(v[i], lo, hi);
		int h8 = (int)floor(f * r.h * 8 + 0.5);
		if (h8 < 1) h8 = 1;                                    /* a zero still shows the low bar */
		for (int row = 0; row < r.h; row++) {
			int below = row * 8;                               /* eighths under this row, counted from the bottom */
			if (h8 <= below) break;
			int here = h8 - below;
			uint32_t ch = here >= 8 ? (uni ? 0x2588 : '#') : (uni ? part[here] : (here >= 4 ? '#' : '.'));
			lt_style st = lt_slot_style(t, slot);
			st.fg = level_color(t, slot, (double)(below + (here > 8 ? 8 : here)) / (r.h * 8.0));
			lt_put(c, all, r.x + (i - first), r.y + r.h - 1 - row, ch, st);
		}
	}
}
