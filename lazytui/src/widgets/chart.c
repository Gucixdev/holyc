/* chart.c - every data-visualisation widget in one file: web/spider chart, ship radar, pie bar + pie chart (+ the categorical palette
 * lt_theme_cat), gauge, line chart + bar chart (sharing one "nice number" axis frame below), histogram, GitHub-style dot heat grid, tag
 * cloud, page minimap. All stateless: feed them numbers (and the theme's gradient does the rest of the colouring). */
#include "lazytui.h"
#include "displayutil.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* ---- shared axis frame for linechart/barchart: "nice" tick spacing (1/2/5 x a power of ten), y labels on the left, an
 * optional grid, x labels below the plot ---- */
static double nice_step(double range, int maxticks)
{
	if (!(range > 0) || maxticks < 1) return 1;
	double raw = range / maxticks, mag = pow(10, floor(log10(raw))), f = raw / mag;
	double nf = f <= 1 ? 1 : f <= 2 ? 2 : f <= 5 ? 5 : 10;
	return nf * mag;
}
static void fmt_tick(char *o, size_t n, double v, double step)
{
	if (fabs(v) >= 1e6 || (fabs(v) > 0 && fabs(v) < 1e-3)) snprintf(o, n, "%.2g", v);
	else if (step >= 1) snprintf(o, n, "%.0f", v);
	else snprintf(o, n, "%.*f", step >= 0.1 ? 1 : step >= 0.01 ? 2 : 3, v);
}

typedef struct { lt_rect plot; double lo, hi, step; int lw; } axes;

/* draws the frame (y ticks + labels on the left, x baseline, optional grid) and returns the plot rectangle */
static bool draw_axes(lt_canvas *c, lt_rect r, const lt_theme *t, const lt_chart_opts *o, double lo, double hi, axes *ax, bool xrule)
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

static void x_labels(lt_canvas *c, const axes *ax, const lt_theme *t, const lt_chart_opts *o, int npoints, bool centered)
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

/* ================================================================== web (spider) chart: axes evenly spaced around the centre, a filled data polygon */
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

/* ================================================================== ship radar: range rings, bearing ticks, a rotating sweep with a fading trail, decaying blips */
#define PI 3.14159265358979323846

/* ---- the ship radar: a round scope, range rings, bearing ticks, a rotating sweep with a fading trail, blips that light up as the
 * beam passes and decay afterwards ---- */
static void wire_circle_at(lt_canvas *c, double ox, double oy, double cx, double cy, double R, lt_color col)
{
	int seg = (int)(R * 7) + 24;
	if (seg > 900) seg = 900;
	for (int i = 0; i < seg; i++) {
		double a0 = 2 * PI * i / seg, a1 = 2 * PI * (i + 1) / seg;
		lt_wire_line(c, ox + cx + R * cos(a0), oy + (cy + R * sin(a0)) / 2.0, ox + cx + R * cos(a1), oy + (cy + R * sin(a1)) / 2.0, col);
	}
}

/* bearing 0 = north (up), clockwise; radius 0..1 of the scope */
static void polar(double cx, double cy, double R, double bearing_deg, double rad, double *x, double *y)
{
	double a = bearing_deg * PI / 180;
	*x = cx + sin(a) * R * rad;
	*y = cy - cos(a) * R * rad;
}

