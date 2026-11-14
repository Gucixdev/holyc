/* histgraph.c - lt_histgraph: a history graph STRETCHED to the rectangle's width (linear interpolation between samples), eighth-block
 * resolution over r.h rows, scaled to a fixed ceiling (hi > 0, e.g. 100 for percent) or to its own maximum (hi <= 0, for rates).
 * Ported from gtop's graph_v. Any sample > 0 shows at least the lowest block. Unlike lt_graph (one column per sample, scrolls) this one
 * always fills the width, so a 60-sample history looks the same in 20 or 120 columns. Returns the ceiling used (the auto maximum).
 * Also: lt_human_bytes / lt_human_rate, the "1.5MB" / "1.5MB/s" formatters every monitor needs.
 * Needs: internal.h (the lib core). */
#include "internal.h"
#include <stdio.h>

double lt_histgraph(lt_canvas *c, lt_rect r, const lt_theme *t, const double *v, int n, double hi, lt_slot slot)
{
	lt_rect all = lt_full(c);
	if (r.w < 1 || r.h < 1 || !v || n < 1) return hi > 0 ? hi : 1;
	double mx = hi;
	if (!(mx > 0)) {
		mx = 1;
		for (int i = 0; i < n; i++) if (isfinite(v[i]) && v[i] > mx) mx = v[i];
	}
	static const uint32_t part[9] = {' ', 0x2581, 0x2582, 0x2583, 0x2584, 0x2585, 0x2586, 0x2587, 0x2588};
	bool uni = lt_canvas_caps(c)->unicode;
	for (int x = 0; x < r.w; x++) {
		double tt = (r.w > 1 && n > 1) ? (double)x / (r.w - 1) * (n - 1) : (double)(n - 1);
		int i0 = (int)tt, i1;
		if (i0 > n - 1) i0 = n - 1;
		i1 = i0 + 1 > n - 1 ? n - 1 : i0 + 1;
		double a = isfinite(v[i0]) && v[i0] > 0 ? v[i0] : 0, b = isfinite(v[i1]) && v[i1] > 0 ? v[i1] : 0;
		double val = a * (1 - (tt - i0)) + b * (tt - i0);
		if (!(val > 0)) continue;
		int h8 = (int)ceil(lt_clamp01(val / mx) * r.h * 8);
		if (h8 < 1) h8 = 1;
		lt_style st = lt_slot_style(t, slot);
		for (int row = 0; row < r.h; row++) {
			int below = row * 8;
			if (h8 <= below) break;
			int here = h8 - below > 8 ? 8 : h8 - below;
			lt_put(c, all, r.x + x, r.y + r.h - 1 - row, uni ? part[here] : (here >= 5 ? '#' : '.'), st);
		}
	}
	return mx;
}

void lt_human_bytes(double v, char *out, size_t n)
{
	static const char *u[] = {"B", "KB", "MB", "GB", "TB"};
	int i = 0;
	if (!(v >= 0)) v = 0;
	while (v >= 1024.0 && i < 4) { v /= 1024.0; i++; }
	if (i == 0) snprintf(out, n, "%.0f%s", v, u[i]); else snprintf(out, n, "%.1f%s", v, u[i]);
}
void lt_human_rate(double v, char *out, size_t n)
{
	char b[32];
	lt_human_bytes(v, b, sizeof b);
	snprintf(out, n, "%.24s/s", b);
}
