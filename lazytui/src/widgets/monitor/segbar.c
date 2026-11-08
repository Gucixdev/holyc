/* segbar.c - lt_segbar: ONE row, a stacked bar of segments (cpu user/sys/iowait, memory used/cache/free) plus a text suffix.
 * Ported from gtop's segmented_bar: each segment is round(width*value/total) cells in its slot's colour, when the values add up to `total` (within 0.5%) the LAST
 * segment takes whatever is left, so rounding never leaves a gap or overflows; if they fall short, the rest stays unfilled, and a light shade fills the part no segment covers. total <= 0 = an empty bar.
 * The suffix (" 42% L1.2", " 3.1GB/16GB") is right of the bar; it is dropped when it would leave the bar under 4 cells.
 * Needs: internal.h (the lib core). */
#include "internal.h"

void lt_segbar(lt_canvas *c, lt_rect r, const lt_theme *t, const double *val, const lt_slot *slots, int n, double total, const char *suffix)
{
	lt_rect all = lt_full(c);
	r = lt_one_row(r);
	if (r.w < 1 || n < 1 || !val || !slots) return;
	int sw = suffix ? lt_utf8_width(suffix) : 0;
	if (r.w - sw < 4) sw = 0;
	int bw = r.w - sw, used = 0;
	bool uni = lt_canvas_caps(c)->unicode;
	double sum = 0;
	for (int i = 0; i < n; i++) if (isfinite(val[i]) && val[i] > 0) sum += val[i];
	bool full = total > 0 && fabs(sum - total) <= total * 0.005;
	if (total > 0) {
		for (int i = 0; i < n && used < bw; i++) {
			double f = isfinite(val[i]) && val[i] > 0 ? val[i] : 0;
			int w = (int)floor(bw * f / total + 0.5);
			if (i == n - 1 && full) w = bw - used;
			if (w > bw - used) w = bw - used;
			lt_style st = lt_slot_style(t, slots[i]);
			for (int k = 0; k < w; k++) lt_put(c, all, r.x + used + k, r.y, uni ? 0x2588 : '#', st);
			used += w;
		}
	}
	lt_style dim = lt_slot_style(t, LT_T_DIM);
	for (int k = used; k < bw; k++) lt_put(c, all, r.x + k, r.y, total > 0 ? (uni ? 0x2591 : '.') : ' ', dim);
	if (sw) lt_text(c, all, r.x + bw, r.y, suffix, lt_slot_style(t, LT_T_TEXT), sw);
}