void lt_radar(lt_canvas *c, lt_rect r, const lt_theme *t, double sweep_deg, const lt_blip *blips, int n, lt_color color)
{
	if (r.w < 8 || r.h < 5) return;
	if (!isfinite(sweep_deg)) sweep_deg = 0;
	sweep_deg = fmod(sweep_deg, 360.0);
	if (sweep_deg < 0) sweep_deg += 360.0;
	lt_rect all = {0, 0, lt_canvas_w(c), lt_canvas_h(c)};
	bool uni = lt_canvas_caps(c)->unicode;
	lt_rect pr = {r.x + 2, r.y + 1, r.w - 4, r.h - 2};          /* room for N E S W */
	if (pr.w < 4 || pr.h < 3) pr = r;
	double PW = pr.w, PH = 2.0 * pr.h, R = fmin(PW, PH) / 2.0, cx = PW / 2.0, cy = PH / 2.0;
	lt_color accent = color != LT_DEFAULT ? color : t->s[LT_T_TEXT].fg != LT_DEFAULT ? t->s[LT_T_TEXT].fg : t->s[LT_T_OK].fg;     /* the phosphor: the text colour */
	lt_color ground = t->s[LT_T_TEXT].bg;
	lt_color face = (ground != LT_DEFAULT && accent != LT_DEFAULT) ? lt_color_mix(ground, accent, 0.07) : ground;
	lt_color rim = accent;
	lt_color dim = (ground != LT_DEFAULT && accent != LT_DEFAULT) ? lt_color_mix(ground, accent, 0.38) : t->s[LT_T_DIM].fg;
	/* the face */
	{
		double xy[2];
		(void)xy;
		lt_circle(c, pr, face, rim);
	}
	double ox = pr.x, oy = pr.y;
	for (int k = 1; k <= 2; k++) wire_circle_at(c, ox, oy, cx, cy, R * k / 3.0 - 0.0, dim);     /* range rings at 1/3 and 2/3 */
	for (int b = 0; b < 360; b += 30) {                                                              /* bearing spokes, north/east/south/west brighter */
		double x0, y0, x1, y1;
		polar(cx, cy, R, b, 0, &x0, &y0); polar(cx, cy, R, b, (b % 90 == 0) ? 0.98 : 0.98, &x1, &y1);
		lt_wire_line(c, ox + x0, oy + y0 / 2.0, ox + x1, oy + y1 / 2.0, b % 90 == 0 ? dim : (ground != LT_DEFAULT ? lt_color_mix(ground, accent, 0.2) : dim));
	}
	for (int b = 0; b < 360; b += 10) {                                                              /* ticks just inside the rim */
		double x0, y0, x1, y1;
		polar(cx, cy, R, b, b % 30 == 0 ? 0.90 : 0.94, &x0, &y0); polar(cx, cy, R, b, 0.985, &x1, &y1);
		lt_wire_line(c, ox + x0, oy + y0 / 2.0, ox + x1, oy + y1 / 2.0, rim);
	}
	/* the trail: older angles first so the newest, brightest lines end up on top of the shared braille cells */
	const int K = 44;
	for (int k = K; k >= 1; k--) {
		double f = (double)k / K;
		double x1, y1;
		polar(cx, cy, R, sweep_deg - k * 1.25, 0.97, &x1, &y1);
		lt_color col = (ground != LT_DEFAULT && accent != LT_DEFAULT) ? lt_color_mix(accent, face, 0.12 + 0.86 * pow(f, 0.6)) : accent;
		lt_wire_line(c, ox + cx, oy + cy / 2.0, ox + x1, oy + y1 / 2.0, col);
	}
	{
		double x1, y1;
		polar(cx, cy, R, sweep_deg, 0.97, &x1, &y1);
		lt_color head = t->s[LT_T_ACCENT].fg != LT_DEFAULT ? lt_color_mix(accent, LT_RGB(255, 255, 255), 0.55) : accent;
		lt_wire_line(c, ox + cx, oy + cy / 2.0, ox + x1, oy + y1 / 2.0, head);
	}
	/* blips: bright right after the beam passed, fading over about a rotation */
	for (int i = 0; blips && i < n; i++) {
		if (!isfinite(blips[i].bearing) || !isfinite(blips[i].range)) continue;
		double age = fmod(sweep_deg - blips[i].bearing + 720.0, 360.0);
		double b = exp(-age / 110.0);
		if (b < 0.06) continue;
		double x, y;
		polar(cx, cy, R, blips[i].bearing, blips[i].range < 0 ? 0 : blips[i].range > 0.97 ? 0.97 : blips[i].range, &x, &y);
		int col = pr.x + (int)floor(x), row = pr.y + (int)floor(y / 2.0);
		lt_color bc = (ground != LT_DEFAULT && accent != LT_DEFAULT) ? lt_color_mix(face, lt_color_mix(accent, LT_RGB(255, 255, 255), 0.35), b) : accent;
		lt_style st = LT_STYLE(bc, face, b > 0.55 ? LT_BOLD : 0);
		lt_put(c, all, col, row, uni ? 0x25CF : 'o', st);
		if (b > 0.55) {                                                                  /* a fresh echo has a halo */
			lt_style h = LT_STYLE(dim, face, 0);
			lt_put(c, all, col - 1, row, uni ? 0xB7 : '.', h); lt_put(c, all, col + 1, row, uni ? 0xB7 : '.', h);
		}
		if (blips[i].label && *blips[i].label && b > 0.3) lt_text(c, all, col + 2, row, blips[i].label, LT_STYLE(bc, face, 0), pr.x + pr.w + 2 - (col + 2));
	}
	/* compass letters outside the rim */
	lt_style ls = t->s[LT_T_LABEL];
	lt_put(c, all, pr.x + pr.w / 2, r.y, 'N', ls); lt_put(c, all, pr.x + pr.w / 2, r.y + r.h - 1, 'S', ls);
	lt_put(c, all, r.x, pr.y + pr.h / 2, 'W', ls); lt_put(c, all, r.x + r.w - 1, pr.y + pr.h / 2, 'E', ls);
}

