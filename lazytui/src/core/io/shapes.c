/* shapes.c - circle and hexagon. Each cell is sampled on an 8x16 grid (the cell taken as 1 wide, 2 tall in physical units)
 * and drawn with whichever block glyph - eighth blocks, halves, quadrants - and pair of colours reproduces those samples
 * best. That is a per-cell two-colour fit, so a curve is followed to about 1/8 of a cell instead of being staircased to
 * half cells.
 *
 * Known background (an explicit colour in the cell): the two colours are the mean of the samples on each side of the glyph, so
 * the edge blends and anti-aliases. Unknown (terminal default): only the shape's own colour can be named, so the fit
 * minimises the geometric mismatch and the missing orientation of a glyph is produced with reverse video, whose "background"
 * is the terminal's own. */
#include "lazytui.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SX 8
#define SY 16
#define NS (SX * SY)

typedef struct { uint32_t ch; uint8_t m[NS]; } glyph;
static glyph G[48];
static int NG;

static void add_glyph(uint32_t ch, int (*pred)(int i, int j, int k), int k)
{
	glyph *g = &G[NG++];
	g->ch = ch;
	for (int j = 0; j < SY; j++) for (int i = 0; i < SX; i++) g->m[j * SX + i] = (uint8_t)pred(i, j, k);
}
static int p_lower(int i, int j, int k) { (void)i; return j >= SY - 2 * k; }
static int p_left(int i, int j, int k) { (void)j; return i < k; }
static int p_upper(int i, int j, int k) { (void)i; return j < 2 * k; }
static int p_right(int i, int j, int k) { (void)j; return i >= SX - k; }
static int p_quad(int i, int j, int mask) { return (mask >> ((j >= SY / 2 ? 2 : 0) + (i >= SX / 2 ? 1 : 0))) & 1; }   /* bit0 UL, 1 UR, 2 LL, 3 LR */
static int p_full(int i, int j, int k) { (void)i; (void)j; (void)k; return 1; }
static int p_none(int i, int j, int k) { (void)i; (void)j; (void)k; return 0; }

static void init_glyphs(void)
{
	if (NG) return;
	add_glyph(' ', p_none, 0);
	add_glyph(0x2588, p_full, 0);
	for (int k = 1; k <= 7; k++) add_glyph(0x2580 + k, p_lower, k);              /* U+2581..2587 lower k/8 */
	for (int k = 1; k <= 7; k++) add_glyph(0x2590 - k, p_left, k);               /* U+258F..2589 left k/8 */
	add_glyph(0x2580, p_upper, 4);                                              /* upper half */
	add_glyph(0x2594, p_upper, 1);                                              /* upper 1/8 */
	add_glyph(0x2590, p_right, 4);                                              /* right half */
	add_glyph(0x2595, p_right, 1);                                              /* right 1/8 */
	static const struct { uint32_t ch; int mask; } q[] = {{0x2598, 1}, {0x259D, 2}, {0x2596, 4}, {0x2597, 8}, {0x259A, 1 | 8}, {0x259E, 2 | 4},
	                                                     {0x2599, 1 | 4 | 8}, {0x259B, 1 | 2 | 4}, {0x259C, 1 | 2 | 8}, {0x259F, 2 | 4 | 8}};
	for (size_t i = 0; i < sizeof q / sizeof q[0]; i++) add_glyph(q[i].ch, p_quad, q[i].mask);
}

/* the shape being drawn, in physical coordinates (x in cell widths, y in half cells, both from the rect's corner) */
typedef struct {
	bool circle;
	double cx, cy, R;                 /* circle */
	double *p; int n;                 /* polygon vertices x0,y0,x1,y1,... */
	double ring;                      /* outline width; 0 = none */
} geom;

