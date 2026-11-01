/* barchart.c - vertical bars over the shared axis frame, labels under them.
 * Needs: lazytui.h (the lib core). One widget, one file: copy it into any project that links the core. */
#include "lazytui.h"
#include "chartaxes.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

void lt_barchart(lt_canvas *c, lt_rect r, const lt_theme *t, const char *const *labels, const double *values, int n, const lt_chart_opts *o)
{
	if (r.w < 8 || r.h < 4 || !values || n <= 0) return;
	lt_rect all = {0, 0, lt_canvas_w(c), lt_canvas_h(c)};
	bool uni = lt_canvas_caps(c)->unicode;
	double hi = 0;
	for (int i = 0; i < n; i++) if (isfinite(values[i]) && values[i] > hi) hi = values[i];
	if (hi <= 0) hi = 1;
	axes ax;
	if (!draw_axes(c, r, t, o, 0, hi, &ax, true)) return;
	lt_rect p = ax.plot;
	static const uint32_t part[9] = {' ', 0x2581, 0x2582, 0x2583, 0x2584, 0x2585, 0x2586, 0x2587, 0x2588};
	int slot = p.w / n;                                                                          /* columns per bar incl. gap */
	if (slot < 1) { n = p.w; slot = 1; }
	int bw = slot > 1 ? slot - 1 : 1;
	if (bw > 8) bw = 8;
	int x0 = p.x + (p.w - slot * n) / 2;
	for (int i = 0; i < n; i++) {
		double v = isfinite(values[i]) && values[i] > 0 ? values[i] : 0;
		double f = clamp01(v / ax.hi);
		int h8 = (int)floor(f * p.h * 8 + 0.5);
		if (v > 0 && h8 < 1) h8 = 1;
		lt_color col = ramp_or(t, f, LT_T_ACCENT);
		int bx = x0 + i * slot + (slot - bw) / 2;
		for (int row = 0; row < p.h; row++) {
			int below = row * 8;
			if (h8 <= below) break;
			int here = h8 - below > 8 ? 8 : h8 - below;
			for (int k = 0; k < bw; k++) lt_put(c, all, bx + k, p.y + p.h - 1 - row, uni ? part[here] : (here >= 4 ? '#' : '.'), LT_STYLE(col, LT_DEFAULT, 0));
		}
		if (labels && labels[i] && bw >= 1) {
			int lw = lt_utf8_width(labels[i]);
			if (lw > slot) lw = slot;
			lt_text(c, all, x0 + i * slot + (slot - lw) / 2, p.y + p.h + 1, labels[i], t->s[LT_T_DIM], lw);
		}
		if (o && o->values && slot >= 4 && h8 / 8 + 1 < p.h) {                                   /* the value above the bar */
			char b[16];
			snprintf(b, sizeof b, "%g", v);
			int w = (int)strlen(b);
			if (w <= slot) lt_text(c, all, x0 + i * slot + (slot - w) / 2, p.y + p.h - 1 - (h8 + 7) / 8 - 1 + 0, b, t->s[LT_T_TEXT], w);
		}
	}
}
