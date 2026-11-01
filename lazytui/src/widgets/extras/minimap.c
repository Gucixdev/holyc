/* minimap.c - a page as braille density with the viewport highlighted and marks on the right edge.
 * Needs: lazytui.h (the lib core). One widget, one file: copy it into any project that links the core. */
#include "lazytui.h"
#include <math.h>

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
				long sum = 0;                                                                /* the mean line in this slice: a minimap row is a bar as long as its text */
				for (int k = a; k < b && k < n; k++) sum += density[k];
				int mx = (int)(sum / (b - a));
				for (int h = 0; h < 2; h++) if ((x * 2 + h + 0.5) * 255.0 / (r.w * 2) < mx) bits |= bit[d][h];
			}
			lt_put(c, all, r.x + x, r.y + y, uni ? (uint32_t)(0x2800 + bits) : (bits > 12 ? '#' : bits ? '.' : ' '), st);
		}
		for (int k = 0; marks && k < nmarks; k++) if (marks[k] >= l0 && marks[k] < l1) lt_put(c, all, r.x + r.w - 1, r.y + y, uni ? 0x25C0 : '<', t->s[LT_T_WARN]);
	}
}
