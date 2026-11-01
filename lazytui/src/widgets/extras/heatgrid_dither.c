/* heatgrid_dither.c - the heat grid with every cell an ordered-dither patch of braille dots (ink density = value).
 * Needs: lazytui.h (the lib core). One widget, one file: copy it into any project that links the core. */
#include "lazytui.h"
#include "util.h"
#include <math.h>

void lt_heatgrid_dither(lt_canvas *c, lt_rect r, const lt_theme *t, const double *v, int cols, int rows, double lo, double hi, int cell_w, const char *const *row_labels)
{
	if (r.w < 2 || r.h < 1 || !v || cols < 1 || rows < 1) return;
	lt_rect all = {0, 0, lt_canvas_w(c), lt_canvas_h(c)};
	bool uni = lt_canvas_caps(c)->unicode;
	static const uint8_t bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};      /* the 4x4 ordered-dither threshold matrix */
	static const uint8_t bit[4][2] = {{0x01, 0x08}, {0x02, 0x10}, {0x04, 0x20}, {0x40, 0x80}};
	if (cell_w < 1) cell_w = 1;
	if (cell_w > 4) cell_w = 4;
	int lw = 0;
	for (int y = 0; row_labels && y < rows; y++) if (row_labels[y]) { int w = lt_utf8_width(row_labels[y]); if (w > lw) lw = w; }
	if (lw) lw++;
	for (int y = 0; y < rows && y < r.h; y++) {
		if (row_labels && row_labels[y]) lt_text(c, all, r.x, r.y + y, row_labels[y], t->s[LT_T_DIM], lw);
		for (int x = 0; x < cols; x++) {
			int cx = r.x + lw + x * (cell_w + 1);
			if (cx + cell_w > r.x + r.w) break;
			double val = v[y * cols + x], f = hi > lo && isfinite(val) ? clamp01((val - lo) / (hi - lo)) : 0;
			lt_color col = t->ngrad > 0 ? lt_theme_ramp(t, f) : lt_color_mix(t->s[LT_T_DIM].fg, t->s[LT_T_ACCENT].fg, f);
			if (col == LT_DEFAULT) col = LT_PAL(f > 0.5 ? 2 : 8);
			lt_style st = LT_STYLE(f > 0 ? col : t->s[LT_T_DIM].fg, LT_DEFAULT, 0);
			for (int k = 0; k < cell_w; k++) {
				int bits = 0;
				for (int d = 0; d < 4; d++)
					for (int h = 0; h < 2; h++)
						if (f * 16.0 > bayer[d][(k * 2 + h) & 3] && f > 0) { bits |= bit[d][h]; }
				static const char ramp[] = " .:-=+*#%@";
				lt_put(c, all, cx + k, r.y + y, uni ? (uint32_t)(0x2800 + bits) : (uint32_t)ramp[(int)(f * 9.0 + 0.5)], st);
			}
		}
	}
}
