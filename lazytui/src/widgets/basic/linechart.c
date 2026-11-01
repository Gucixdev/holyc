/* linechart.c - one or more series as braille wireframes over the shared axis frame.
 * Needs: lazytui.h (the lib core). One widget, one file: copy it into any project that links the core. */
#include "lazytui.h"
#include "chartaxes.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

void lt_linechart(lt_canvas *c, lt_rect r, const lt_theme *t, const lt_series *ser, int ns, const lt_chart_opts *o)
{
	if (r.w < 10 || r.h < 5 || !ser || ns <= 0) return;
	lt_rect all = {0, 0, lt_canvas_w(c), lt_canvas_h(c)};
	double lo = INFINITY, hi = -INFINITY;
	int maxn = 0;
	for (int k = 0; k < ns; k++) for (int i = 0; ser[k].v && i < ser[k].n; i++) if (isfinite(ser[k].v[i])) { if (ser[k].v[i] < lo) lo = ser[k].v[i]; if (ser[k].v[i] > hi) hi = ser[k].v[i]; if (i + 1 > maxn) maxn = i + 1; }
	for (int k = 0; k < ns; k++) if (ser[k].n > maxn) maxn = ser[k].n;
	if (!isfinite(lo)) { lo = 0; hi = 1; }
	if (lo > 0 && lo < (hi - lo) * 0.3 ) lo = 0;                                             /* start at zero unless the data sits far above it */
	axes ax;
	if (!draw_axes(c, r, t, o, lo, hi, &ax, true)) return;
	lt_rect p = ax.plot;
	lt_canvas_set_clip(c, p);
	for (int k = 0; k < ns; k++) {
		lt_color col = ser[k].color != LT_DEFAULT ? ser[k].color : lt_theme_cat(t, k);
		double px = 0, py = 0;
		bool have = false;
		for (int i = 0; ser[k].v && i < ser[k].n; i++) {
			if (!isfinite(ser[k].v[i])) { have = false; continue; }
			double f = ser[k].n > 1 ? (double)i / (ser[k].n - 1) : 0.5;
			double x = p.x + f * (p.w - 1) + 0.25, y = p.y + (1 - (ser[k].v[i] - ax.lo) / (ax.hi - ax.lo)) * (p.h - 1) + 0.12;
			if (have) lt_wire_line(c, px, py, x, y, col); else lt_wire_line(c, x, y, x, y, col);
			px = x; py = y; have = true;
		}
	}
	lt_canvas_set_clip(c, all);
	x_labels(c, &ax, t, o, maxn, false);
	if (!o || o->legend) {                                                                       /* legend: top right, one entry per series */
		int x = r.x + r.w;
		for (int k = ns - 1; k >= 0; k--) {
			if (!ser[k].name) continue;
			int w = lt_utf8_width(ser[k].name) + 2;
			x -= w + 1;
			if (x < p.x) break;
			lt_color col = ser[k].color != LT_DEFAULT ? ser[k].color : lt_theme_cat(t, k);
			lt_put(c, all, x, r.y, lt_canvas_caps(c)->unicode ? 0x25A0 : '#', LT_STYLE(col, LT_DEFAULT, 0));
			lt_text(c, all, x + 2, r.y, ser[k].name, t->s[LT_T_TEXT], w - 2);
		}
	}
}
