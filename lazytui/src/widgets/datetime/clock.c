/* clock.c - Lt_clock: wall time in three forms, purely a function of "when"
 * Needs: lazytui.h (the lib core). One widget, one file: copy it into any project that links the core. */
#include "internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void draw_digital(lt_canvas *c, lt_rect r, const lt_theme *t, struct tm when, bool seconds)
{
	lt_rect all = lt_full(c);
	char buf[16];
	if (seconds) snprintf(buf, sizeof buf, "%02d:%02d:%02d", when.tm_hour, when.tm_min, when.tm_sec);
	else snprintf(buf, sizeof buf, "%02d:%02d", when.tm_hour, when.tm_min);
	int w = (int)strlen(buf), x = r.x + (r.w - w) / 2, y = r.y + r.h / 2;
	if (x < r.x) x = r.x;
	lt_text(c, all, x, y, buf, LT_STYLE(t->s[LT_T_ACCENT].fg, LT_DEFAULT, LT_BOLD), r.w);
}

/* a plain 3-wide x 5-tall block font, '#' = lit; the colon is its own 1-wide glyph */
static const char *const BIGDIGIT[10][5] = {
	{"###", "# #", "# #", "# #", "###"}, {"  #", "  #", "  #", "  #", "  #"}, {"###", "  #", "###", "#  ", "###"},
	{"###", "  #", "###", "  #", "###"}, {"# #", "# #", "###", "  #", "  #"}, {"###", "#  ", "###", "  #", "###"},
	{"###", "#  ", "###", "# #", "###"}, {"###", "  #", "  #", "  #", "  #"}, {"###", "# #", "###", "# #", "###"},
	{"###", "# #", "###", "  #", "###"},
};
static const char *const BIGCOLON[5] = {" ", "#", " ", "#", " "};

static void draw_big(lt_canvas *c, lt_rect r, const lt_theme *t, struct tm when, bool seconds)
{
	lt_rect all = lt_full(c);
	bool uni = lt_canvas_caps(c)->unicode;
	int digit[9], nd = 0;
	digit[nd++] = when.tm_hour / 10; digit[nd++] = when.tm_hour % 10; digit[nd++] = -1;
	digit[nd++] = when.tm_min / 10; digit[nd++] = when.tm_min % 10;
	if (seconds) { digit[nd++] = -1; digit[nd++] = when.tm_sec / 10; digit[nd++] = when.tm_sec % 10; }
	int gw = -1;
	for (int i = 0; i < nd; i++) gw += (digit[i] < 0 ? 1 : 3) + 1;
	if (gw > r.w) { draw_digital(c, r, t, when, seconds); return; }        /* too narrow for the big font: fall back */
	int ox = r.x + (r.w - gw) / 2, oy = r.y + (r.h - 5) / 2;
	if (ox < r.x) ox = r.x;
	if (oy < r.y) oy = r.y;
	lt_color ink = t->s[LT_T_ACCENT].fg;
	uint32_t on = uni ? 0x2588 : '#';
	int x = ox;
	for (int i = 0; i < nd; i++) {
		int dw = digit[i] < 0 ? 1 : 3;
		const char *const *rows = digit[i] < 0 ? BIGCOLON : BIGDIGIT[digit[i]];
		for (int row = 0; row < 5 && oy + row < r.y + r.h; row++)
			for (int col = 0; col < dw; col++)
				if (rows[row][col] == '#') lt_put(c, all, x + col, oy + row, on, LT_STYLE(ink, LT_DEFAULT, LT_BOLD));
		x += dw + 1;
	}
}

static void polar_pt(double cx, double cy, double R, double deg, double *x, double *y)
{
	double a = deg * M_PI / 180.0;
	*x = cx + sin(a) * R;
	*y = cy - cos(a) * R;
}

static void wire_ring(lt_canvas *c, double ox, double oy, double cx, double cy, double R, lt_color col)
{
	int seg = (int)(R * 6) + 20;
	if (seg > 720) seg = 720;
	for (int i = 0; i < seg; i++) {
		double a0 = 2 * M_PI * i / seg, a1 = 2 * M_PI * (i + 1) / seg;
		lt_wire_line(c, ox + cx + R * cos(a0), oy + (cy + R * sin(a0)) / 2.0, ox + cx + R * cos(a1), oy + (cy + R * sin(a1)) / 2.0, col);
	}
}

static void draw_analog(lt_canvas *c, lt_rect r, const lt_theme *t, struct tm when)
{
	if (r.w < 5 || r.h < 3) return;
	lt_rect all = lt_full(c);
	bool uni = lt_canvas_caps(c)->unicode;
	double PW = r.w, PH = 2.0 * r.h, R = fmin(PW, PH) / 2.0 - 0.5, cx = PW / 2.0, cy = PH / 2.0;
	if (R < 1) return;
	lt_color face = t->s[LT_T_BORDER].fg, accent = t->s[LT_T_ACCENT].fg;
	wire_ring(c, r.x, r.y, cx, cy, R, face);
	for (int h = 0; h < 12; h += 3) {
		double x, y;
		polar_pt(cx, cy, R + (R > 4 ? 1.1 : 0.6), h * 30.0, &x, &y);
		char lab[3];
		snprintf(lab, sizeof lab, "%d", h == 0 ? 12 : h);
		int px = r.x + (int)floor(x + 0.5) - (int)strlen(lab) / 2, py = r.y + (int)floor(y / 2.0 + 0.5);
		lt_text(c, all, px, py, lab, t->s[LT_T_DIM], 2);
	}
	double hour_deg = fmod(when.tm_hour, 12) * 30.0 + when.tm_min / 2.0;
	double min_deg = when.tm_min * 6.0 + when.tm_sec / 10.0;
	double sec_deg = when.tm_sec * 6.0;
	double x, y;
	polar_pt(cx, cy, R * 0.5, hour_deg, &x, &y);
	lt_wire_line(c, r.x + cx, r.y + cy / 2.0, r.x + x, r.y + y / 2.0, accent);
	polar_pt(cx, cy, R * 0.8, min_deg, &x, &y);
	lt_wire_line(c, r.x + cx, r.y + cy / 2.0, r.x + x, r.y + y / 2.0, t->s[LT_T_TEXT].fg);
	polar_pt(cx, cy, R * 0.92, sec_deg, &x, &y);
	lt_wire_line(c, r.x + cx, r.y + cy / 2.0, r.x + x, r.y + y / 2.0, t->s[LT_T_ERR].fg);
	lt_put(c, all, r.x + (int)floor(cx + 0.5), r.y + (int)floor(cy / 2.0 + 0.5), uni ? 0x25CF : 'o', LT_STYLE(accent, LT_DEFAULT, LT_BOLD));
}

void lt_clock_draw(lt_canvas *c, lt_rect r, const lt_theme *t, lt_clock_kind kind, struct tm when, bool show_seconds)
{
	if (r.w < 1 || r.h < 1) return;
	if (kind == LT_CLOCK_ANALOG) { draw_analog(c, r, t, when); return; }
	if (kind == LT_CLOCK_BIG && r.h >= 5) { draw_big(c, r, t, when, show_seconds); return; }
	draw_digital(c, r, t, when, show_seconds);
}
