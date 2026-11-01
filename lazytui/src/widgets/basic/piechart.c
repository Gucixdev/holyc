/* piechart.c - a circle or donut of sectors with a legend (sampled through lt_shade).
 * Needs: lazytui.h (the lib core). One widget, one file: copy it into any project that links the core. */
#include "lazytui.h"
#include "legend.h"
#include <math.h>
#include <stdlib.h>

typedef struct { const lt_slice *s; int n; double total, cx, cy, R, hole; const lt_theme *t; double *edge; } piectx;
static double pie_shader(double x, double y, double rgb[3], void *ctx)
{
	piectx *p = ctx;
	double dx = x - p->cx, dy = y - p->cy, d2 = dx * dx + dy * dy;
	if (d2 > p->R * p->R || d2 < p->hole * p->hole) return 0;
	double a = atan2(dy, dx) + M_PI / 2;                                  /* 0 at the top, clockwise */
	if (a < 0) a += 2 * M_PI;
	double f = a / (2 * M_PI);
	int i = 0;
	while (i < p->n - 1 && f >= p->edge[i]) i++;
	lt_color col = slice_color(p->t, &p->s[i], i);
	int r, g, b;
	if (!lt_color_rgb(col, &r, &g, &b)) { r = g = b = 200; }
	rgb[0] = r; rgb[1] = g; rgb[2] = b;
	return 1;
}

void lt_piechart(lt_canvas *c, lt_rect r, const lt_theme *t, const lt_slice *s, int n, bool donut, bool with_legend)
{
	if (r.w < 4 || r.h < 2 || n <= 0 || !s) return;
	double total = slice_total(s, n);
	lt_rect area = r, leg = {0, 0, 0, 0};
	if (with_legend && r.w >= 26) { int lw = r.w / 2 > 26 ? 26 : r.w / 2; area.w = r.w - lw - 1; leg = (lt_rect){r.x + area.w + 1, r.y + (r.h > n ? (r.h - n) / 2 : 0), lw, r.h > n ? n : r.h}; }
	if (total <= 0) return;
	lt_arena_mark m = lt_arena_save(lt_scratch());
	double *edge = lt_arena_alloc(lt_scratch(), (size_t)n * sizeof *edge), acc = 0;
	for (int i = 0; i < n; i++) { acc += (isfinite(s[i].value) && s[i].value > 0 ? s[i].value : 0) / total; edge[i] = acc; }
	double PW = area.w, PH = 2.0 * area.h, R = fmin(PW, PH) / 2.0;
	piectx q = {s, n, total, PW / 2.0, PH / 2.0, R, donut ? R * 0.52 : 0, t, edge};
	lt_shade(c, area, pie_shader, &q);
	lt_arena_restore(lt_scratch(), m);
	if (leg.w > 0) legend(c, leg, t, s, n, total);
}
