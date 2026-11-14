/* heatgrid.c - a GitHub-contributions-style grid of dots, growing and brightening with the value.
 * Needs: lazytui.h (the lib core). One widget, one file: copy it into any project that links the core. */
#include "lazytui.h"
#include "util.h"
#include <math.h>

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