/* ================================================================== lt_theme_cat (the categorical palette), a segmented bar (piebar) and a circle/donut of sectors (piechart), both with a legend */
/* the i-th colour of a categorical palette (slices, series), harmonised with the theme; greys on themes without colour */
lt_color lt_theme_cat(const lt_theme *t, int i)
{
	static const unsigned pal[10] = {0x4cc9f0, 0x81c784, 0xffd54f, 0xff8a65, 0xf06292, 0xba68c8, 0x7986cb, 0x4db6ac, 0xa1887f, 0x90a4ae};
	i = ((i % 10) + 10) % 10;
	if (t->ngrad == 0 && (t->s[LT_T_TEXT].fg >> 24) == 2) { int g = 30 + (i * 17) % 170; return LT_RGB(g, g, g); }
	lt_color c = LT_RGB((pal[i] >> 16) & 255, (pal[i] >> 8) & 255, pal[i] & 255);
	lt_color acc = t->s[LT_T_ACCENT].fg;
	return acc != LT_DEFAULT && (acc >> 24) == 2 ? lt_color_mix(c, acc, 0.15) : c;
}

static double slice_total(const lt_slice *s, int n) { double t = 0; for (int i = 0; s && i < n; i++) if (isfinite(s[i].value) && s[i].value > 0) t += s[i].value; return t; }
static lt_color slice_color(const lt_theme *t, const lt_slice *s, int i) { return s->color != LT_DEFAULT ? s->color : lt_theme_cat(t, i); }

static void legend(lt_canvas *c, lt_rect r, const lt_theme *t, const lt_slice *s, int n, double total)
{
	lt_rect all = {0, 0, lt_canvas_w(c), lt_canvas_h(c)};
	bool uni = lt_canvas_caps(c)->unicode;
	for (int i = 0; i < n && i < r.h; i++) {
		double v = isfinite(s[i].value) && s[i].value > 0 ? s[i].value : 0;
		char pct[16];
		snprintf(pct, sizeof pct, "%3d%%", total > 0 ? (int)floor(v / total * 100 + 0.5) : 0);
		lt_put(c, all, r.x, r.y + i, uni ? 0x25A0 : '#', LT_STYLE(slice_color(t, &s[i], i), LT_DEFAULT, 0));
		int pw = r.w >= 12 ? 4 : 0;
		lt_text(c, all, r.x + 2, r.y + i, s[i].label ? s[i].label : "", t->s[LT_T_TEXT], r.w - 3 - (pw ? pw + 1 : 0));
		if (pw) lt_text(c, all, r.x + r.w - pw, r.y + i, pct, t->s[LT_T_DIM], pw);
	}
}

