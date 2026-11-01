/* radar.c - ship radar: range rings, bearing ticks, a sweep line and the blips it lights up.
 * Needs: lazytui.h (the lib core). One widget, one file: copy it into any project that links the core. */
#include "lazytui.h"
#include "util.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

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
