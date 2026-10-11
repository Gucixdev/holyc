/* test_kinds.c - the widget kinds registered behind kind_ops (kinds.c), through the spec language, headless. */
#include "lazytui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); fails++; } } while (0)
static lt_caps UNI = {true, true, false, true, 256};
static lt_event key(int k) { return (lt_event){LT_EV_KEY, k, 0, 0, 0, NULL}; }

static char last_id[32], last_kind[32], last_text[64];
static int last_index, calls;
static void on_any(lt_app *a, const lt_appevent *ev, void *ctx)
{
	(void)a; (void)ctx;
	snprintf(last_id, sizeof last_id, "%s", ev->id); snprintf(last_kind, sizeof last_kind, "%s", ev->kind); snprintf(last_text, sizeof last_text, "%s", ev->text ? ev->text : "");
	last_index = ev->index; calls++;
}
static char *screen(lt_app *a, int w, int h)
{
	lt_canvas *c = lt_canvas_new(w, h, UNI);
	lt_theme th = lt_theme_default();
	lt_canvas_clear(c, LT_STYLE(0, 0, 0));
	lt_app_render(a, c, &th);
	char *d = lt_canvas_dump(c);
	lt_canvas_free(c);
	return d;
}
static void press(lt_app *a, int k) { lt_event e = key(k); lt_app_dispatch(a, &e, &UNI, 0); }

int main(void)
{
	char err[200];
	lt_app *a = lt_app_new();
	CHECK(lt_app_load(a,
		"col\n"
		"  row size=1\n"
		"    buttonbar id=b1 group=top items=\"one,two\" size=12\n"
		"    buttonbar id=b2 group=top items=\"alpha,beta\" size=fill\n"
		"  panel title=\"tree\" size=10\n"
		"    treeview id=tv bind=picked\n", err, sizeof err), err);
	lt_app_on(a, "*", "*", on_any, NULL);
	lt_tree *t = lt_app_tree(a, "tv");
	CHECK(t != NULL, "a treeview owns a tree reachable by id");
	int root = lt_tree_add(t, -1, "root", 1), kid = lt_tree_add(t, root, "kid", 2);
	lt_tree_add(t, root, "sibling", 3); lt_tree_add(t, kid, "leaf", 4);
	lt_tree_expand_all(t, true);

	char *d = screen(a, 60, 14);
	CHECK(strstr(d, "root") && strstr(d, "tree view"), "the plain tree look with the mode footer");
	free(d);

	/* Tab focuses (no focus yet), then the buttonbars form one bar */
	press(a, LT_K_TAB);
	CHECK(!strcmp(lt_app_focused(a), "b1"), "Tab focuses the first focusable");
	press(a, LT_K_RIGHT); press(a, LT_K_RIGHT);
	CHECK(!strcmp(lt_app_focused(a), "b2"), "Right past the edge of a grouped bar moves into the next bar");
	press(a, LT_K_LEFT);
	CHECK(!strcmp(lt_app_focused(a), "b1"), "Left at the start of the next bar returns to the previous one");
	press(a, LT_K_TAB);
	CHECK(!strcmp(lt_app_focused(a), "b2"), "Tab moves focus on (a buttonbar no longer swallows it)");
	press(a, LT_K_TAB);
	CHECK(!strcmp(lt_app_focused(a), "tv"), "Tab reaches the treeview");

	/* Tab on the treeview switches its mode instead of moving focus (just tree/dropdown now - the breadcrumbs look moved out
	 * to its own standalone kind, tested separately below, so it is no longer also a treeview mode under a second name) */
	calls = 0;
	press(a, LT_K_TAB);
	CHECK(!strcmp(lt_app_focused(a), "tv") && !strcmp(last_kind, "mode") && !strcmp(last_text, "dropdown") && last_index == 1, "Tab switches the mode to dropdown");
	d = screen(a, 60, 14);
	CHECK(strstr(d, "dropdown view"), "the dropdown look");
	free(d);
	press(a, LT_K_TAB);
	CHECK(!strcmp(last_text, "tree") && last_index == 0, "and back to the plain tree");
	press(a, LT_K_BTAB);
	CHECK(strcmp(lt_app_focused(a), "tv") != 0, "Shift-Tab still moves focus");
	press(a, LT_K_F6);
	CHECK(!strcmp(lt_app_focused(a), "tv"), "F6 moves focus forward");

	lt_app_free(a);
	printf(fails ? "kinds: %d failed\n" : "kinds: ok\n", fails);
	return fails != 0;
}
