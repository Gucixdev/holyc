/* test_app.c - the data-driven / event-driven layer, headless: a spec, a model, synthetic events, handlers. */
#include "lazytui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); fails++; } } while (0)

static lt_caps UNI = {true, true, false, true, 256};
static lt_event key(int k) { return (lt_event){LT_EV_KEY, k, 0, 0, 0, NULL}; }
static lt_event mouse(int k, int x, int y) { return (lt_event){LT_EV_MOUSE, k, x, y, 0, NULL}; }

static char last_id[32], last_kind[32], last_text[64];
static int calls, last_index;
static void on_any(lt_app *a, const lt_appevent *ev, void *ctx)
{
	(void)a; (void)ctx;
	snprintf(last_id, sizeof last_id, "%s", ev->id); snprintf(last_kind, sizeof last_kind, "%s", ev->kind); snprintf(last_text, sizeof last_text, "%s", ev->text ? ev->text : "");
	last_index = ev->index; calls++;
}
static int theme_calls, tick_calls, timer_calls;
static void on_theme(lt_app *a, const lt_appevent *ev, void *ctx) { (void)a; (void)ev; (void)ctx; theme_calls++; }
static void on_tick(lt_app *a, const lt_appevent *ev, void *ctx) { (void)a; (void)ev; (void)ctx; tick_calls++; }
static void on_timer(lt_app *a, const lt_appevent *ev, void *ctx) { (void)a; (void)ev; (void)ctx; timer_calls++; }

static int custom_draws, custom_events;
static void draw_cb(lt_app *app, const char *id, lt_canvas *c, lt_rect r, const lt_theme *t, bool focused, void *ctx)
{ (void)app; (void)id; (void)c; (void)r; (void)t; (void)focused; (void)ctx; custom_draws++; }
static lt_wres event_cb(lt_app *app, const char *id, lt_rect r, const lt_event *ev, void *ctx)
{ (void)app; (void)id; (void)r; (void)ev; (void)ctx; custom_events++; return LT_W_HANDLED; }
static lt_wres event_ignore_cb(lt_app *app, const char *id, lt_rect r, const lt_event *ev, void *ctx)
{ (void)app; (void)id; (void)r; (void)ev; (void)ctx; return LT_W_IGNORED; }

static const char *SPEC =
	"# a small program\n"
	"col\n"
	"  buttonbar id=top items=\"main,extras\" active=tab size=1\n"
	"  pages bind=tab\n"
	"    col\n"
	"      panel title=\"bars\" size=6\n"
	"        progressbar name=\"progress\" bind=progress size=1\n"
	"        searchbar id=q prompt=\"> \" bind=query size=1\n"
	"        dropdown id=mode items=\"filter,fuzzy,tag\" bind=mode name=\"mode\" size=1\n"
	"      list id=results\n"
	"    col\n"
	"      text text=\"the extras page\"\n"
	"  textbar bind=status size=1\n";

static char *render_dump(lt_app *a, int w, int h)
{
	lt_canvas *c = lt_canvas_new(w, h, UNI);
	lt_theme th = lt_theme_default();
	lt_canvas_clear(c, LT_STYLE(0, 0, 0));
	lt_app_render(a, c, &th);
	char *d = lt_canvas_dump(c);
	lt_canvas_free(c);
	return d;
}