/* 0 outside, 1 fill, 2 outline */
static int classify(const geom *g, double x, double y)
{
	if (g->circle) {
		double dx = x - g->cx, dy = y - g->cy, d2 = dx * dx + dy * dy;
		if (d2 > g->R * g->R) return 0;
		double in = g->R - g->ring;
		return (g->ring > 0 && (in <= 0 || d2 > in * in)) ? 2 : 1;
	}
	bool inside = false;                                          /* even-odd crossing test: works for star-shaped polygons too */
	double mind = 1e300;
	for (int i = 0, j = g->n - 1; i < g->n; j = i++) {
		double xi = g->p[2 * i], yi = g->p[2 * i + 1], xj = g->p[2 * j], yj = g->p[2 * j + 1];
		if (((yi > y) != (yj > y)) && (x < (xj - xi) * (y - yi) / (yj - yi) + xi)) inside = !inside;
		if (g->ring > 0) {                                        /* distance to this edge */
			double ex = xj - xi, ey = yj - yi, l2 = ex * ex + ey * ey, t = l2 > 0 ? ((x - xi) * ex + (y - yi) * ey) / l2 : 0;
			t = t < 0 ? 0 : t > 1 ? 1 : t;
			double dx = x - (xi + t * ex), dy = y - (yi + t * ey), d = dx * dx + dy * dy;
			if (d < mind) mind = d;
		}
	}
	if (!inside) return 0;
	return (g->ring > 0 && mind < g->ring * g->ring) ? 2 : 1;
}

static lt_color rgb_color(double r, double g, double b)
{
	int R = (int)(r + 0.5), Gc = (int)(g + 0.5), B = (int)(b + 0.5);
	return LT_RGB(R < 0 ? 0 : R > 255 ? 255 : R, Gc < 0 ? 0 : Gc > 255 ? 255 : Gc, B < 0 ? 0 : B > 255 ? 255 : B);
}

/* indexes of the samples each glyph paints, for fast partition sums */
static uint8_t GI[48][NS];
static int GN[48];
static void init_index(void)
{
	static bool done;
	if (done) return;
	done = true;
	for (int g = 0; g < NG; g++) { GN[g] = 0; for (int s = 0; s < NS; s++) if (G[g].m[s]) GI[g][GN[g]++] = (uint8_t)s; }
}

/* ---- the pixel shader engine: fn(x, y) gives coverage 0..1 and a colour for the physical point (x in cell widths, y in half cells,
 * from r's corner); every cell is fitted with the block glyph and two colours that reproduce its 8x16 samples best (least squares,
 * computed from partition sums: err = sum|c|^2 - |sumA|^2/nA - |sumB|^2/nB) ---- */
