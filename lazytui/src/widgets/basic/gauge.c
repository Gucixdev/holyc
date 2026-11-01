/* gauge.c - a 270-degree arc (or a full ring) filled to a value, the number in the middle.
 * Needs: lazytui.h (the lib core). One widget, one file: copy it into any project that links the core. */
#include "lazytui.h"
#include "util.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

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