void lt_piebar(lt_canvas *c, lt_rect r, const lt_theme *t, const lt_slice *s, int n, bool with_legend)
{
	if (r.w < 2 || r.h < 1 || n <= 0 || !s) return;
	double total = slice_total(s, n);
	lt_rect bar = {r.x, r.y, r.w, 1};
	if (total <= 0) { lt_fill(c, bar, ' ', t->s[LT_T_BAR_BG]); return; }
	int *w = lt_arena_calloc(lt_scratch(), (size_t)n * sizeof *w);              /* largest remainder: widths sum to exactly r.w */
	double *rem = lt_arena_calloc(lt_scratch(), (size_t)n * sizeof *rem);
	lt_arena_mark m = lt_arena_save(lt_scratch());
	int used = 0;
	for (int i = 0; i < n; i++) { double q = (isfinite(s[i].value) && s[i].value > 0 ? s[i].value : 0) / total * r.w; w[i] = (int)floor(q); rem[i] = q - w[i]; used += w[i]; }
	while (used < r.w) { int b = 0; for (int i = 1; i < n; i++) if (rem[i] > rem[b]) b = i; w[b]++; rem[b] = -1; used++; }
	int x = r.x;
	for (int i = 0; i < n; i++) { lt_fill(c, (lt_rect){x, r.y, w[i], 1}, ' ', LT_STYLE(LT_DEFAULT, slice_color(t, &s[i], i), 0)); x += w[i]; }
	lt_arena_restore(lt_scratch(), m);
	if (with_legend && r.h > 1) legend(c, (lt_rect){r.x, r.y + 1, r.w, r.h - 1}, t, s, n, total);
}

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

/* ================================================================== a 270-degree arc (or full ring) filled 0..1 along the theme gradient */
static double fin(double v, double d) { return isfinite(v) ? v : d; }
static double clamp01(double v) { if (!(v > 0)) return 0; return v > 1 ? 1 : v; }

typedef struct { double v, cx, cy, R, r0; const lt_theme *t; bool full; } gaugectx;
static double gauge_shader(double x, double y, double rgb[3], void *ctx)
{
	gaugectx *g = ctx;
	double dx = x - g->cx, dy = y - g->cy, d2 = dx * dx + dy * dy;
	if (d2 > g->R * g->R || d2 < g->r0 * g->r0) return 0;
	double a = atan2(dy, dx) * 180.0 / M_PI, s, sweep;
	if (g->full) { s = fmod(a + 90 + 720, 360); sweep = 360; }
	else { s = fmod(a - 135 + 720, 360); sweep = 270; if (s > sweep) return 0; }
	int r, gg, b;
	bool on = s <= g->v * sweep;
	lt_color col = on ? ramp_or(g->t, s / sweep, LT_T_ACCENT) : g->t->s[LT_T_BAR_BG].bg != LT_DEFAULT ? g->t->s[LT_T_BAR_BG].bg : g->t->s[LT_T_DIM].fg;
	if (!lt_color_rgb(col, &r, &gg, &b)) { r = gg = b = on ? 220 : 70; }
	rgb[0] = r; rgb[1] = gg; rgb[2] = b;
	return 1;
}

void lt_gauge(lt_canvas *c, lt_rect r, const lt_theme *t, double value, const char *label, bool full_ring)
{
	if (r.w < 4 || r.h < 2) return;
	lt_rect all = {0, 0, lt_canvas_w(c), lt_canvas_h(c)};
	value = clamp01(fin(value, 0));
	double PW = r.w, PH = 2.0 * r.h, R = fmin(PW, PH) / 2.0;
	gaugectx g = {value, PW / 2.0, PH / 2.0, R, R * 0.66, t, full_ring};
	lt_shade(c, r, gauge_shader, &g);
	char b[16];
	snprintf(b, sizeof b, "%d%%", (int)floor(value * 100 + 0.5));
	int bw = (int)strlen(b), cx = r.x + r.w / 2 - bw / 2, cy = r.y + (r.h - 1) / 2;
	lt_text(c, all, cx, cy, b, over(c, cx, cy, ramp_or(t, value, LT_T_ACCENT), LT_BOLD), bw);
	if (label && *label && r.h >= 4) { int lw = lt_utf8_width(label); if (lw > r.w) lw = r.w; lt_text(c, all, r.x + (r.w - lw) / 2, cy + 1, label, over(c, r.x + r.w / 2, cy + 1, t->s[LT_T_DIM].fg, 0), lw); }
}

/* ================================================================== one or more series as braille wireframe lines over the shared axis frame, with a legend */
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

/* ================================================================== vertical bars over the shared axis frame, eighth-block resolution, optional value labels */

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