void lt_shade(lt_canvas *c, lt_rect r, lt_shader_fn fn, void *ctx)
{
	if (r.w < 1 || r.h < 1 || !fn) return;
	init_glyphs();
	init_index();
	const lt_caps *caps = lt_canvas_caps(c);
	lt_rect all = {0, 0, lt_canvas_w(c), lt_canvas_h(c)};
	double al[NS], col[NS][3];
	for (int y = 0; y < r.h; y++)
		for (int x = 0; x < r.w; x++) {
			int X = r.x + x, Y = r.y + y;
			if (X < 0 || Y < 0 || X >= all.w || Y >= all.h) continue;
			int nop = 0, nany = 0;
			{                                                     /* coarse pass: 32 of the 128 samples; a cell they all agree on is flat and needs no more */
				double cmn[3] = {1e9, 1e9, 1e9}, cmx[3] = {-1e9, -1e9, -1e9}, cs[3] = {0, 0, 0}, cc[3];
				bool flat = true;
				for (int j = 1; j < SY && flat; j += 2)
					for (int i = 1; i < SX; i += 2) {
						double a = fn(x + (i + 0.5) / SX, 2.0 * y + (j + 0.5) * (2.0 / SY), cc, ctx);
						if (!(a >= 1)) { flat = false; break; }
						for (int k = 0; k < 3; k++) { if (cc[k] < cmn[k]) cmn[k] = cc[k]; if (cc[k] > cmx[k]) cmx[k] = cc[k]; cs[k] += cc[k]; }
					}
				if (flat && cmx[0] - cmn[0] <= 10 && cmx[1] - cmn[1] <= 10 && cmx[2] - cmn[2] <= 10) {
					lt_put(c, all, X, Y, ' ', LT_STYLE(LT_DEFAULT, rgb_color(cs[0] / 32, cs[1] / 32, cs[2] / 32), 0));
					continue;
				}
			}
			bool uniform = true;
			double mn[3] = {1e9, 1e9, 1e9}, mx[3] = {-1e9, -1e9, -1e9}, sum[3] = {0, 0, 0};
			for (int j = 0; j < SY; j++)
				for (int i = 0; i < SX; i++) {
					int s = j * SX + i;
					double px = x + (i + 0.5) / SX, py = 2.0 * y + (j + 0.5) * (2.0 / SY);
					al[s] = fn(px, py, col[s], ctx);
					if (!(al[s] > 0)) { al[s] = 0; continue; }
					if (al[s] > 1) al[s] = 1;
					nany++;
					if (al[s] > 0.5) nop++;
					if (al[s] < 1) uniform = false;
					for (int k = 0; k < 3; k++) { if (col[s][k] < mn[k]) mn[k] = col[s][k]; if (col[s][k] > mx[k]) mx[k] = col[s][k]; sum[k] += col[s][k]; }
				}
			if (!nany) continue;
			if (!caps->unicode) {                                 /* no block glyphs: a whole cell, its mean colour as background */
				if (nop < NS / 2) continue;
				double m[3] = {0, 0, 0};
				for (int s = 0; s < NS; s++) if (al[s] > 0.5) { m[0] += col[s][0]; m[1] += col[s][1]; m[2] += col[s][2]; }
				lt_color cc = rgb_color(m[0] / nop, m[1] / nop, m[2] / nop);
				if (caps->colors >= 16) lt_put(c, all, X, Y, ' ', LT_STYLE(LT_DEFAULT, cc, 0));
				else lt_put(c, all, X, Y, '#', LT_STYLE(LT_DEFAULT, LT_DEFAULT, 0));
				continue;
			}
			/* a cell whose samples all cover it and differ by less than ~4% of the range (texture noise) is one flat colour: skips the glyph fit,
			 * which is where nearly all the time goes */
			if (uniform && nany == NS && mx[0] - mn[0] <= 10 && mx[1] - mn[1] <= 10 && mx[2] - mn[2] <= 10) { lt_put(c, all, X, Y, ' ', LT_STYLE(LT_DEFAULT, rgb_color(sum[0] / NS, sum[1] / NS, sum[2] / NS), 0)); continue; }
			lt_color under = lt_canvas_bg(c, X, Y);
			int br, bgc, bb;
			bool known = lt_color_rgb(under, &br, &bgc, &bb);
			double v[NS][3], tot[3] = {0, 0, 0}, sq = 0;
			if (known) {
				for (int s = 0; s < NS; s++) {
					v[s][0] = br + (col[s][0] - br) * al[s]; v[s][1] = bgc + (col[s][1] - bgc) * al[s]; v[s][2] = bb + (col[s][2] - bb) * al[s];
					tot[0] += v[s][0]; tot[1] += v[s][1]; tot[2] += v[s][2];
					sq += v[s][0] * v[s][0] + v[s][1] * v[s][1] + v[s][2] * v[s][2];
				}
				double best = 1e300, fa[3] = {0}, fb[3] = {0};
				uint32_t bestch = ' ';
				for (int g = 0; g < NG; g++) {
					double a[3] = {0, 0, 0};
					int na = GN[g], nb = NS - na;
					for (int k = 0; k < na; k++) { const double *p = v[GI[g][k]]; a[0] += p[0]; a[1] += p[1]; a[2] += p[2]; }
					double b[3] = {tot[0] - a[0], tot[1] - a[1], tot[2] - a[2]};
					double err = sq;
					if (na) err -= (a[0] * a[0] + a[1] * a[1] + a[2] * a[2]) / na;
					if (nb) err -= (b[0] * b[0] + b[1] * b[1] + b[2] * b[2]) / nb;
					if (err < best - 1e-6) {
						best = err; bestch = G[g].ch;
						for (int k = 0; k < 3; k++) { fa[k] = na ? a[k] / na : 0; fb[k] = nb ? b[k] / nb : 0; }
					}
				}
				lt_color fgc = rgb_color(fa[0], fa[1], fa[2]), bgcol = rgb_color(fb[0], fb[1], fb[2]);
				if (fgc == bgcol || bestch == ' ') lt_put(c, all, X, Y, ' ', LT_STYLE(LT_DEFAULT, bestch == ' ' ? bgcol : fgc, 0));
				else lt_put(c, all, X, Y, bestch, LT_STYLE(fgc, bgcol, 0));
			} else {
				/* unknown ground (terminal default): only the shape's own colour can be named. Fit the shape mask, and produce the
				 * complement orientation of a glyph with reverse video, whose "background" is the terminal's own. */
				double m[3] = {0, 0, 0};
				int cnt = 0;
				for (int s = 0; s < NS; s++) if (al[s] > 0.5) { m[0] += col[s][0]; m[1] += col[s][1]; m[2] += col[s][2]; cnt++; }
				if (!cnt) continue;
				lt_color cc = rgb_color(m[0] / cnt, m[1] / cnt, m[2] / cnt);
				int best = NS + 1, bgi = 0;
				bool rev = false;
				for (int g = 0; g < NG; g++) {
					int inter = 0;
					for (int k = 0; k < GN[g]; k++) if (al[GI[g][k]] > 0.5) inter++;
					int mis = GN[g] + cnt - 2 * inter;                     /* shape xor glyph */
					if (mis < best) { best = mis; bgi = g; rev = false; }
					int mis2 = NS - mis;                                   /* shape xor complement */
					if (mis2 < best) { best = mis2; bgi = g; rev = true; }
				}
				uint32_t ch = G[bgi].ch;
				if (rev && ch == ' ') { lt_put(c, all, X, Y, ' ', LT_STYLE(LT_DEFAULT, cc, 0)); continue; }
				if (!rev && ch == ' ') continue;
				lt_put(c, all, X, Y, ch, LT_STYLE(cc, LT_DEFAULT, rev ? LT_REVERSE : 0));
			}
		}
}

