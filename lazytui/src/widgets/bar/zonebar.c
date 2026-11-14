/* zonebar.c - Zonebar: a bar split into weighted zones (used/cache/free style), with an optional piebar-style legend below
 * Needs: lazytui.h (the lib core). One widget, one file: copy it into any project that links the core. */
#include "internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* black or white text, whichever reads better on `bg` */
static lt_color ink_for(lt_color bg)
{
	if ((bg >> 24) == 2) return (0.299 * ((bg >> 16) & 255) + 0.587 * ((bg >> 8) & 255) + 0.114 * (bg & 255)) > 140 ? LT_RGB(0, 0, 0) : LT_RGB(255, 255, 255);
	if ((bg >> 24) == 1) { unsigned n = bg & 255; return (n == 0 || n == 1 || n == 4 || n == 5 || n == 8) ? LT_PAL(15) : LT_PAL(0); }
	return LT_PAL(0);
}

/* draws just the bar row (r is collapsed to one row); returns the total weight, so lt_zonebar_ex's legend can compute percentages
 * without summing twice, and 0 when there is nothing to show (the caller then paints an empty track). */
static double bar_row(lt_canvas *c, lt_rect r, const lt_theme *t, const lt_zone *z, int n)
{
	if (n <= 0 || r.w <= 0) return 0;
	double total = 0;
	for (int i = 0; i < n; i++) total += z[i].weight > 0 ? z[i].weight : 0;
	if (total <= 0) { lt_fill(c, r, ' ', lt_slot_style(t, LT_T_BAR_BG)); return 0; }
	/* largest remainder: integer widths that sum to exactly r.w */
	lt_arena_mark sm_ = lt_arena_save(lt_scratch());
	int *w = lt_arena_calloc(lt_scratch(), (size_t)n * sizeof *w);
	double *rem = lt_arena_calloc(lt_scratch(), (size_t)n * sizeof *rem);
	int used = 0;
	for (int i = 0; i < n; i++) {
		double q = (z[i].weight > 0 ? z[i].weight : 0) / total * r.w;
		w[i] = (int)floor(q); rem[i] = q - w[i]; used += w[i];
	}
	while (used < r.w) {
		int best = 0;
		for (int i = 1; i < n; i++) if (rem[i] > rem[best]) best = i;
		w[best]++; rem[best] = -1; used++;
	}
	int x = r.x;
	for (int i = 0; i < n; i++) {
		lt_style base = lt_slot_style(t, z[i].colour);
		lt_style st = base.fg == LT_DEFAULT ? LT_STYLE(LT_DEFAULT, LT_DEFAULT, LT_REVERSE) : LT_STYLE(ink_for(base.fg), base.fg, LT_BOLD);
		lt_fill(c, (lt_rect){x, r.y, w[i], 1}, ' ', st);        /* pure colour, no label text - a legend (lt_zonebar_ex) is what names a zone */
		x += w[i];
	}
	lt_arena_restore(lt_scratch(), sm_);
	return total;
}

void lt_zonebar(lt_canvas *c, lt_rect r, const lt_theme *t, const lt_zone *z, int n) { bar_row(c, lt_one_row(r), t, z, n); }

/* the bar, then (with_legend) one compact line below it: a coloured square, its label and its percentage, per zone, side by side -
 * everything the old stacked per-row legend showed, just laid out horizontally instead of one row per zone. A zone that doesn't fit
 * the remaining width is simply not reached (nothing wraps or overlaps). */
void lt_zonebar_ex(lt_canvas *c, lt_rect r, const lt_theme *t, const lt_zone *z, int n, bool with_legend)
{
	if (n <= 0 || r.w <= 0 || r.h <= 0) return;
	lt_rect bar = {r.x, r.y, r.w, 1};
	double total = bar_row(c, bar, t, z, n);
	if (!with_legend || r.h < 2) return;
	lt_rect all = lt_full(c);
	bool uni = lt_canvas_caps(c)->unicode;
	int y = r.y + 1, x = r.x, end = r.x + r.w;
	for (int i = 0; i < n && x < end; i++) {
		lt_style base = lt_slot_style(t, z[i].colour);
		lt_color sq = base.fg != LT_DEFAULT ? base.fg : t->s[LT_T_TEXT].fg;
		lt_put(c, all, x, y, uni ? 0x25A0 : '#', LT_STYLE(sq, LT_DEFAULT, 0));
		x += 2;
		if (x >= end) break;
		double w = z[i].weight > 0 ? z[i].weight : 0;
		int pct = total > 0 ? (int)floor(w / total * 100 + 0.5) : 0;
		char seg[40];
		if (z[i].label) snprintf(seg, sizeof seg, "%s %d%%", z[i].label, pct);
		else snprintf(seg, sizeof seg, "%d%%", pct);
		int sw = lt_utf8_width(seg);
		if (x + sw > end) sw = end - x;                                    /* truncate rather than overlap the next swatch */
		lt_text(c, all, x, y, seg, t->s[LT_T_TEXT], sw);
		x += sw + 2;                                                       /* a gap before the next swatch */
	}
}
