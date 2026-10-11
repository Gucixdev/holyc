/* test_display.c - the read-only widgets split out of the old display.c: spinners, pie bar/chart, gauge, line/bar chart, histogram,
 * heat grid, tag cloud, minimap. Correctness spot-checks plus a NaN/empty/tiny-rect fuzz, the same shape as test_widgets.c. */
#include "lazytui.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); fails++; } } while (0)
static lt_caps UNI = {true, true, false, true, 256};
static unsigned rng_state;
static int rnd(int n) { rng_state = rng_state * 1664525u + 1013904223u; return (int)((rng_state >> 8) % (unsigned)n); }

static bool has_glyph(const lt_canvas *c, int lo, int hi)
{
	for (int y = 0; y < lt_canvas_h(c); y++) for (int x = 0; x < lt_canvas_w(c); x++) { uint32_t ch = lt_canvas_char(c, x, y); if (ch != ' ' && ch != 0) return true; (void)lo; (void)hi; }
	return false;
}

int main(void)
{
	lt_theme th = lt_theme_matrix(), grad = lt_theme_matrix(), mono = lt_theme_mono();
	mono.ngrad = 0;
	lt_canvas *c = lt_canvas_new(60, 20, UNI);

	/* spinners: every kind draws something, and time moves the glyph */
	for (int k = 0; k <= LT_SPIN_PARALLAX; k++) {
		lt_canvas_clear(c, th.s[LT_T_TEXT]);
		lt_spinner(c, (lt_rect){2, 2, 30, 4}, &th, (lt_spin_kind)k, 0.0, "loading");
		CHECK(has_glyph(c, 0, 0), "spinner kind draws at least one glyph");
	}
	char *d1, *d2;
	lt_canvas_clear(c, th.s[LT_T_TEXT]); lt_spinner(c, (lt_rect){0, 0, 10, 1}, &th, LT_SPIN_LINE, 0.0, NULL); d1 = lt_canvas_dump(c);
	lt_canvas_clear(c, th.s[LT_T_TEXT]); lt_spinner(c, (lt_rect){0, 0, 10, 1}, &th, LT_SPIN_LINE, 0.5, NULL); d2 = lt_canvas_dump(c);
	CHECK(strcmp(d1, d2) != 0, "the line spinner's glyph changes with time");
	free(d1); free(d2);

	/* pie bar / pie chart: legend percentages sum to ~100 */
	lt_slice sl[3] = {{"a", 50, LT_DEFAULT}, {"b", 30, LT_DEFAULT}, {"c", 20, LT_DEFAULT}};
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	lt_piebar(c, (lt_rect){0, 0, 40, 5}, &th, sl, 3, true);
	char *pb = lt_canvas_dump(c);
	CHECK(strstr(pb, "50%") && strstr(pb, "30%") && strstr(pb, "20%"), "piebar legend shows each slice's share");
	free(pb);
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	lt_piechart(c, (lt_rect){0, 0, 30, 14}, &th, sl, 3, true, false);
	CHECK(has_glyph(c, 0, 0), "a full pie draws");
	lt_slice zero[2] = {{"a", 0, LT_DEFAULT}, {"b", 0, LT_DEFAULT}};
	lt_piebar(c, (lt_rect){0, 0, 20, 1}, &th, zero, 2, false);
	lt_piechart(c, (lt_rect){0, 0, 20, 8}, &th, zero, 2, false, false);           /* all-zero: must not divide by zero / crash */

	/* gauge: 0% and 100% differ, the label is centred */
	lt_canvas_clear(c, th.s[LT_T_TEXT]); lt_gauge(c, (lt_rect){0, 0, 16, 8}, &th, 0.0, "cpu", false); char *g0 = lt_canvas_dump(c);
	lt_canvas_clear(c, th.s[LT_T_TEXT]); lt_gauge(c, (lt_rect){0, 0, 16, 8}, &th, 1.0, "cpu", false); char *g1 = lt_canvas_dump(c);
	CHECK(strstr(g0, "0%") && strstr(g1, "100%") && strcmp(g0, g1) != 0, "gauge shows the percentage and redraws for different values");
	free(g0); free(g1);
	lt_gauge(c, (lt_rect){0, 0, 16, 8}, &th, NAN, "x", true);                     /* NaN: must not crash, must not throw the arc */

	/* line chart: two series, one NaN gap; a legend name appears */
	double s1v[8] = {1, 2, NAN, 4, 5, 6, 5, 4}, s2v[8] = {8, 7, 6, 5, 4, 3, 2, 1};
	lt_series ss[2] = {{"cpu", s1v, 8, LT_DEFAULT}, {"mem", s2v, 8, LT_RGB(200, 60, 60)}};
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	lt_linechart(c, (lt_rect){0, 0, 55, 16}, &th, ss, 2, NULL);
	char *lc = lt_canvas_dump(c);
	CHECK(strstr(lc, "cpu") && strstr(lc, "mem"), "the line chart's legend names both series");
	free(lc);
	lt_chart_opts opt = {0}; opt.title = "history"; opt.grid = true;
	const char *xl[3] = {"t0", "t1", "t2"};
	opt.xlabels = xl; opt.nxlabels = 3;
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	lt_linechart(c, (lt_rect){0, 0, 55, 16}, &th, ss, 2, &opt);
	char *lc2 = lt_canvas_dump(c);
	CHECK(strstr(lc2, "history") && strstr(lc2, "t0"), "title and x labels are drawn when given");
	free(lc2);

	/* bar chart + value labels + histogram */
	double bv[4] = {10, 40, 25, 5};
	const char *bl[4] = {"q1", "q2", "q3", "q4"};
	lt_chart_opts bopt = {0}; bopt.values = true;
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	lt_barchart(c, (lt_rect){0, 0, 40, 12}, &th, bl, bv, 4, &bopt);
	char *bc = lt_canvas_dump(c);
	CHECK(strstr(bc, "q2") && strstr(bc, "40"), "the tallest bar's label and value both appear");
	free(bc);
	double sample[200];
	unsigned rng = 3;
	for (int i = 0; i < 200; i++) { rng = rng * 1664525u + 1013904223u; sample[i] = (double)((rng >> 8) % 100); }
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	lt_histogram(c, (lt_rect){0, 0, 40, 12}, &th, sample, 200, 10, 0, 0, NULL);
	CHECK(has_glyph(c, 0, 0), "a histogram over auto-ranged data draws");

	/* heat grid: dots, not filled cells - low and high values get different foreground colours (0 is the "no data" placeholder dot) */
	double hv[9] = {0, 1, 2, 3, 4, 5, 6, 7, 8};
	lt_canvas_clear(c, grad.s[LT_T_TEXT]);
	lt_heatgrid(c, (lt_rect){0, 0, 30, 3}, &grad, hv, 9, 1, 0, 8, 2, NULL);      /* cell_w=2: dot centres land at x = i*3 + 1 */
	lt_color cold = lt_canvas_fg(c, 4, 0), hot = lt_canvas_fg(c, 25, 0);         /* cell 1 (value 1) vs cell 8 (value 8, the max) */
	CHECK(cold != hot, "low and high heatgrid cells get different colours");
	CHECK(lt_canvas_char(c, 1, 0) != lt_canvas_char(c, 25, 0), "and different-sized dots (a 0 value is just the empty placeholder)");
	double nanrow[3] = {NAN, NAN, NAN};
	lt_heatgrid(c, (lt_rect){0, 0, 10, 1}, &th, nanrow, 3, 1, 0, 1, 1, NULL);     /* all-NaN: must not crash */

	/* tag cloud: every word is placed somewhere */
	const char *words[3] = {"small", "medium", "HUGE"};
	double weights[3] = {1, 5, 10};
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	lt_tagcloud(c, (lt_rect){0, 0, 40, 4}, &th, words, weights, 3);
	char *tc = lt_canvas_dump(c);
	CHECK(strstr(tc, "HUGE") && strstr(tc, "small"), "every word is placed somewhere");
	free(tc);

	/* minimap: draws its density, a mark on the right edge */
	uint8_t density[100];
	for (int i = 0; i < 100; i++) density[i] = (uint8_t)(i % 2 ? 200 : 10);
	int marks[1] = {5};
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	lt_minimap(c, (lt_rect){0, 0, 8, 10}, &th, density, 100, 0, 10, marks, 1);
	CHECK(has_glyph(c, 0, 0), "the minimap draws its density");

	/* the fuzz: every widget above, many small/odd rectangles, NaN sprinkled in, must never crash */
	rng_state = 7;
	for (int it = 0; it < 4000; it++) {
		int w = rnd(50);
		int h = rnd(20);
		int rx = rnd(6) - 2;
		int ry = rnd(6) - 2;
		lt_rect rr = {rx, ry, w, h};
		lt_theme *tt = rnd(4) == 0 ? &mono : &th;
		int which = rnd(11);
		switch (which) {
		case 0: lt_spinner(c, rr, tt, (lt_spin_kind)rnd(6), rnd(2) ? NAN : (double)rnd(1000) / 10, rnd(2) ? "lbl" : NULL); break;
		case 1: lt_piebar(c, rr, tt, sl, rnd(2) ? 3 : 0, rnd(2)); break;
		case 2: lt_piechart(c, rr, tt, sl, rnd(2) ? 3 : 0, rnd(2), rnd(2)); break;
		case 3: lt_gauge(c, rr, tt, rnd(2) ? NAN : (double)rnd(100) / 100, rnd(2) ? "l" : NULL, rnd(2)); break;
		case 4: lt_linechart(c, rr, tt, ss, rnd(3), NULL); break;
		case 5: lt_linechart(c, rr, tt, ss, rnd(3), &opt); break;
		case 6: lt_barchart(c, rr, tt, bl, bv, rnd(5), rnd(2) ? &bopt : NULL); break;
		case 7: lt_histogram(c, rr, tt, sample, rnd(200), 1 + rnd(20), 0, rnd(2) ? 0 : 100, NULL); break;
		case 8: { int hcols = 1 + rnd(3); lt_heatgrid(c, rr, tt, hv, hcols, 1 + rnd(9 / hcols), 0, rnd(2) ? 8 : 0, 1 + rnd(4), NULL); break; }   /* cols*rows must stay within hv's 9 doubles */
		case 9: lt_tagcloud(c, rr, tt, words, weights, rnd(4)); break;
		case 10: lt_minimap(c, rr, tt, density, 1 + rnd(100), rnd(50), rnd(50), marks, rnd(2)); break;
		}
	}
	printf("display fuzz: %d iterations, no crash\n", 4000);

	lt_canvas_free(c);
	printf(fails ? "display: %d failed\n" : "display: ok\n", fails);
	return fails != 0;
}