typedef struct { const geom *g; double f[3], o[3]; } gctx;
static double geom_shader(double x, double y, double rgb[3], void *ctx)
{
	gctx *q = ctx;
	int k = classify(q->g, x, y);
	if (!k) return 0;
	const double *s = k == 2 ? q->o : q->f;
	rgb[0] = s[0]; rgb[1] = s[1]; rgb[2] = s[2];
	return 1;
}

static void draw_geom(lt_canvas *c, lt_rect r, const geom *g, lt_color fill, lt_color outline)
{
	gctx q = {g, {200, 200, 200}, {200, 200, 200}};
	int a, b, cc;
	if (lt_color_rgb(fill, &a, &b, &cc)) { q.f[0] = a; q.f[1] = b; q.f[2] = cc; }
	q.o[0] = q.f[0]; q.o[1] = q.f[1]; q.o[2] = q.f[2];
	if (outline != LT_DEFAULT && lt_color_rgb(outline, &a, &b, &cc)) { q.o[0] = a; q.o[1] = b; q.o[2] = cc; }
	lt_shade(c, r, geom_shader, &q);
}

/* ---- public shapes: fit the geometry into the rect, then draw ---- */
void lt_polygon(lt_canvas *c, lt_rect r, const double *xy, int n, lt_color fill, lt_color outline)
{
	if (r.w < 1 || r.h < 1 || n < 3 || !xy) return;
	double PW = r.w, PH = 2.0 * r.h;
	double x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300;
	for (int i = 0; i < n; i++) {
		double x = isfinite(xy[2 * i]) ? xy[2 * i] : 0, y = isfinite(xy[2 * i + 1]) ? xy[2 * i + 1] : 0;
		x0 = fmin(x0, x); x1 = fmax(x1, x); y0 = fmin(y0, y); y1 = fmax(y1, y);
	}
	double bw = x1 - x0, bh = y1 - y0;
	if (bw <= 0 || bh <= 0) return;
	double sc = fmin(PW / bw, PH / bh);
	lt_arena_mark sm_ = lt_arena_save(lt_scratch());
	double *p = lt_arena_alloc(lt_scratch(), (size_t)n * 2 * sizeof *p);
	for (int i = 0; i < n; i++) {
		double x = isfinite(xy[2 * i]) ? xy[2 * i] : 0, y = isfinite(xy[2 * i + 1]) ? xy[2 * i + 1] : 0;
		p[2 * i] = PW / 2.0 + (x - (x0 + x1) / 2.0) * sc;
		p[2 * i + 1] = PH / 2.0 + (y - (y0 + y1) / 2.0) * sc;
	}
	geom g = {false, 0, 0, 0, p, n, outline != LT_DEFAULT ? 1.0 : 0.0};
	draw_geom(c, r, &g, fill, outline);
	lt_arena_restore(lt_scratch(), sm_);
}

