/* bench.c - milliseconds per draw of the heavy widgets and of a whole frame; run by hand (make bench), not part of make test. */
#include "lazytui.h"
#include <stdio.h>
#include <stdlib.h>

static lt_caps UNI = {true, true, false, true, 256};
static lt_canvas *C;
static lt_theme TH;

#define BENCH(name, n, stmt) do { double t0 = lt_now(); for (int i_ = 0; i_ < (n); i_++) { stmt; } printf("%-34s %8.3f ms\n", name, (lt_now() - t0) * 1000.0 / (n)); } while (0)

int main(void)
{
	TH = lt_theme_default();
	C = lt_canvas_new(110, 36, UNI);
	lt_rect r = {0, 0, 110, 36}, small = {0, 0, 54, 24};
	lt_map_opts o = {0};
	BENCH("worldmap climate 110x36", 10, lt_worldmap_ex(C, r, &o));
	o.flags = LT_MAP_BORDERS | LT_MAP_LABELS;
	BENCH("worldmap borders+labels", 10, lt_worldmap_ex(C, r, &o));
	o.flags = LT_MAP_WIRE | LT_MAP_GRID;
	BENCH("worldmap wire", 10, lt_worldmap_ex(C, r, &o));
	o.flags = 0; o.lon0 = 10;
	BENCH("globe 54x24", 10, lt_globe_ex(C, small, &o));
	lt_axis ax[6] = {{"a", 1}, {"b", 2}, {"c", 3}, {"d", 4}, {"e", 5}, {"f", 6}};
	BENCH("webchart 54x24", 20, lt_webchart(C, small, &TH, ax, 6, 6, 4, LT_DEFAULT));
	BENCH("radar 54x24", 20, lt_radar(C, small, &TH, 40, NULL, 0, LT_DEFAULT));
	BENCH("rain 110x36", 50, lt_rain(C, r, &TH, 1.5, 7));
	char *d = NULL;
	BENCH("canvas clear + dump 110x36", 200, { lt_canvas_clear(C, TH.s[LT_T_TEXT]); d = lt_canvas_dump(C); free(d); });
	lt_canvas *P = lt_canvas_new(110, 36, UNI);
	lt_canvas_copy(P, C);
	lt_rain(C, r, &TH, 1.5, 7);
	size_t n;
	BENCH("canvas diff (rain vs blank)", 200, { char *s = lt_canvas_diff(C, P, 0, &n); free(s); });
	return 0;
}