/* ================================================================== buckets a sample into bins and draws it with the bar chart above */
void lt_histogram(lt_canvas *c, lt_rect r, const lt_theme *t, const double *values, int n, int bins, double lo, double hi, const lt_chart_opts *o)
{
	if (!values || n <= 0 || bins < 1 || bins > 256) return;
	lt_arena_mark m = lt_arena_save(lt_scratch());
	double *cnt = lt_arena_calloc(lt_scratch(), (size_t)bins * sizeof *cnt);
	char **lab = lt_arena_alloc(lt_scratch(), (size_t)bins * sizeof *lab);
	if (!(hi > lo)) { lo = INFINITY; hi = -INFINITY; for (int i = 0; i < n; i++) if (isfinite(values[i])) { if (values[i] < lo) lo = values[i]; if (values[i] > hi) hi = values[i]; } if (!isfinite(lo)) { lo = 0; hi = 1; } if (!(hi > lo)) hi = lo + 1; }
	for (int i = 0; i < n; i++) {
		if (!isfinite(values[i]) || values[i] < lo || values[i] > hi) continue;
		int b = (int)floor((values[i] - lo) / (hi - lo) * bins);
		cnt[b >= bins ? bins - 1 : b]++;
	}
	for (int b = 0; b < bins; b++) { lab[b] = lt_arena_alloc(lt_scratch(), 16); snprintf(lab[b], 16, "%.3g", lo + (hi - lo) * (b + 0.5) / bins); }
	lt_barchart(c, r, t, (const char *const *)lab, cnt, bins, o);
	lt_arena_restore(lt_scratch(), m);
}

/* ================================================================== a GitHub-contributions-style grid of dots, growing and brightening with the value */

void lt_heatgrid(lt_canvas *c, lt_rect r, const lt_theme *t, const double *v, int cols, int rows, double lo, double hi, int cell_w, const char *const *row_labels)
{
	if (r.w < 2 || r.h < 1 || !v || cols < 1 || rows < 1) return;
	lt_rect all = {0, 0, lt_canvas_w(c), lt_canvas_h(c)};
	bool uni = lt_canvas_caps(c)->unicode;
	if (cell_w < 1) cell_w = 1;
	if (cell_w > 4) cell_w = 4;
	int lw = 0;
	for (int y = 0; row_labels && y < rows; y++) if (row_labels[y]) { int w = lt_utf8_width(row_labels[y]); if (w > lw) lw = w; }
	if (lw) lw++;
	for (int y = 0; y < rows && y < r.h; y++) {
		if (row_labels && row_labels[y]) lt_text(c, all, r.x, r.y + y, row_labels[y], t->s[LT_T_DIM], lw);
		for (int x = 0; x < cols; x++) {
			int cx = r.x + lw + x * (cell_w + 1) + cell_w / 2;
			if (cx >= r.x + r.w) break;
			double val = v[y * cols + x], f = hi > lo && isfinite(val) ? clamp01((val - lo) / (hi - lo)) : 0;
			if (!isfinite(val) || f <= 0) { lt_put(c, all, cx, r.y + y, uni ? 0xB7 : '.', t->s[LT_T_DIM]); continue; }
			lt_color col = t->ngrad > 0 ? lt_theme_ramp(t, f) : lt_color_mix(t->s[LT_T_DIM].fg, t->s[LT_T_ACCENT].fg, f);
			if (col == LT_DEFAULT) col = LT_PAL(f > 0.5 ? 2 : 8);
			uint32_t dot = uni ? (f > 0.6 ? 0x25CF /* big filled dot */ : f > 0.3 ? 0x2022 /* small bullet */ : 0x2219 /* tiny dot */) : (f > 0.5 ? '#' : '.');
			lt_put(c, all, cx, r.y + y, dot, LT_STYLE(col, LT_DEFAULT, f > 0.6 ? LT_BOLD : 0));
		}
	}
}

/* ================================================================== words centred on wrapped lines, weight -> size/emphasis */
/* the breathing room around a word: heavier words get more of it, light ones get crammed tight - a "size" signal that reads even in a
 * low-contrast or monochrome theme, where the colour/bold emphasis below can wash out */
