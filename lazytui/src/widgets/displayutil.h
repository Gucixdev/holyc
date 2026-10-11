/* displayutil.h - tiny helpers shared by two or more of the display widgets (spinner.c, chart.c: gauge/barchart): a colour that falls back
 * from the theme's gradient to a slot, and the ground colour under text drawn on a shaded cell. Private, not part of the public API. */
#ifndef LT_DISPLAYUTIL_H
#define LT_DISPLAYUTIL_H
#include "lazytui.h"

static inline lt_color ramp_or(const lt_theme *t, double f, lt_slot fallback)
{
	lt_color c = lt_theme_ramp(t, f);
	return c != LT_DEFAULT ? c : t->s[fallback].fg;
}
/* the ground colour under text drawn on a shaded cell */
static inline lt_style over(const lt_canvas *c, int x, int y, lt_color fg, uint8_t attr) { return LT_STYLE(fg, lt_canvas_bg(c, x, y), attr); }
static inline unsigned hash32(unsigned a, unsigned b) { unsigned h = a * 374761393u + b * 668265263u; h = (h ^ (h >> 13)) * 1274126177u; return h ^ (h >> 16); }
#endif
