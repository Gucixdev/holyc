/* test_robust.c - a randomized fuzz of the app layer: every widget kind in one spec, random canvas sizes, random key/mouse events,
 * ticks and page switches, thousands of iterations. The only assertion is "it never crashes" (run under ASan/UBSan by `make asan`). */
#include "lazytui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static lt_caps UNI = {true, true, false, true, 256};

static const char *SPEC =
	"col\n"
	"  row size=1\n"
	"    buttonbar id=tabs group=nav items=\"main,extras\" active=tab size=12\n"
	"    buttonbar id=actions group=nav items=\"a,b,c\" size=fill\n"
	"  pages bind=tab\n"
	"    scroll id=page0\n"
	"      panel title=\"bars\" size=6\n"
	"        progressbar name=p bind=progress size=1\n"
	"        zonebar name=z zones=\"used=zu:err,free=zf:ok\" size=1\n"
	"        pacmanbar name=pb size=1\n"
	"      panel title=\"search\" size=10\n"
	"        searchbar id=q prompt=\"> \" bind=query size=1\n"
	"        list id=l\n"
	"      panel title=\"tree\" size=8\n"
	"        treeview id=t\n"
	"    scroll id=page1\n"
	"      panel title=\"graph\" size=8\n"
	"        custom id=g type=graph\n"
	"      rain\n"
	"  keybar hints=\"tab:focus;f1:x\" size=2\n";

static void draw_graph(lt_app *a, const char *id, lt_canvas *c, lt_rect r, const lt_theme *t, bool focused, void *ctx)
{
	(void)a; (void)id; (void)focused;
	double *vals = ctx;
	lt_graph(c, r, t, vals, 20, 0, 100, LT_T_OK);
}
static lt_wres event_graph(lt_app *a, const char *id, lt_rect r, const lt_event *ev, void *ctx)
{
	(void)a; (void)id; (void)r; (void)ev; (void)ctx;
	return LT_W_IGNORED;
}

int main(void)
{
	char err[300];
	lt_app *a = lt_app_new();
	if (!lt_app_load(a, SPEC, err, sizeof err)) { printf("robust: FAIL spec: %s\n", err); return 1; }
	double vals[20];
	for (int i = 0; i < 20; i++) vals[i] = i * 5;
	lt_app_custom(a, "graph", draw_graph, event_graph, vals);

	lt_list *fl = lt_app_list(a, "l");
	lt_tree *ft = lt_app_tree(a, "t");
	for (int i = 0; i < 50; i++) { char b[24]; snprintf(b, sizeof b, "row %d", i); lt_list_add(fl, b, i); lt_tree_add(ft, i > 3 ? i / 4 - 1 : -1, b, i); }
	lt_tree_expand_all(ft, true);
	lt_app_key(a, "ctrl-x", "boom");
	lt_app_set(a, "zu", 30); lt_app_set(a, "zf", 70);
	lt_app_set(a, "done", 10); lt_app_set(a, "total", 100); lt_app_set(a, "rate", 5);

	unsigned rng = 99;
	static const int ks[] = {LT_K_UP, LT_K_DOWN, LT_K_LEFT, LT_K_RIGHT, LT_K_ENTER, LT_K_ESC, LT_K_TAB, LT_K_BTAB, LT_K_PGUP, LT_K_PGDN,
	                          LT_K_HOME, LT_K_END, ' ', 'a', 'x', LT_K_BSPACE, LT_MOD_CTRL | 'x', LT_MOD_ALT | 'b', LT_K_F1, LT_K_F6};
	static const int ms[] = {LT_K_CLICK, LT_K_CLICK, LT_K_DRAG, LT_K_RELEASE, LT_K_WHEEL_UP, LT_K_WHEEL_DOWN, LT_K_RCLICK, LT_K_DCLICK};
#define R(n) (rng = rng * 1664525u + 1013904223u, (int)((rng >> 8) % (unsigned)(n)))
	long widget_calls = 0;
	for (int it = 0; it < 6000; it++) {
		int w = 1 + R(90), h = 1 + R(30);
		lt_canvas *fc = lt_canvas_new(w, h, UNI);
		lt_theme th2 = lt_theme_matrix();
		lt_app_render(a, fc, &th2);
		for (int k = 0; k < 3; k++) {
			lt_event fe = {0};
			if (R(3)) { fe.kind = LT_EV_KEY; fe.key = ks[R(20)]; } else { fe.kind = LT_EV_MOUSE; fe.key = ms[R(8)]; fe.x = R(100) - 5; fe.y = R(34) - 2; }
			lt_app_dispatch(a, &fe, &UNI, 0);
			widget_calls++;
		}
		if (R(50) == 0) lt_app_tick(a, 0.05);
		if (R(40) == 0) lt_app_set(a, "tab", R(2));
		if (R(30) == 0) lt_app_set(a, "progress", (double)R(100) / 100.0);
		lt_canvas_free(fc);
	}
	lt_app_free(a);

	printf("robust: ok (%ld widget calls)\n", widget_calls);
	return 0;
}