static int tagcloud_pad(double f) { return f > 0.66 ? 3 : f > 0.33 ? 2 : 1; }

void lt_tagcloud(lt_canvas *c, lt_rect r, const lt_theme *t, const char *const *words, const double *weights, int n)
{
	if (r.w < 4 || r.h < 1 || !words || n <= 0) return;
	lt_rect all = {0, 0, lt_canvas_w(c), lt_canvas_h(c)};
	double mx = 0, mn = INFINITY;
	for (int i = 0; weights && i < n; i++) if (isfinite(weights[i])) { if (weights[i] > mx) mx = weights[i]; if (weights[i] < mn) mn = weights[i]; }
	if (!isfinite(mn)) mn = 0;
	/* pass 1: the words of each line; pass 2: draw the line centred */
	int i = 0, y = r.y;
	while (i < n && y < r.y + r.h) {
		int j = i, used = 0;
		while (j < n) {
			double f = weights && mx > mn && isfinite(weights[j]) ? (weights[j] - mn) / (mx - mn) : 0.5;
			int w = lt_utf8_width(words[j] ? words[j] : "") + (j > i ? tagcloud_pad(f) : 0);
			if (used + w > r.w && j > i) break;
			used += w; j++;
		}
		int x = r.x + (r.w - used) / 2;
		for (int k = i; k < j; k++) {
			const char *word = words[k] ? words[k] : "";
			double f = weights && mx > mn && isfinite(weights[k]) ? (weights[k] - mn) / (mx - mn) : 0.5;
			lt_style st = f > 0.66 ? t->s[LT_T_ACCENT] : f > 0.33 ? t->s[LT_T_TEXT] : t->s[LT_T_DIM];
			if (t->ngrad > 0) st.fg = lt_theme_ramp(t, f);
			if (f > 0.66) st.attr |= LT_BOLD;
			int w = lt_utf8_width(word);
			if (w > r.w) w = r.w;
			lt_text(c, all, x, y, word, st, w);
			x += w + tagcloud_pad(f);
		}
		i = j; y++;
	}
}

/* ================================================================== a page as braille density with the viewport highlighted and marks on the right edge */
void lt_minimap(lt_canvas *c, lt_rect r, const lt_theme *t, const uint8_t *density, int n, int offset, int page, const int *marks, int nmarks)
{
	if (r.w < 1 || r.h < 1 || !density || n <= 0) return;
	lt_rect all = {0, 0, lt_canvas_w(c), lt_canvas_h(c)};
	bool uni = lt_canvas_caps(c)->unicode;
	static const uint8_t bit[4][2] = {{0x01, 0x08}, {0x02, 0x10}, {0x04, 0x20}, {0x40, 0x80}};
	double per_row = (double)n / r.h;                                                            /* source lines per cell row */
	for (int y = 0; y < r.h; y++) {
		int l0 = (int)floor(y * per_row), l1 = (int)floor((y + 1) * per_row);
		if (l1 <= l0) l1 = l0 + 1;
		bool inview = page > 0 && l1 > offset && l0 < offset + page;
		lt_style st = inview ? LT_STYLE(t->s[LT_T_TEXT].fg, t->s[LT_T_BAR_BG].bg, 0) : t->s[LT_T_DIM];
		for (int x = 0; x < r.w; x++) {
			int bits = 0;
			for (int d = 0; d < 4; d++) {
				int a = l0 + (l1 - l0) * d / 4, b = l0 + (l1 - l0) * (d + 1) / 4;
				if (b <= a) b = a + 1;
				long sum = 0;
				for (int k = a; k < b && k < n; k++) sum += density[k];
				int avg = (int)(sum / (b - a));
				if (avg > 40) bits |= bit[d][0];
				if (avg > 130) bits |= bit[d][1];
			}
			lt_put(c, all, r.x + x, r.y + y, uni ? (uint32_t)(0x2800 + bits) : (bits > 12 ? '#' : bits ? '.' : ' '), st);
		}
		for (int k = 0; marks && k < nmarks; k++) if (marks[k] >= l0 && marks[k] < l1) lt_put(c, all, r.x + r.w - 1, r.y + y, uni ? 0x25C0 : '<', t->s[LT_T_WARN]);
	}
}
