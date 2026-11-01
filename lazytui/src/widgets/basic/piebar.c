/* piebar.c - a segmented bar of slices (widths by largest remainder) with an optional legend.
 * Needs: lazytui.h (the lib core). One widget, one file: copy it into any project that links the core. */
#include "lazytui.h"
#include "legend.h"
#include <math.h>

void lt_piebar(lt_canvas *c, lt_rect r, const lt_theme *t, const lt_slice *s, int n, bool with_legend)
{
	if (r.w < 2 || r.h < 1 || n <= 0 || !s) return;
	double total = slice_total(s, n);
	lt_rect bar = {r.x, r.y, r.w, 1};
	if (total <= 0) { lt_fill(c, bar, ' ', t->s[LT_T_BAR_BG]); return; }
	int *w = lt_arena_calloc(lt_scratch(), (size_t)n * sizeof *w);              /* largest remainder: widths sum to exactly r.w */
	double *rem = lt_arena_calloc(lt_scratch(), (size_t)n * sizeof *rem);
	lt_arena_mark m = lt_arena_save(lt_scratch());
	int used = 0;
	for (int i = 0; i < n; i++) { double q = (isfinite(s[i].value) && s[i].value > 0 ? s[i].value : 0) / total * r.w; w[i] = (int)floor(q); rem[i] = q - w[i]; used += w[i]; }
	while (used < r.w) { int b = 0; for (int i = 1; i < n; i++) if (rem[i] > rem[b]) b = i; w[b]++; rem[b] = -1; used++; }
	int x = r.x;
	for (int i = 0; i < n; i++) { lt_fill(c, (lt_rect){x, r.y, w[i], 1}, ' ', LT_STYLE(LT_DEFAULT, slice_color(t, &s[i], i), 0)); x += w[i]; }
	lt_arena_restore(lt_scratch(), m);
	if (with_legend && r.h > 1) legend(c, (lt_rect){r.x, r.y + 1, r.w, r.h - 1}, t, s, n, total);
}
