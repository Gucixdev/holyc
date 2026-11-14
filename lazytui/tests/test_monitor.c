/* test_monitor.c - histgraph, segbar, corebars, ratepanel, proctable (the gtop ports) + human formatters. Headless. */
#include "lazytui.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); fails++; } } while (0)
static lt_caps UNI = {true, true, false, true, 256};
static lt_caps ASC = {false, false, false, false, 16};
static lt_event key(int k) { return (lt_event){LT_EV_KEY, k, 0, 0, 0, NULL}; }
static lt_event mouse(int k, int x, int y) { return (lt_event){LT_EV_MOUSE, k, x, y, 0, NULL}; }
static char *shot(lt_canvas *c) { return lt_canvas_dump(c); }
static int count(const char *s, const char *needle) { int n = 0; for (; (s = strstr(s, needle)); s += strlen(needle)) n++; return n; }

int main(void)
{
	lt_theme th = lt_theme_default();
	char b[40];
	lt_human_bytes(0, b, sizeof b); CHECK(!strcmp(b, "0B"), "0 bytes");
	lt_human_bytes(1536, b, sizeof b); CHECK(!strcmp(b, "1.5KB"), "1.5KB");
	lt_human_rate(3.0 * 1024 * 1024, b, sizeof b); CHECK(!strcmp(b, "3.0MB/s"), "rate");
	lt_human_bytes(-5, b, sizeof b); CHECK(!strcmp(b, "0B"), "negative is 0");
	lt_human_bytes(1e30, b, sizeof b); CHECK(strstr(b, "TB"), "huge caps at TB");

	/* histgraph */
	lt_canvas *c = lt_canvas_new(30, 6, UNI);
	double h[5] = {0, 50, 100, 50, 0};
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	CHECK(lt_histgraph(c, (lt_rect){0, 0, 20, 1}, &th, h, 5, 100, LT_T_OK) == 100, "fixed ceiling is returned");
	char *d = shot(c);
	CHECK(strstr(d, "\xE2\x96\x88") && strstr(d, "\xE2\x96\x84"), "full and half blocks present");
	free(d);
	double r[3] = {10, 40, 20};
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	CHECK(lt_histgraph(c, (lt_rect){0, 0, 10, 3}, &th, r, 3, 0, LT_T_OK) == 40, "auto ceiling = max sample");
	double tiny[3] = {0, 0.0001, 0};
	CHECK(lt_histgraph(c, (lt_rect){0, 4, 10, 1}, &th, tiny, 3, 100, LT_T_OK) == 100, "tiny value ok");
	d = shot(c); CHECK(strstr(d, "\xE2\x96\x81"), "a tiny non-zero value still shows the lowest block"); free(d);
	lt_histgraph(c, (lt_rect){0, 0, 0, 0}, &th, r, 3, 0, LT_T_OK);
	lt_histgraph(c, (lt_rect){0, 0, 10, 1}, &th, NULL, 3, 0, LT_T_OK);
	lt_histgraph(c, (lt_rect){0, 0, 10, 1}, &th, r, 0, 0, LT_T_OK);
	double nan3[3] = {NAN, INFINITY, -4};
	lt_histgraph(c, (lt_rect){0, 0, 10, 1}, &th, nan3, 3, 0, LT_T_OK);
	lt_histgraph(c, (lt_rect){25, 5, 20, 3}, &th, r, 3, 0, LT_T_OK);       /* partly off canvas: clipped */
	double one[1] = {7};
	lt_histgraph(c, (lt_rect){0, 0, 5, 1}, &th, one, 1, 0, LT_T_OK);
	lt_canvas_free(c);
	c = lt_canvas_new(20, 3, ASC);
	lt_histgraph(c, (lt_rect){0, 0, 10, 1}, &th, h, 5, 100, LT_T_OK);
	d = shot(c); CHECK(strstr(d, "#"), "ascii fallback"); free(d);
	lt_canvas_free(c);

	/* segbar */
	c = lt_canvas_new(40, 2, UNI);
	double v[3] = {50, 30, 20};
	lt_slot sl[3] = {LT_T_OK, LT_T_WARN, LT_T_ERR};
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	lt_segbar(c, (lt_rect){0, 0, 40, 1}, &th, v, sl, 3, 100, " 100%");
	d = shot(c);
	CHECK(count(d, "\xE2\x96\x88") == 35 && !strstr(d, "\xE2\x96\x91") && strstr(d, "100%"), "values sum to total: bar fully filled (35 cells, suffix 5)");
	free(d);
	double part[2] = {10, 10};
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	lt_segbar(c, (lt_rect){0, 0, 40, 1}, &th, part, sl, 2, 100, NULL);
	d = shot(c);
	CHECK(count(d, "\xE2\x96\x88") == 8 && count(d, "\xE2\x96\x91") == 32, "values below total leave the rest as shade (not stretched)");
	free(d);
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	lt_segbar(c, (lt_rect){0, 0, 40, 1}, &th, part, sl, 2, 0, "x");
	d = shot(c); CHECK(count(d, "\xE2\x96\x88") == 0, "total 0 = empty bar"); free(d);
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	double third[3] = {1, 1, 1};
	lt_segbar(c, (lt_rect){0, 0, 10, 1}, &th, third, sl, 3, 3, NULL);
	d = shot(c); CHECK(count(d, "\xE2\x96\x88") == 10, "thirds of 10 cells: remainder goes to the last, no gap"); free(d);
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	lt_segbar(c, (lt_rect){0, 0, 6, 1}, &th, v, sl, 3, 100, " a very long suffix");
	d = shot(c); CHECK(!strstr(d, "suffix"), "suffix dropped when it would squash the bar"); free(d);
	double neg[2] = {-5, NAN};
	lt_segbar(c, (lt_rect){0, 0, 10, 1}, &th, neg, sl, 2, 100, NULL);
	lt_segbar(c, (lt_rect){0, 0, 0, 1}, &th, v, sl, 3, 100, NULL);
	lt_segbar(c, (lt_rect){0, 0, 10, 1}, &th, NULL, sl, 3, 100, NULL);
	lt_canvas_free(c);

	/* corebars */
	c = lt_canvas_new(50, 5, UNI);
	double hist[4] = {10, 90, 40, 80};
	lt_core cores[3] = {{"c0", 12.5, "3400MHz 55C", hist, 4}, {"c1", 95, NULL, NULL, 0}, {"c2", 70, "x", hist, 4}};
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	CHECK(lt_corebars(c, (lt_rect){0, 0, 50, 5}, &th, cores, 3, 0) == 3, "three rows drawn");
	d = shot(c); CHECK(strstr(d, "c0") && strstr(d, "12.5%") && strstr(d, "3400MHz 55C") && strstr(d, "95.0%"), "label, percent and info drawn"); free(d);
	CHECK(lt_corebars(c, (lt_rect){0, 0, 50, 2}, &th, cores, 3, 0) == 2, "cut to r.h");
	CHECK(lt_corebars(c, (lt_rect){0, 0, 50, 5}, &th, cores, 3, 2) == 1, "first scrolls");
	CHECK(lt_corebars(c, (lt_rect){0, 0, 50, 5}, &th, cores, 3, 99) == 0, "first past the end");
	CHECK(lt_corebars(c, (lt_rect){0, 0, 0, 5}, &th, cores, 3, 0) == 0 && lt_corebars(c, (lt_rect){0, 0, 5, 5}, &th, cores, 3, 0) == 3, "tiny widths do not crash");
	CHECK(lt_load_slot(10) == LT_T_OK && lt_load_slot(70) == LT_T_WARN && lt_load_slot(99) == LT_T_ERR && lt_load_slot(NAN) == LT_T_OK, "load tiers");
	lt_canvas_free(c);

	/* ratepanel */
	c = lt_canvas_new(60, 6, UNI);
	double rh[4] = {1000, 5000, 2000, 9000};
	lt_rate rates[2] = {{"read", 2048, 1e6, rh, 4}, {"write", 0, 0, NULL, 0}};
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	CHECK(lt_ratepanel(c, (lt_rect){0, 0, 60, 2}, &th, rates, 2) == 2, "two rows in two lines");
	d = shot(c); CHECK(strstr(d, "read") && strstr(d, "2.0KB/s") && strstr(d, "pk 976.6KB/s"), "label, rate and peak"); free(d);
	CHECK(lt_ratepanel(c, (lt_rect){0, 0, 60, 6}, &th, rates, 2) == 2, "tall: two lines per row");
	CHECK(lt_ratepanel(c, (lt_rect){0, 0, 60, 1}, &th, rates, 2) == 1, "cut to height");
	CHECK(lt_ratepanel(c, (lt_rect){0, 0, 4, 1}, &th, rates, 2) == 1, "narrow ok");
	CHECK(lt_ratepanel(c, (lt_rect){0, 0, 60, 0}, &th, rates, 2) == 0 && lt_ratepanel(c, (lt_rect){0, 0, 60, 3}, &th, NULL, 0) == 0, "empty");
	lt_canvas_free(c);

	/* proctable */
	lt_proc P[] = {
		{"alpha", 10, -1, 5, 100, 0, false}, {"beta", 11, -1, 50, 300, 0, false}, {"kid-a", 12, 1, 1, 10, 0, false},
		{"kid-b", 13, 1, 9, 20, 0, false}, {"gamma", 14, -1, 20, 200, 0, false},
	};
	lt_proctable *t = lt_proctable_new();
	lt_proctable_set(t, P, 5);
	CHECK(lt_proctable_count(t) == 3, "children hidden while collapsed");
	CHECK(lt_proctable_selected_pid(t) == 11 && !strcmp(lt_proctable_row_name(t, 0), "beta"), "default sort CPU desc: beta first");
	lt_rect tr = {0, 0, 80, 8};
	c = lt_canvas_new(80, 8, UNI);
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	lt_proctable_draw(t, c, tr, &th, true);
	d = shot(c); CHECK(strstr(d, "NAME") && strstr(d, "CPU%") && strstr(d, "beta") && strstr(d, "50.0%"), "draws header and rows"); free(d);
	lt_event e = key(LT_K_RIGHT);
	CHECK(lt_proctable_event(t, tr, &e) == LT_W_CHANGED && lt_proctable_count(t) == 5, "Right expands beta");
	CHECK(!strcmp(lt_proctable_row_name(t, 1), "kid-b"), "children sorted under the parent by the active column");
	e = key(LT_K_DOWN); lt_proctable_event(t, tr, &e);
	CHECK(lt_proctable_selected_pid(t) == 13, "Down moves into the child");
	lt_proctable_set(t, P, 5);
	CHECK(lt_proctable_count(t) == 5 && lt_proctable_selected_pid(t) == 13, "a refresh keeps expansion and selection");
	e = key('s'); CHECK(lt_proctable_event(t, tr, &e) == LT_W_CHANGED && lt_proctable_sort_col(t) == LT_PROC_MEM, "s cycles CPU -> MEM");
	lt_proctable_event(t, tr, &e); lt_proctable_event(t, tr, &e); CHECK(lt_proctable_sort_col(t) == LT_PROC_NAME, "... wraps from DISK to NAME");
	lt_proctable_set_sort(t, LT_PROC_MEM, true);
	CHECK(!strcmp(lt_proctable_row_name(t, 0), "beta"), "sort by mem desc");
	e = key('r'); lt_proctable_event(t, tr, &e);
	CHECK(lt_proctable_sort_desc(t) == false && !strcmp(lt_proctable_row_name(t, 0), "alpha"), "r reverses");
	e = key(LT_K_END); lt_proctable_event(t, tr, &e); CHECK(lt_proctable_selected_pid(t) >= 0, "End");
	e = key(LT_K_HOME); lt_proctable_event(t, tr, &e);
	e = key(LT_K_ENTER); CHECK(lt_proctable_event(t, tr, &e) == LT_W_ACTIVATE, "Enter on a leaf activates");
	e = key('z'); CHECK(lt_proctable_event(t, tr, &e) == LT_W_IGNORED, "other keys ignored");
	lt_canvas_clear(c, th.s[LT_T_TEXT]); lt_proctable_draw(t, c, tr, &th, true);
	e = mouse(LT_K_CLICK, 40, 0);              /* header click */
	lt_proctable_event(t, tr, &e);
	CHECK(lt_proctable_sort_col(t) != LT_PROC_MEM || !lt_proctable_sort_desc(t), "clicking the header changes sorting");
	e = mouse(LT_K_CLICK, 5, 3); CHECK(lt_proctable_event(t, tr, &e) != LT_W_IGNORED, "row click handled");
	e = mouse(LT_K_WHEEL_DOWN, 5, 3); CHECK(lt_proctable_event(t, tr, &e) == LT_W_CHANGED, "wheel moves");
	e = mouse(LT_K_CLICK, 5, 30); CHECK(lt_proctable_event(t, tr, &e) == LT_W_IGNORED, "click outside ignored");
	/* robustness: cycles, bad parents, empty, nan, tiny and huge rects, many rows with scrolling */
	lt_proc bad[3] = {{"a", 1, 1, 1, 1, 1, true}, {"b", 2, 2, NAN, INFINITY, -1, true}, {"c", 3, 99, 0, 0, 0, true}};
	lt_proctable_set(t, bad, 3);
	CHECK(lt_proctable_count(t) == 3, "self-parent / bad parent rows still show as roots");
	lt_proc cyc[2] = {{"x", 1, 1, 0, 0, 0, true}, {"y", 2, 0, 0, 0, 0, true}};
	cyc[0].parent = 1;
	lt_proctable_set(t, cyc, 2);
	CHECK(lt_proctable_count(t) == 2, "a parent cycle neither hangs nor drops rows");
	lt_proctable_set(t, NULL, 0);
	e = key(LT_K_DOWN); CHECK(lt_proctable_event(t, tr, &e) == LT_W_IGNORED && lt_proctable_selected(t) == -1, "empty table ignores keys");
	lt_canvas_clear(c, th.s[LT_T_TEXT]); lt_proctable_draw(t, c, tr, &th, false);
	lt_proc many[200];
	char names[200][12];
	for (int i = 0; i < 200; i++) { snprintf(names[i], 12, "p%d", i); many[i] = (lt_proc){names[i], 1000 + i, -1, i % 97, i * 1e6, i, false}; }
	lt_proctable_set(t, many, 200);
	for (int i = 0; i < 30; i++) { e = key(LT_K_PGDN); lt_proctable_event(t, tr, &e); }
	lt_canvas_clear(c, th.s[LT_T_TEXT]); lt_proctable_draw(t, c, tr, &th, true);
	d = shot(c); CHECK(strstr(d, "\xE2\x94\x83") || strstr(d, "\xE2\x94\x82"), "scrollbar drawn when rows overflow"); free(d);
	lt_proctable_draw(t, c, (lt_rect){0, 0, 3, 1}, &th, true);
	lt_proctable_draw(t, c, (lt_rect){0, 0, 80, 1}, &th, true);
	lt_proctable_draw(t, c, (lt_rect){70, 5, 80, 20}, &th, true);
	lt_proctable_draw(t, c, (lt_rect){0, 0, 14, 6}, &th, true);
	lt_proctable_free(t);
	lt_canvas_free(c);
	lt_proctable_free(NULL);

	if (fails) { printf("test_monitor: %d FAILED\n", fails); return 1; }
	printf("test_monitor: ok\n");
	return 0;
}
