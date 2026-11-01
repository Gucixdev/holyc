/* draw/legend.h - what piebar and piechart share: the sum of the slices, a slice's colour, and the "# label  42%" legend. Private. */
#ifndef LT_LEGEND_H
#define LT_LEGEND_H
#include "lazytui.h"
#include "util.h"
#include <math.h>
#include <stdio.h>

static inline double slice_total(const lt_slice *s, int n) { double t = 0; for (int i = 0; s && i < n; i++) if (isfinite(s[i].value) && s[i].value > 0) t += s[i].value; return t; }
static inline lt_color slice_color(const lt_theme *t, const lt_slice *s, int i) { return s->color != LT_DEFAULT ? s->color : lt_theme_cat(t, i); }

static inline void legend(lt_canvas *c, lt_rect r, const lt_theme *t, const lt_slice *s, int n, double total)
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
#endif
