/* corebars.c - lt_corebars: a group of per-core rows, "c0   12.3% 3400MHz 55C ▁▂▃▅", one row per core.
 * Ported from gtop's draw_cpu loop. The fixed text (label, percent, caller's info string) is coloured by load tier (<=60 ok, <=85 warn,
 * above err), the rest of the row is a history graph (lt_histgraph, fixed 0..100) 4..16 cells wide. Rows beyond r.h are cut; `first`
 * scrolls. info and hist may be NULL. Returns how many rows were drawn.
 * Needs: internal.h, histgraph.c. */
#include "internal.h"
#include <stdio.h>

lt_slot lt_load_slot(double pct) { return pct > 85.0 ? LT_T_ERR : pct > 60.0 ? LT_T_WARN : LT_T_OK; }

int lt_corebars(lt_canvas *c, lt_rect r, const lt_theme *t, const lt_core *cores, int n, int first)
{
	lt_rect all = lt_full(c);
	if (r.w < 1 || r.h < 1 || !cores || n < 1) return 0;
	if (first < 0) first = 0;
	int rows = 0;
	for (int i = first; i < n && rows < r.h; i++, rows++) {
		const lt_core *k = &cores[i];
		char fixed[160];
		snprintf(fixed, sizeof fixed, "%-4s%5.1f%% %s%s", k->label ? k->label : "", isfinite(k->pct) ? k->pct : 0.0, k->info ? k->info : "", k->info && *k->info ? " " : "");
		int fw = lt_utf8_width(fixed);
		int gw = r.w - fw;
		if (gw > 16) gw = 16;
		if (gw < 4) { fw = r.w; gw = 0; }                   /* too narrow for a graph: the text takes the row */
		lt_text(c, all, r.x, r.y + rows, fixed, lt_slot_style(t, lt_load_slot(k->pct)), fw);
		if (gw > 0 && k->hist && k->nhist > 0) lt_histgraph(c, (lt_rect){r.x + fw, r.y + rows, gw, 1}, t, k->hist, k->nhist, 100.0, LT_T_ACCENT);
	}
	return rows;
}
