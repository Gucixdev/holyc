/* test_volumebar.c - lt_volumebar: draw (level, MUTE, vertical, tiny sizes), keys, mouse (click/drag/wheel/mute text), callbacks. Headless. */
#include "lazytui.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); fails++; } } while (0)
static lt_caps UNI = {true, true, false, true, 256};
static lt_event key(int k) { return (lt_event){LT_EV_KEY, k, 0, 0, 0, NULL}; }
static lt_event mouse(int k, int x, int y) { return (lt_event){LT_EV_MOUSE, k, x, y, 0, NULL}; }

typedef struct { double level; int muted, sets, mutes; } fake;
static double f_get(void *c) { return ((fake *)c)->level; }
static void f_set(void *c, double v) { fake *f = c; f->level = v; f->sets++; }
static int f_gm(void *c) { return ((fake *)c)->muted; }
static void f_sm(void *c, int m) { fake *f = c; f->muted = m; f->mutes++; }

int main(void)
{
	lt_theme th = lt_theme_default();
	lt_canvas *c = lt_canvas_new(40, 12, UNI);
	fake f = {0.50, 0, 0, 0};
	lt_volumebar *v = lt_volumebar_new("spk", (lt_volume_cb){f_get, f_set, f_gm, f_sm, &f});
	lt_rect r = {0, 0, 30, 1};

	lt_canvas_clear(c, th.s[LT_T_TEXT]); lt_volumebar_draw(v, c, r, &th, true);
	char *d = lt_canvas_dump(c);
	CHECK(strstr(d, "spk") && strstr(d, "50%"), "draws label and percent from the get callback");
	free(d);

	CHECK(lt_volumebar_event(v, r, &(lt_event){LT_EV_KEY, LT_K_RIGHT, 0, 0, 0, NULL}) == LT_W_CHANGED && fabs(f.level - 0.55) < 1e-9, "Right steps up by 0.05 through set()");
	lt_volumebar_event(v, r, &(lt_event){LT_EV_KEY, '-', 0, 0, 0, NULL}); lt_volumebar_event(v, r, &(lt_event){LT_EV_KEY, LT_K_LEFT, 0, 0, 0, NULL});
	CHECK(fabs(f.level - 0.45) < 1e-9, "- and Left step down");
	lt_event e = key(LT_K_UP);
	CHECK(lt_volumebar_event(v, r, &e) == LT_W_IGNORED, "Up is ignored on a horizontal bar");
	e = key(LT_K_END); lt_volumebar_event(v, r, &e); CHECK(f.level == 1.0, "End = max");
	e = key('+'); lt_volumebar_event(v, r, &e); CHECK(f.level == 1.0, "clamped at max");
	e = key(LT_K_HOME); lt_volumebar_event(v, r, &e); e = key('-'); lt_volumebar_event(v, r, &e); CHECK(f.level == 0.0, "clamped at 0");
	e = key('m'); CHECK(lt_volumebar_event(v, r, &e) == LT_W_TOGGLE && f.muted == 1 && f.mutes == 1, "m mutes through set_mute");
	lt_canvas_clear(c, th.s[LT_T_TEXT]); lt_volumebar_draw(v, c, r, &th, false); d = lt_canvas_dump(c);
	CHECK(strstr(d, "MUTE"), "muted shows MUTE");
	free(d);
	e = key('m'); lt_volumebar_event(v, r, &e); CHECK(f.muted == 0, "m again unmutes");

	/* mouse: bar spans x=4..24 (label 3 + gap, text 5) */
	e = mouse(LT_K_CLICK, 4, 0); lt_volumebar_event(v, r, &e); CHECK(f.level == 0.0, "click at the bar's left edge = 0");
	e = mouse(LT_K_CLICK, 24, 0); lt_volumebar_event(v, r, &e); CHECK(f.level == 1.0, "click at the right edge = max");
	e = mouse(LT_K_DRAG, 14, 0); lt_volumebar_event(v, r, &e); CHECK(f.level > 0.4 && f.level < 0.6, "drag sets to the middle");
	f.level = 0.5; e = mouse(LT_K_WHEEL_UP, 10, 0); lt_volumebar_event(v, r, &e); CHECK(fabs(f.level - 0.55) < 1e-9, "wheel up steps up");
	e = mouse(LT_K_WHEEL_DOWN, 10, 0); lt_volumebar_event(v, r, &e); CHECK(fabs(f.level - 0.50) < 1e-9, "wheel down steps down");
	e = mouse(LT_K_CLICK, 27, 0); CHECK(lt_volumebar_event(v, r, &e) == LT_W_TOGGLE && f.muted == 1, "click on the percent text mutes");
	e = mouse(LT_K_CLICK, 5, 5); CHECK(lt_volumebar_event(v, r, &e) == LT_W_IGNORED, "click outside is ignored");
	f.muted = 0;

	/* external change is shown without us touching it */
	f.level = 0.20; lt_canvas_clear(c, th.s[LT_T_TEXT]); lt_volumebar_draw(v, c, r, &th, false); d = lt_canvas_dump(c);
	CHECK(strstr(d, "20%"), "an external change shows up on the next draw");
	free(d);

	/* vertical */
	lt_volumebar *w = lt_volumebar_new("mic", (lt_volume_cb){f_get, f_set, f_gm, f_sm, &f});
	lt_volumebar_set_vertical(w, true); lt_volumebar_set_step(w, 0.1);
	lt_rect vr = {0, 0, 5, 10};
	f.level = 0.5; lt_canvas_clear(c, th.s[LT_T_TEXT]); lt_volumebar_draw(w, c, vr, &th, true); d = lt_canvas_dump(c);
	CHECK(strstr(d, "mic") && strstr(d, "50%"), "vertical shows label and percent");
	free(d);
	e = key(LT_K_UP); lt_volumebar_event(w, vr, &e); CHECK(fabs(f.level - 0.6) < 1e-9, "Up raises a vertical bar by its step");
	e = key(LT_K_RIGHT); CHECK(lt_volumebar_event(w, vr, &e) == LT_W_IGNORED, "Right ignored on vertical");
	e = mouse(LT_K_CLICK, 2, 1); lt_volumebar_event(w, vr, &e); CHECK(f.level == 1.0, "click on the top of a vertical bar = max");
	e = mouse(LT_K_CLICK, 2, 8); lt_volumebar_event(w, vr, &e); CHECK(f.level == 0.0, "click on the bottom = 0");

	/* no callbacks: keeps its own state */
	lt_volumebar *s = lt_volumebar_new(NULL, (lt_volume_cb){0});
	e = key('+'); lt_volumebar_event(s, r, &e); e = key('m'); lt_volumebar_event(s, r, &e);
	CHECK(fabs(lt_volumebar_level(s) - 0.05) < 1e-9 && lt_volumebar_muted(s), "works standalone without callbacks");
	lt_volumebar_set_max(s, 1.5); lt_volumebar_set_level(s, 9); CHECK(lt_volumebar_level(s) == 1.5, "max 1.5 allows boost, clamps there");

	/* never crash on odd sizes / NaN */
	f.level = NAN;
	lt_rect odd[] = {{0, 0, 0, 0}, {0, 0, 1, 1}, {0, 0, 3, 1}, {35, 10, 20, 5}, {0, 0, 2, 12}};
	for (int i = 0; i < 5; i++) { lt_volumebar_draw(v, c, odd[i], &th, true); lt_volumebar_draw(w, c, odd[i], &th, false); lt_volumebar_event(v, odd[i], &(lt_event){LT_EV_MOUSE, LT_K_CLICK, 0, 0, 0, NULL}); }
	lt_canvas *asc = lt_canvas_new(40, 12, (lt_caps){false, false, false, false, 16});
	f.level = 0.5; lt_volumebar_draw(v, asc, r, &th, true); lt_volumebar_draw(w, asc, vr, &th, true);

	lt_canvas_free(asc); lt_canvas_free(c); lt_volumebar_free(v); lt_volumebar_free(w); lt_volumebar_free(s);
	if (fails) { printf("%d failures\n", fails); return 1; }
	printf("test_volumebar: ok\n");
	return 0;
}