void lt_polygon_phys(lt_canvas *c, lt_rect r, const double *xy, int n, lt_color fill, lt_color outline)
{
	if (r.w < 1 || r.h < 1 || n < 3 || n > 256 || !xy) return;
	lt_arena_mark sm_ = lt_arena_save(lt_scratch());
	double *p = lt_arena_alloc(lt_scratch(), (size_t)n * 2 * sizeof *p);
	for (int i = 0; i < 2 * n; i++) p[i] = isfinite(xy[i]) ? xy[i] : 0;
	geom g = {false, 0, 0, 0, p, n, outline != LT_DEFAULT ? 1.0 : 0.0};
	draw_geom(c, r, &g, fill, outline);
	lt_arena_restore(lt_scratch(), sm_);
}

void lt_ngon(lt_canvas *c, lt_rect r, int sides, double rotation_deg, lt_color fill, lt_color outline)
{
	if (sides < 3) return;
	if (sides > 64) sides = 64;
	double xy[128];
	for (int i = 0; i < sides; i++) {
		double a = (rotation_deg + 360.0 * i / sides) * 3.14159265358979323846 / 180.0;
		xy[2 * i] = cos(a); xy[2 * i + 1] = sin(a);
	}
	lt_polygon(c, r, xy, sides, fill, outline);
}

void lt_shape_draw(lt_canvas *c, lt_rect r, lt_shape shape, lt_color fill, lt_color outline)
{
	if (shape == LT_SHAPE_HEXAGON) { lt_ngon(c, r, 6, 0, fill, outline); return; }
	if (shape == LT_SHAPE_OCTAGON) { lt_ngon(c, r, 8, 22.5, fill, outline); return; }
	if (r.w < 1 || r.h < 1) return;
	double PW = r.w, PH = 2.0 * r.h;
	geom g = {true, PW / 2.0, PH / 2.0, fmin(PW, PH) / 2.0, NULL, 0, outline != LT_DEFAULT ? 1.0 : 0.0};
	draw_geom(c, r, &g, fill, outline);
}
void lt_circle(lt_canvas *c, lt_rect r, lt_color fill, lt_color outline) { lt_shape_draw(c, r, LT_SHAPE_CIRCLE, fill, outline); }
void lt_hexagon(lt_canvas *c, lt_rect r, lt_color fill, lt_color outline) { lt_shape_draw(c, r, LT_SHAPE_HEXAGON, fill, outline); }
void lt_octagon(lt_canvas *c, lt_rect r, lt_color fill, lt_color outline) { lt_shape_draw(c, r, LT_SHAPE_OCTAGON, fill, outline); }

/* ---- wireframe: thin lines on braille dots, 2 x 4 per cell (square dots on cells twice as tall as wide) ---- */
static void dot(lt_canvas *c, int dx, int dy, lt_color col)
{
	int cx = dx >> 1, cy = dy >> 2;
	if (dx < 0 || dy < 0 || cx >= lt_canvas_w(c) || cy >= lt_canvas_h(c)) return;
	uint32_t ch = lt_canvas_char(c, cx, cy);
	uint32_t bits = 0;
	if (ch >= 0x2800 && ch <= 0x28FF) bits = ch - 0x2800;
	else if (ch != ' ') return;                                   /* keep glyphs (a filled shape's edge) intact */
	static const uint8_t bit[4][2] = {{0x01, 0x08}, {0x02, 0x10}, {0x04, 0x20}, {0x40, 0x80}};
	bits |= bit[dy & 3][dx & 1];
	lt_rect all = {0, 0, lt_canvas_w(c), lt_canvas_h(c)};
	lt_put(c, all, cx, cy, 0x2800 + bits, LT_STYLE(col, lt_canvas_bg(c, cx, cy), 0));
}

