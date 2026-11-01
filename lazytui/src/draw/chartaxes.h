/* draw/chartaxes.h - the axis frame linechart and barchart share: "nice" tick spacing (1/2/5 x a power of ten), y labels on the left,
 * an optional grid, x labels below the plot. Private helpers, not part of the public API. */
#ifndef LT_CHARTAXES_H
#define LT_CHARTAXES_H
#include "lazytui.h"
#include "util.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static inline double nice_step(double range, int maxticks)
{
	if (!(range > 0) || maxticks < 1) return 1;
	double raw = range / maxticks, mag = pow(10, floor(log10(raw))), f = raw / mag;
	double nf = f <= 1 ? 1 : f <= 2 ? 2 : f <= 5 ? 5 : 10;
	return nf * mag;
}
static inline void fmt_tick(char *o, size_t n, double v, double step)
{
	if (fabs(v) >= 1e6 || (fabs(v) > 0 && fabs(v) < 1e-3)) snprintf(o, n, "%.2g", v);
	else if (step >= 1) snprintf(o, n, "%.0f", v);
	else snprintf(o, n, "%.*f", step >= 0.1 ? 1 : step >= 0.01 ? 2 : 3, v);
}

typedef struct { lt_rect plot; double lo, hi, step; int lw; } axes;

/* draws the frame (y ticks + labels on the left, x baseline, optional grid) and returns the plot rectangle */
static inline bool draw_axes(lt_canvas *c, lt_rect r, const lt_theme *t, const lt_chart_opts *o, double lo, double hi, axes *ax, bool xrule)
{
	lt_rect all = {0, 0, lt_canvas_w(c), lt_canvas_h(c)};
	bool uni = lt_canvas_caps(c)->unicode;
	if (!(hi > lo)) { hi = lo + 1; }
	int title_rows = o && o->title && *o->title ? 1 : 0;
	int maxticks = (r.h - title_rows - 2) / 3;
	if (maxticks < 2) maxticks = 2;
	double step = nice_step(hi - lo, maxticks);
	double nlo = floor(lo / step) * step, nhi = ceil(hi / step) * step;
	if (o && !o->autoscale && isfinite(o->ymin) && isfinite(o->ymax) && o->ymax > o->ymin) { nlo = o->ymin; nhi = o->ymax; }
	char lab[24];
	int lw = 1;
	for (double v = nlo; v <= nhi + step * 1e-9; v += step) { fmt_tick(lab, sizeof lab, v, step); int w = (int)strlen(lab); if (w > lw) lw = w; }
	int bottom = xrule ? 2 : 1;
	lt_rect plot = {r.x + lw + 1, r.y + title_rows, r.w - lw - 1, r.h - title_rows - bottom};
	if (plot.w < 3 || plot.h < 2) return false;
	if (title_rows) lt_text(c, all, r.x + (r.w - lt_utf8_width(o->title)) / 2 > r.x ? r.x + (r.w - lt_utf8_width(o->title)) / 2 : r.x, r.y, o->title, t->s[LT_T_TITLE], r.w);
	for (int y = 0; y < plot.h; y++) lt_put(c, all, plot.x - 1, plot.y + y, uni ? 0x2502 : '|', t->s[LT_T_BORDER]);
	if (xrule) for (int x = 0; x < plot.w; x++) lt_put(c, all, plot.x + x, plot.y + plot.h, uni ? 0x2500 : '-', t->s[LT_T_BORDER]);
	if (xrule) lt_put(c, all, plot.x - 1, plot.y + plot.h, uni ? 0x2514 : '+', t->s[LT_T_BORDER]);
	for (double v = nlo; v <= nhi + step * 1e-9; v += step) {
		double f = (v - nlo) / (nhi - nlo);
		int y = plot.y + (int)floor((1 - f) * (plot.h - 1) + 0.5);
		fmt_tick(lab, sizeof lab, v, step);
		int w = (int)strlen(lab);
		lt_text(c, all, r.x + lw - w, y, lab, t->s[LT_T_DIM], w);
		lt_put(c, all, plot.x - 1, y, uni ? 0x2524 : '+', t->s[LT_T_BORDER]);
		if (o && o->grid) for (int x = 0; x < plot.w; x += 2) lt_put(c, all, plot.x + x, y, uni ? 0xB7 : '.', t->s[LT_T_DIM]);
	}
	*ax = (axes){plot, nlo, nhi, step, lw};
	return true;
}

static inline void x_labels(lt_canvas *c, const axes *ax, const lt_theme *t, const lt_chart_opts *o, int npoints, bool centered)
{
	if (!o || !o->xlabels || o->nxlabels <= 0 || npoints <= 0) return;
	lt_rect all = {0, 0, lt_canvas_w(c), lt_canvas_h(c)};
	int last_end = ax->plot.x - 1;
	for (int i = 0; i < o->nxlabels; i++) {
		if (!o->xlabels[i]) continue;
		int idx = o->nxlabels == 1 ? 0 : (int)floor((double)i * (npoints - 1) / (o->nxlabels - 1) + 0.5);
		double f = npoints == 1 ? 0 : (double)idx / (npoints - 1);
		int x = ax->plot.x + (centered ? (int)floor((idx + 0.5) * ax->plot.w / npoints) : (int)floor(f * (ax->plot.w - 1) + 0.5)), w = lt_utf8_width(o->xlabels[i]);
		int sx = x - w / 2;
		if (sx < ax->plot.x) sx = ax->plot.x;                                                /* the first label hugs the left edge instead of centering off it */
		if (sx < last_end + 1 || sx + w > ax->plot.x + ax->plot.w) continue;                 /* labels that would collide are skipped */
		lt_text(c, all, sx, ax->plot.y + ax->plot.h + 1, o->xlabels[i], t->s[LT_T_DIM], w);
		last_end = sx + w;
	}
}

#endif
