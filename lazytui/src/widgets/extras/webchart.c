/* webchart.c - web (spider) chart: axes evenly spaced around the centre, a filled data polygon.
 * Needs: lazytui.h (the lib core). One widget, one file: copy it into any project that links the core. */
#include "lazytui.h"
#include "util.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define PI 3.14159265358979323846

void lt_webchart(lt_canvas *c, lt_rect r, const lt_theme *t, const lt_axis *axes, int n, double max, int rings, lt_color color)
{
	if (r.w < 6 || r.h < 4 || n < 3 || n > 32 || !axes || !(max > 0)) return;
	if (rings < 1) rings = 1;
	if (rings > 12) rings = 12;
	lt_rect all = {0, 0, lt_canvas_w(c), lt_canvas_h(c)};
	bool uni = lt_canvas_caps(c)->unicode;
	/* room for the labels: the widest label on each side, one row above and below */
	int lw = 0;
	for (int i = 0; i < n; i++) { int w = axes[i].label ? lt_utf8_width(axes[i].label) : 0; if (w > lw) lw = w; }
	int mx = lw + 1, my = 1;
	if (r.w - 2 * mx < 4) mx = (r.w - 4) / 2 > 0 ? (r.w - 4) / 2 : 0;
	lt_rect pr = {r.x + mx, r.y + my, r.w - 2 * mx, r.h - 2 * my};
	if (pr.w < 2 || pr.h < 2) return;
	double PW = pr.w, PH = 2.0 * pr.h, R = fmin(PW, PH) / 2.0, cx = PW / 2.0, cy = PH / 2.0;
	lt_color accent = color != LT_DEFAULT ? color : t->s[LT_T_ACCENT].fg;
	lt_color ground = t->s[LT_T_TEXT].bg;
	lt_color fill = ground != LT_DEFAULT && accent != LT_DEFAULT ? lt_color_mix(ground, accent, 0.38) : accent;
	lt_arena_mark sm_ = lt_arena_save(lt_scratch());
	double *vx = lt_arena_alloc(lt_scratch(), (size_t)n * sizeof *vx), *vy = lt_arena_alloc(lt_scratch(), (size_t)n * sizeof *vy), *xy = lt_arena_alloc(lt_scratch(), (size_t)n * 2 * sizeof *xy);
	for (int i = 0; i < n; i++) {
		double a = -PI / 2 + 2 * PI * i / n, v = isfinite(axes[i].value) ? axes[i].value : 0;
		v = v < 0 ? 0 : v > max ? max : v;
		vx[i] = cos(a); vy[i] = sin(a);
		xy[2 * i] = cx + vx[i] * R * v / max; xy[2 * i + 1] = cy + vy[i] * R * v / max;
	}
	lt_polygon_phys(c, pr, xy, n, fill, accent);                       /* data first: the grid then goes on the empty cells */
	lt_color grid = t->s[LT_T_DIM].fg != LT_DEFAULT ? t->s[LT_T_DIM].fg : t->s[LT_T_BORDER].fg;
	for (int k = 1; k <= rings; k++) {                                /* rings: wire polygons, in cell coordinates */
		double rr = R * k / rings;
		for (int i = 0; i < n; i++) {
			int j = (i + 1) % n;
			lt_wire_line(c, pr.x + cx + vx[i] * rr, pr.y + (cy + vy[i] * rr) / 2.0, pr.x + cx + vx[j] * rr, pr.y + (cy + vy[j] * rr) / 2.0, grid);
		}
	}
	for (int i = 0; i < n; i++) lt_wire_line(c, pr.x + cx, pr.y + cy / 2.0, pr.x + cx + vx[i] * R, pr.y + (cy + vy[i] * R) / 2.0, grid);
	/* labels just outside each vertex, aligned away from the centre */
	for (int i = 0; i < n; i++) {
		const char *lab = axes[i].label;
		if (!lab || !*lab) continue;
		double px = pr.x + cx + vx[i] * (R + 1.5), py = pr.y + (cy + vy[i] * (R + 1.5)) / 2.0;
		int w = lt_utf8_width(lab), x, y = (int)floor(py);
		if (vx[i] > 0.3) x = (int)floor(px) + 0;
		else if (vx[i] < -0.3) x = (int)floor(px) - w + 1;
		else x = (int)floor(px + 0.5) - w / 2;
		if (vy[i] < -0.5) y = pr.y - 1 > r.y - 1 ? y : y;
		lt_text(c, all, x, y, lab, t->s[LT_T_LABEL], w);
	}
	(void)uni;
	lt_arena_restore(lt_scratch(), sm_);
}
