/* progressbar.c - Progressbar: a labelled bar filled 0..1, eighth-block resolution, follows the theme gradient when it has one
 * Needs: lazytui.h (the lib core). One widget, one file: copy it into any project that links the core. */
#include "internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* label on the left, percent on the right, bar in between: returns the bar's rectangle */
static lt_rect frame_bar(lt_canvas *c, lt_rect r, const lt_theme *t, double v, const char *label, bool percent)
{
	lt_rect all = lt_full(c);
	r = lt_one_row(r);
	int left = 0, right = 0;
	if (label && *label) {
		int lw = lt_utf8_width(label), maxl = r.w / 3;
		if (lw > maxl) lw = maxl;
		left = lt_text(c, all, r.x, r.y, label, lt_slot_style(t, LT_T_LABEL), lw) + 1;
	}
	if (percent) {
		char b[16];
		snprintf(b, sizeof b, "%3d%%", (int)floor(lt_clamp01(v) * 100 + 0.5));
		right = 5;
		if (r.w - left - right >= 6) lt_text(c, all, r.x + r.w - 4, r.y, b, lt_slot_style(t, LT_T_TEXT), 4);
		else right = 0;
	}
	lt_rect bar = {r.x + left, r.y, r.w - left - right, 1};
	if (bar.w < 1) bar.w = 1;
	return bar;
}

void lt_progressbar(lt_canvas *c, lt_rect r, const lt_theme *t, double v, const char *label, bool percent)
{
	lt_rect all = lt_full(c), b = frame_bar(c, r, t, v, label, percent);
	v = lt_clamp01(v);
	lt_style on = LT_STYLE(lt_slot_style(t, LT_T_BAR).fg, lt_slot_style(t, LT_T_BAR_BG).bg, lt_slot_style(t, LT_T_BAR).attr);
	lt_style off = LT_STYLE(lt_slot_style(t, LT_T_BAR).fg, lt_slot_style(t, LT_T_BAR_BG).bg, lt_slot_style(t, LT_T_BAR_BG).attr);
	if (!lt_canvas_caps(c)->unicode) {
		if (b.w < 4) { lt_fill(c, b, v >= 1 ? '#' : '-', on); return; }
		int inner = b.w - 2, fill = (int)floor(v * inner);
		lt_put(c, all, b.x, b.y, '[', off);
		for (int i = 0; i < inner; i++) lt_put(c, all, b.x + 1 + i, b.y, i < fill ? '=' : (i == fill && v < 1 ? '>' : ' '), i <= fill && v > 0 ? on : off);
		lt_put(c, all, b.x + b.w - 1, b.y, ']', off);
		return;
	}
	static const uint32_t eighths[9] = {' ', 0x258F, 0x258E, 0x258D, 0x258C, 0x258B, 0x258A, 0x2589, 0x2588};
	double cells = v * b.w;
	int whole_cells = (int)floor(cells), eighth = (int)floor((cells - whole_cells) * 8 + 0.5);
	if (eighth == 8) { whole_cells++; eighth = 0; }
	for (int i = 0; i < b.w; i++) {
		lt_style g = on;
		if (t->ngrad > 0) g.fg = lt_theme_ramp(t, b.w > 1 ? (double)i / (b.w - 1) : 0);          /* hue follows the position along the bar */
		if (i < whole_cells) lt_put(c, all, b.x + i, b.y, 0x2588, g);
		else if (i == whole_cells && eighth > 0) lt_put(c, all, b.x + i, b.y, eighths[eighth], g);
		else lt_put(c, all, b.x + i, b.y, ' ', off);
	}
}