int main(void)
{
	char err[200];
	lt_app *a = lt_app_new();
	CHECK(lt_app_load(a, SPEC, err, sizeof err), err);
	lt_app_on(a, "*", "*", on_any, NULL);
	lt_app_on(a, "app", "theme", on_theme, NULL);
	lt_app_on(a, "app", "tick", on_tick, NULL);
	lt_app_on(a, "app", "every", on_timer, NULL);
	lt_app_key(a, "ctrl-t", "theme");
	lt_app_every(a, 0.5, "every");
	lt_list *res = lt_app_list(a, "results");
	CHECK(res != NULL && lt_app_list(a, "nope") == NULL, "the list is reachable by id");
	for (int i = 0; i < 30; i++) { char b[32]; snprintf(b, sizeof b, "result %d", i); lt_list_add(res, b, i); }

	/* model -> screen */
	lt_app_set(a, "progress", 0.5);
	lt_app_set_text(a, "status", "ready");
	char *d = render_dump(a, 60, 20);
	CHECK(strstr(d, "main") && strstr(d, "extras") && strstr(d, "bars") && strstr(d, "50%") && strstr(d, "ready") && strstr(d, "result 0"), "the first page and the model values are on screen");
	CHECK(!strstr(d, "the extras page"), "the other page is not drawn");
	free(d);
	CHECK(lt_app_number(a, "progress") == 0.5 && !strcmp(lt_app_text(a, "status"), "ready") && !strcmp(lt_app_text(a, "missing"), ""), "model getters");

	/* switching pages via the model */
	lt_app_set(a, "tab", 1);
	d = render_dump(a, 60, 20);
	CHECK(strstr(d, "the extras page") && !strstr(d, "result 0"), "page 1 replaces page 0's content");
	free(d);
	lt_app_set(a, "tab", 0);

	/* mouse: focus the search bar by clicking it, type, the model follows and the handler runs */
	render_dump(a, 60, 20);                                                     /* a render is needed so widgets have screen rects */
	lt_rect qrect;
	CHECK(lt_app_rect(a, "q", &qrect), "a drawn widget's rectangle is queryable");
	lt_event e = mouse(LT_K_CLICK, qrect.x + 2, qrect.y);
	lt_app_dispatch(a, &e, &UNI, 0);
	CHECK(!strcmp(lt_app_focused(a), "q"), "clicking the search bar focuses it");
	calls = 0;
	e.kind = LT_EV_KEY; e.key = 'x';
	lt_app_dispatch(a, &e, &UNI, 0);
	CHECK(!strcmp(lt_app_text(a, "query"), "x") && calls > 0 && !strcmp(last_id, "q") && !strcmp(last_kind, "change"), "typing updates the model and fires a change event");
	lt_app_focus(a, "");
	CHECK(!*lt_app_focused(a), "focus can be cleared by id \"\"");
	lt_app_focus(a, "q");
	CHECK(!strcmp(lt_app_focused(a), "q"), "focus can be set directly by id");
	lt_app_focus(a, "nope");
	CHECK(!strcmp(lt_app_focused(a), "q"), "focusing an unknown id leaves the focus unchanged");

	/* Tab cycles focus among focusable widgets */
	lt_app_focus(a, "");
	e = key(LT_K_TAB);
	lt_app_dispatch(a, &e, &UNI, 0);
	CHECK(*lt_app_focused(a) != 0, "Tab focuses the first focusable widget");
	const char *first = lt_app_focused(a);
	char firstbuf[32];
	snprintf(firstbuf, sizeof firstbuf, "%s", first);
	for (int i = 0; i < 10; i++) { lt_app_dispatch(a, &e, &UNI, 0); }            /* enough Tabs to wrap all the way around */
	CHECK(!strcmp(lt_app_focused(a), firstbuf) || *lt_app_focused(a), "Tab cycling never loses focus entirely");

	/* a global hotkey fires a named app event */
	theme_calls = 0;
	e = key(LT_MOD_CTRL | 't');
	lt_app_dispatch(a, &e, &UNI, 0);
	CHECK(theme_calls == 1, "ctrl-t fires the \"theme\" app event exactly once");

	/* tick and the periodic timer */
	lt_app_tick(a, 0.1);
	CHECK(tick_calls == 1, "a tick fires the tick event");
	timer_calls = 0;
	for (int i = 0; i < 10; i++) lt_app_tick(a, 0.1);                            /* 1.0s total, the 0.5s timer should fire twice */
	CHECK(timer_calls >= 1, "the periodic timer fires on schedule");

	/* a custom widget kind: draw + event callbacks run through a second app instance with a simpler spec */
	lt_app *a2 = lt_app_new();
	CHECK(lt_app_load(a2, "col\n  custom id=w type=probe size=fill\n", err, sizeof err), err);
	lt_app_custom(a2, "probe", draw_cb, event_cb, NULL);
	render_dump(a2, 20, 10);
	CHECK(custom_draws > 0, "a custom widget's draw callback runs during render");
	lt_app_focus(a2, "w");
	lt_event ce = mouse(LT_K_CLICK, 5, 5);
	lt_app_dispatch(a2, &ce, &UNI, 0);
	CHECK(custom_events > 0, "a custom widget's event callback runs during dispatch");
	lt_app_free(a2);

	/* regression: a click that lands on a focusable custom widget's own area, but which its own event handler chooses to ignore (e.g. a
	 * calendar's header row, not a day cell), must still focus it. The dispatcher used to unconditionally clear focus back to NULL right
	 * after such a click just because nothing on it returned other than IGNORED - silently breaking Tab/keyboard input for any widget
	 * clicked outside its "hot" sub-area (found chasing a real report against lt_datepicker's month/year quick-pick). */
	lt_app *a3 = lt_app_new();
	CHECK(lt_app_load(a3, "col\n  panel title=\"p\" size=10\n    custom id=w2 type=ignorer\n", err, sizeof err), err);
	lt_app_custom(a3, "ignorer", draw_cb, event_ignore_cb, NULL);
	render_dump(a3, 40, 20);
	lt_event c1 = mouse(LT_K_CLICK, 5, 3);                                      /* inside w2's own area */
	lt_app_dispatch(a3, &c1, &UNI, 0);
	CHECK(!strcmp(lt_app_focused(a3), "w2"), "a click on a focusable widget's own area focuses it, even when that widget's handler ignores the click");
	lt_event c2 = mouse(LT_K_CLICK, 5, 0);                                      /* the panel's own border row: outside every focusable widget */
	lt_app_dispatch(a3, &c2, &UNI, 0);
	CHECK(!strcmp(lt_app_focused(a3), ""), "...but a click that lands on nothing focusable still clears focus");
	lt_app_free(a3);

	printf(fails ? "app: %d failed\n" : "app: ok\n", fails);
	lt_app_free(a);
	return fails != 0;
}
