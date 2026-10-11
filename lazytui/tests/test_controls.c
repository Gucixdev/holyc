/* test_controls.c - button, checkbox/radio/switch, radiogroup, slider (controls.c): draw + synthetic key/mouse events, headless. */
#include "lazytui.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int fails;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); fails++; } } while (0)
static lt_caps UNI = {true, true, false, true, 256};
static lt_event key(int k) { return (lt_event){LT_EV_KEY, k, 0, 0, 0, NULL}; }
static lt_event mouse(int k, int x, int y) { return (lt_event){LT_EV_MOUSE, k, x, y, 0, NULL}; }

int main(void)
{
	lt_theme th = lt_theme_default();
	lt_canvas *c = lt_canvas_new(40, 10, UNI);
	lt_rect r = {0, 0, 20, 1};

	/* button */
	lt_button *b = lt_button_new("OK", LT_BTN_PRIMARY);
	CHECK(lt_button_width(b) > 0, "a button reports a positive width");
	lt_button_draw(b, c, r, &th, true);
	char *bd = lt_canvas_dump(c);
	CHECK(strstr(bd, "OK"), "the button's label is drawn");
	free(bd);
	lt_event e = key(LT_K_ENTER);
	CHECK(lt_button_event(b, r, &e) == LT_W_ACTIVATE, "Enter activates a focused button");
	e = key(' ');
	CHECK(lt_button_event(b, r, &e) == LT_W_ACTIVATE, "Space also activates");
	e = mouse(LT_K_CLICK, 1, 0);
	CHECK(lt_button_event(b, r, &e) == LT_W_ACTIVATE, "a click inside activates");
	e = mouse(LT_K_CLICK, 39, 9);
	CHECK(lt_button_event(b, r, &e) == LT_W_IGNORED, "a click outside is ignored");
	lt_button_set_label(b, "Cancel");
	lt_button_draw(b, c, r, &th, false);
	char *bd2 = lt_canvas_dump(c);
	CHECK(strstr(bd2, "Cancel"), "set_label changes what is drawn");
	free(bd2);
	lt_button_free(b);

	/* checkbox / radio / switch: space and click toggle, the state persists */
	lt_check *ck = lt_check_new("enabled", LT_CHECKBOX, false);
	CHECK(!lt_check_checked(ck), "starts unchecked");
	e = key(' ');
	CHECK(lt_check_event(ck, r, &e) == LT_W_CHANGED && lt_check_checked(ck), "space toggles it on");
	e = key(' ');
	CHECK(lt_check_event(ck, r, &e) == LT_W_CHANGED && !lt_check_checked(ck), "space toggles it back off");
	lt_check_set(ck, true);
	CHECK(lt_check_checked(ck), "set() forces the state");
	lt_check_draw(ck, c, r, &th, true);
	char *chd = lt_canvas_dump(c);
	CHECK(strstr(chd, "enabled"), "the checkbox's label is drawn");
	free(chd);
	e = mouse(LT_K_CLICK, 0, 0);
	lt_wres cr = lt_check_event(ck, r, &e);
	CHECK(cr == LT_W_CHANGED || cr == LT_W_IGNORED, "a click on the box either toggles or is ignored, never crashes");
	lt_check_free(ck);
	lt_check *sw = lt_check_new("wifi", LT_SWITCH, false);
	lt_check_draw(sw, c, r, &th, false);                                        /* every kind must at least draw */
	lt_check_free(sw);
	lt_check *rd = lt_check_new("choice", LT_RADIO, true);
	lt_check_draw(rd, c, r, &th, false);
	lt_check_free(rd);

	/* radiogroup: arrows move, space/click chooses */
	const char *opts[3] = {"a", "b", "c"};
	lt_radiogroup *g = lt_radiogroup_new(opts, 3);
	CHECK(lt_radiogroup_selected(g) == 0, "starts on the first option");
	lt_rect gr = {0, 0, 10, 3};
	e = key(LT_K_DOWN);
	CHECK(lt_radiogroup_event(g, gr, &e) == LT_W_HANDLED && lt_radiogroup_selected(g) == 0, "Down only moves the cursor, the selection stays until chosen");
	e = key(' ');
	CHECK(lt_radiogroup_event(g, gr, &e) == LT_W_CHANGED && lt_radiogroup_selected(g) == 1, "Space chooses whatever the cursor is on");
	e = key(LT_K_DOWN); lt_radiogroup_event(g, gr, &e);
	e = key(LT_K_DOWN);                                                         /* past the end: clamps, must not crash */
	lt_radiogroup_event(g, gr, &e);
	e = key(' ');
	lt_radiogroup_event(g, gr, &e);
	CHECK(lt_radiogroup_selected(g) >= 0 && lt_radiogroup_selected(g) < 3, "the selection always stays in range");
	lt_radiogroup_set(g, 2);
	CHECK(lt_radiogroup_selected(g) == 2, "set() jumps directly");
	lt_radiogroup_draw(g, c, gr, &th, true);
	char *gd = lt_canvas_dump(c);
	CHECK(strstr(gd, "a") && strstr(gd, "b") && strstr(gd, "c"), "every option label is drawn");
	free(gd);
	lt_radiogroup_free(g);

	/* slider: keys step, Home/End jump, the value stays within [min,max], the label and value are drawn */
	lt_slider *s = lt_slider_new(0, 100, 5, 50);
	CHECK(lt_slider_value(s) == 50, "starts at the given value");
	lt_rect sr = {0, 0, 30, 1};
	e = key(LT_K_RIGHT);
	CHECK(lt_slider_event(s, sr, &e) == LT_W_CHANGED && lt_slider_value(s) == 55, "Right steps up by the step size");
	e = key(LT_K_LEFT);
	lt_slider_event(s, sr, &e);
	CHECK(lt_slider_value(s) == 50, "Left steps back down");
	e = key(LT_K_HOME);
	lt_slider_event(s, sr, &e);
	CHECK(lt_slider_value(s) == 0, "Home jumps to the minimum");
	e = key(LT_K_END);
	lt_slider_event(s, sr, &e);
	CHECK(lt_slider_value(s) == 100, "End jumps to the maximum");
	lt_slider_set(s, 200);                                                      /* out of range: must clamp, not crash */
	CHECK(lt_slider_value(s) <= 100 && lt_slider_value(s) >= 0, "set() clamps to [min,max]");
	e = mouse(LT_K_WHEEL_DOWN, 5, 0);
	lt_wres wr = lt_slider_event(s, sr, &e);
	CHECK(wr == LT_W_CHANGED || wr == LT_W_IGNORED, "wheel over the slider is handled without crashing");
	lt_slider_draw(s, c, sr, &th, true, "volume");
	char *sd = lt_canvas_dump(c);
	CHECK(strstr(sd, "volume"), "the slider's label is drawn");
	free(sd);
	lt_slider_free(s);

	lt_canvas_free(c);
	printf(fails ? "controls: %d failed\n" : "controls: ok\n", fails);
	return fails != 0;
}
