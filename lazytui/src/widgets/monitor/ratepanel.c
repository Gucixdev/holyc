/* ratepanel.c - lt_ratepanel: a disk / network rate panel. One row per interface or device: "eth0  ↓ 1.2MB/s  ↑ 40KB/s  ▁▂▅" would need
 * two series, so a row here is ONE rate (give "eth0 rx" and "eth0 tx" as two rows): label, human rate, optional peak, and a rate history
 * graph that auto-scales to its own maximum (lt_histgraph with hi 0). From gtop's draw_disk / draw_net. With r.h >= 2*rows each row gets
 * two lines and a taller graph. Returns the rows drawn.
 * Needs: internal.h, histgraph.c. */
#include "internal.h"
#include <stdio.h>

int lt_ratepanel(lt_canvas *c, lt_rect r, const lt_theme *t, const lt_rate *rows, int n)
{
	lt_rect all = lt_full(c);
	if (r.w < 1 || r.h < 1 || !rows || n < 1) return 0;
	int lw = 0;
	for (int i = 0; i < n; i++) if (rows[i].label) { int w = lt_utf8_width(rows[i].label); if (w > lw) lw = w; }
	if (lw > r.w / 3) lw = r.w / 3;
	int per = r.h >= 2 * n ? 2 : 1, drawn = 0;
	for (int i = 0; i < n && (drawn + 1) * per <= r.h; i++, drawn++) {
		const lt_rate *q = &rows[i];
		int y = r.y + drawn * per;
		char rate[40], line[120];
		lt_human_rate(isfinite(q->rate) ? q->rate : 0, rate, sizeof rate);
		int x = r.x;
		x += lt_text(c, all, x, y, q->label ? q->label : "", lt_slot_style(t, LT_T_LABEL), lw);
		x = r.x + lw + 1;
		snprintf(line, sizeof line, "%9s", rate);
		x += lt_text(c, all, x, y, line, lt_slot_style(t, LT_T_TEXT), r.x + r.w - x);
		if (q->peak > 0 && r.x + r.w - x > 14) {
			char pk[40];
			lt_human_rate(q->peak, pk, sizeof pk);
			snprintf(line, sizeof line, " pk %s", pk);
			x += lt_text(c, all, x, y, line, lt_slot_style(t, LT_T_DIM), r.x + r.w - x);
		}
		x++;
		lt_rect g = per == 2 ? (lt_rect){r.x + lw + 1, y + 1, r.w - lw - 1, 1} : (lt_rect){x, y, r.x + r.w - x, 1};
		if (g.w > 0 && q->hist && q->nhist > 0) lt_histgraph(c, g, t, q->hist, q->nhist, 0, LT_T_ACCENT);
	}
	return drawn;
}
