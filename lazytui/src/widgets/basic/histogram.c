/* histogram.c - buckets a sample into bins and draws them as a barchart.
 * Needs: lazytui.h (the lib core). One widget, one file: copy it into any project that links the core. */
#include "lazytui.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

void lt_histogram(lt_canvas *c, lt_rect r, const lt_theme *t, const double *values, int n, int bins, double lo, double hi, const lt_chart_opts *o)
{
	if (!values || n <= 0 || bins < 1 || bins > 256) return;
	lt_arena_mark m = lt_arena_save(lt_scratch());
	double *cnt = lt_arena_calloc(lt_scratch(), (size_t)bins * sizeof *cnt);
	char **lab = lt_arena_alloc(lt_scratch(), (size_t)bins * sizeof *lab);
	if (!(hi > lo)) { lo = INFINITY; hi = -INFINITY; for (int i = 0; i < n; i++) if (isfinite(values[i])) { if (values[i] < lo) lo = values[i]; if (values[i] > hi) hi = values[i]; } if (!isfinite(lo)) { lo = 0; hi = 1; } if (!(hi > lo)) hi = lo + 1; }
	for (int i = 0; i < n; i++) {
		if (!isfinite(values[i]) || values[i] < lo || values[i] > hi) continue;
		int b = (int)floor((values[i] - lo) / (hi - lo) * bins);
		cnt[b >= bins ? bins - 1 : b]++;
	}
	for (int b = 0; b < bins; b++) { lab[b] = lt_arena_alloc(lt_scratch(), 16); snprintf(lab[b], 16, "%.3g", lo + (hi - lo) * (b + 0.5) / bins); }
	lt_barchart(c, r, t, (const char *const *)lab, cnt, bins, o);
	lt_arena_restore(lt_scratch(), m);
}
