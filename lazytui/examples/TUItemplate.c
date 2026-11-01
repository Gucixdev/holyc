/* TUItemplate.c - the smallest lazytui app worth copying: a bar on top with the tabs, a bar at the bottom with the key hints, and in the
 * middle a scrolling page per tab holding whatever widgets or text you want. Copy it, rename the tabs, put your own panels in SPEC.
 *   cc -std=c99 -Iinclude -o TUItemplate examples/TUItemplate.c liblazytui.a -lm
 * `TUItemplate --snapshot [W H [tab]]` prints one frame as plain text (no tty needed). */
#include "lazytui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the screen as data: top bar | the pages (one per tab, each scrolls) | bottom bar ---- */
static const char *SPEC =
	"col\n"
	"  buttonbar id=tabs group=top items=\"home,list,about\" active=tab size=1\n"        /* top: the tabs (F1..F3 or click) */
	"  pages bind=tab\n"
	"    scroll id=page_home\n"
	"      panel title=\"welcome\" size=5\n"
	"        text bind=hello\n"
	"      panel title=\"progress\" size=3\n"
	"        progressbar name=load namew=8 bind=progress size=1\n"
	"    scroll id=page_list\n"
	"      panel title=\"things\" size=12\n"
	"        list id=things\n"
	"      panel title=\"picked\" size=3\n"
	"        text bind=picked\n"
	"    scroll id=page_about\n"
	"      panel title=\"about\" size=4\n"
	"        text bind=about\n"
	"  keybar hints=\"f1/f2/f3:tab;wheel:scroll;Enter:pick;esc:quit\" size=1\n";                 /* bottom: the key hints */

static void on_app(lt_app *a, const lt_appevent *ev, void *ctx)
{
	(void)ctx;
	const char *k = ev->kind;
	if (!strcmp(k, "tick")) { double p = lt_app_number(a, "progress") + 0.004; lt_app_set(a, "progress", p > 1 ? 0 : p); }
	else if (!strcmp(k, "page0")) lt_app_set(a, "tab", 0);
	else if (!strcmp(k, "page1")) lt_app_set(a, "tab", 1);
	else if (!strcmp(k, "page2")) lt_app_set(a, "tab", 2);
	else if (!strcmp(k, "quit")) lt_app_quit(a);
}

static void on_widget(lt_app *a, const lt_appevent *ev, void *ctx)
{
	(void)ctx;
	if (!strcmp(ev->id, "things") && !strcmp(ev->kind, "activate")) {
		char buf[80];
		snprintf(buf, sizeof buf, "you picked item %d", ev->index);
		lt_app_set_text(a, "picked", buf);
	}
}

int main(int argc, char **argv)
{
	lt_app *a = lt_app_new();
	char err[200];
	if (!lt_app_load(a, SPEC, err, sizeof err)) { fprintf(stderr, "TUItemplate: spec error: %s\n", err); return 1; }
	lt_app_on(a, "app", "*", on_app, NULL);
	lt_app_on(a, "*", "activate", on_widget, NULL);
	lt_app_key(a, "f1", "page0"); lt_app_key(a, "f2", "page1"); lt_app_key(a, "f3", "page2");
	lt_app_every(a, 0.1, "tick");
	lt_app_key(a, "esc", "quit"); lt_app_key(a, "ctrl-q", "quit");
	lt_app_set_text(a, "hello", "Edit SPEC at the top of this file: tabs, pages and key hints are data.");
	lt_app_set_text(a, "about", "Top bar = tabs, bottom bar = keys, the middle scrolls.");
	lt_app_set_text(a, "picked", "nothing yet");
	lt_app_set(a, "progress", 0.3);
	lt_app_set(a, "tab", 0);
	lt_list *l = lt_app_list(a, "things");
	for (int i = 1; i <= 20; i++) { char buf[32]; snprintf(buf, sizeof buf, "thing number %d", i); lt_list_add(l, buf, i); }

	if (argc > 1 && !strcmp(argv[1], "--snapshot")) {
		int w = argc > 2 ? atoi(argv[2]) : 70, h = argc > 3 ? atoi(argv[3]) : 20;
		if (argc > 4) lt_app_set(a, "tab", atoi(argv[4]));
		lt_canvas *c = lt_canvas_new(w, h, lt_caps_detect());
		lt_theme th = lt_theme_default();
		lt_canvas_clear(c, th.s[LT_T_TEXT]);
		lt_app_render(a, c, &th);
		char *d = lt_canvas_dump(c);
		fputs(d, stdout);
		free(d);
		return 0;
	}
	lt_tty *t = lt_tty_open((lt_tty_opts){.alt_screen = true, .mouse = true, .kitty = true});
	if (!t) { fprintf(stderr, "TUItemplate: needs a terminal (try --snapshot)\n"); return 1; }
	lt_ui *ui = lt_ui_new(t);
	lt_app_attach(a, ui);
	lt_ui_run(ui);
	lt_ui_free(ui);
	lt_tty_close(t);
	lt_app_free(a);
	return 0;
}