void lt_wire_line(lt_canvas *c, double x0, double y0, double x1, double y1, lt_color col)
{
	if (!isfinite(x0) || !isfinite(y0) || !isfinite(x1) || !isfinite(y1)) return;
	{                                                             /* Liang-Barsky: keep only the part on the canvas (zoomed maps make huge segments) */
		double dx = x1 - x0, dy = y1 - y0, t0 = 0, t1 = 1;
		double p[4] = {-dx, dx, -dy, dy}, q[4] = {x0, lt_canvas_w(c) - x0, y0, lt_canvas_h(c) - y0};
		for (int i = 0; i < 4; i++) {
			if (p[i] == 0) { if (q[i] < 0) return; continue; }
			double t = q[i] / p[i];
			if (p[i] < 0) { if (t > t1) return; if (t > t0) t0 = t; }
			else { if (t < t0) return; if (t < t1) t1 = t; }
		}
		double nx0 = x0 + dx * t0, ny0 = y0 + dy * t0, nx1 = x0 + dx * t1, ny1 = y0 + dy * t1;
		x0 = nx0; y0 = ny0; x1 = nx1; y1 = ny1;
	}
	if (!lt_canvas_caps(c)->unicode) {                            /* no braille: plain characters along the line */
		int steps = (int)fmax(fabs(x1 - x0), fabs(y1 - y0)) + 1;
		if (steps > 4000) return;
		lt_rect all = {0, 0, lt_canvas_w(c), lt_canvas_h(c)};
		for (int i = 0; i <= steps; i++) {
			double t = (double)i / steps;
			lt_put(c, all, (int)floor(x0 + (x1 - x0) * t), (int)floor(y0 + (y1 - y0) * t), fabs(x1 - x0) > 2 * fabs(y1 - y0) ? '-' : fabs(y1 - y0) > 2 * fabs(x1 - x0) ? '|' : '+', LT_STYLE(col, LT_DEFAULT, 0));
		}
		return;
	}
	double ax = x0 * 2, ay = y0 * 4, bx = x1 * 2, by = y1 * 4;
	int steps = (int)ceil(fmax(fabs(bx - ax), fabs(by - ay)));
	if (steps > 20000) return;
	if (steps < 1) steps = 1;
	for (int i = 0; i <= steps; i++) {
		double t = (double)i / steps;
		dot(c, (int)floor(ax + (bx - ax) * t), (int)floor(ay + (by - ay) * t), col);
	}
}

static void poly_points(lt_rect r, const double *xy, int n, double *out)      /* same fit as lt_polygon, in cell coordinates */
{
	double PW = r.w, PH = 2.0 * r.h, x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300;
	for (int i = 0; i < n; i++) { x0 = fmin(x0, xy[2 * i]); x1 = fmax(x1, xy[2 * i]); y0 = fmin(y0, xy[2 * i + 1]); y1 = fmax(y1, xy[2 * i + 1]); }
	double sc = fmin(PW / (x1 - x0), PH / (y1 - y0));
	for (int i = 0; i < n; i++) {
		out[2 * i] = r.x + PW / 2.0 + (xy[2 * i] - (x0 + x1) / 2.0) * sc;
		out[2 * i + 1] = r.y + (PH / 2.0 + (xy[2 * i + 1] - (y0 + y1) / 2.0) * sc) / 2.0;
	}
}

void lt_wire_polygon(lt_canvas *c, lt_rect r, const double *xy, int n, lt_color col)
{
	if (r.w < 1 || r.h < 1 || n < 2 || n > 256 || !xy) return;
	for (int i = 0; i < n; i++) if (!isfinite(xy[2 * i]) || !isfinite(xy[2 * i + 1])) return;
	double p[512];
	poly_points(r, xy, n, p);
	for (int i = 0; i < n; i++) { int j = (i + 1) % n; lt_wire_line(c, p[2 * i], p[2 * i + 1], p[2 * j], p[2 * j + 1], col); }
}

void lt_wire_ngon(lt_canvas *c, lt_rect r, int sides, double rotation_deg, lt_color col)
{
	if (sides < 3 || sides > 64) return;
	double xy[128];
	for (int i = 0; i < sides; i++) { double a = (rotation_deg + 360.0 * i / sides) * 3.14159265358979323846 / 180.0; xy[2 * i] = cos(a); xy[2 * i + 1] = sin(a); }
	lt_wire_polygon(c, r, xy, sides, col);
}

void lt_wire_circle(lt_canvas *c, lt_rect r, lt_color col)
{
	if (r.w < 1 || r.h < 1) return;
	double PW = r.w, PH = 2.0 * r.h, R = fmin(PW, PH) / 2.0;
	int seg = (int)(R * 8) + 16;
	if (seg > 720) seg = 720;
	double cx = r.x + PW / 2.0, cy = r.y + r.h / 2.0;
	for (int i = 0; i < seg; i++) {
		double a0 = 2 * 3.14159265358979323846 * i / seg, a1 = 2 * 3.14159265358979323846 * (i + 1) / seg;
		lt_wire_line(c, cx + R * cos(a0), cy + R * sin(a0) / 2.0, cx + R * cos(a1), cy + R * sin(a1) / 2.0, col);
	}
}
