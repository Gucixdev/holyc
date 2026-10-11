/* test_sequencer.c - the step sequencer: get/set/toggle/clear, track names, playhead, grouped-step hit-testing (mouse and keyboard), and
 * the 'r' shortcut. Headless. */
#include "lazytui.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); fails++; } } while (0)
static lt_caps UNI = {true, true, false, true, 256};
static lt_event key(int k) { return (lt_event){LT_EV_KEY, k, 0, 0, 0, NULL}; }
static lt_event mouse(int k, int x, int y) { return (lt_event){LT_EV_MOUSE, k, x, y, 0, NULL}; }

int main(void)
{
	lt_theme th = lt_theme_default();
	lt_canvas *c = lt_canvas_new(60, 10, UNI);

	lt_sequencer *sq = lt_sequencer_new(4, 16);
	CHECK(lt_sequencer_tracks(sq) == 4 && lt_sequencer_steps(sq) == 16, "the size given at construction is kept");
	CHECK(!lt_sequencer_get(sq, 0, 0), "a fresh pattern starts with every step off");
	lt_sequencer_set(sq, 1, 3, true);
	CHECK(lt_sequencer_get(sq, 1, 3) && !lt_sequencer_get(sq, 1, 4), "set turns on exactly the step asked for");
	lt_sequencer_toggle(sq, 1, 3);
	CHECK(!lt_sequencer_get(sq, 1, 3), "toggle flips it back off");
	lt_sequencer_toggle(sq, 2, 5);
	lt_sequencer_clear(sq);
	CHECK(!lt_sequencer_get(sq, 2, 5), "clear turns every step off");
	CHECK(!lt_sequencer_get(sq, -1, 0) && !lt_sequencer_get(sq, 0, 99), "an out-of-range track/step reads as off, not a crash");
	lt_sequencer_set(sq, -1, 0, true);
	lt_sequencer_set(sq, 0, 99, true);                                          /* out-of-range writes must not crash either */

	lt_sequencer_set_track_name(sq, 0, "Kick");
	CHECK(!strcmp(lt_sequencer_track_name(sq, 0), "Kick"), "a track name round-trips");
	CHECK(lt_sequencer_track_name(sq, 1) == NULL, "an unnamed track has no name");
	lt_sequencer_set_track_name(sq, 0, "Snare");
	CHECK(!strcmp(lt_sequencer_track_name(sq, 0), "Snare"), "setting a new name replaces the old one, not leaks it (run under asan)");

	CHECK(lt_sequencer_playhead(sq) == -1, "no playhead by default");
	lt_sequencer_set_playhead(sq, 4);
	CHECK(lt_sequencer_playhead(sq) == 4, "set_playhead round-trips");

	CHECK(lt_sequencer_cursor_step(sq) == 0 && lt_sequencer_cursor_track(sq) == 0, "the keyboard cursor starts at the origin");

	/* draw, then click - grouping adds a one-column gap every 4 steps, so step 4 does not start right after step 3. Track 0 is named
	 * "Snare" (5 wide), so the grid itself starts 6 columns in (the label column + a gap), not at r.x. */
	lt_sequencer_set_group(sq, 4);
	lt_rect r = {0, 0, 60, 4};
	int gx = r.x + 6;
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	lt_sequencer_draw(sq, c, r, &th, true);
	lt_event e = mouse(LT_K_CLICK, gx, r.y);                                    /* track 0, step 0 */
	CHECK(lt_sequencer_event(sq, r, &e) == LT_W_CHANGED && lt_sequencer_get(sq, 0, 0), "clicking a cell toggles that step on");
	e = mouse(LT_K_CLICK, gx, r.y);
	CHECK(lt_sequencer_event(sq, r, &e) == LT_W_CHANGED && !lt_sequencer_get(sq, 0, 0), "clicking it again toggles it back off");
	e = mouse(LT_K_CLICK, gx + 2 * 4 + 1, r.y + 1);                             /* step 4 (2 cols/step * 4 steps + the group-4 gap), track 1 */
	CHECK(lt_sequencer_event(sq, r, &e) == LT_W_CHANGED && lt_sequencer_get(sq, 1, 4), "a click past a group gap lands on the right step, not one column short");
	e = mouse(LT_K_CLICK, r.x + 1000, r.y);                                     /* nowhere near the grid */
	CHECK(lt_sequencer_event(sq, r, &e) == LT_W_IGNORED, "a click far outside the grid is ignored");

	/* keyboard: arrows move the cursor, Enter/Space toggles, 'r' clears */
	lt_sequencer_clear(sq);
	e = key(LT_K_HOME); lt_sequencer_event(sq, r, &e);                          /* the clicks above left the cursor wherever they last landed */
	e = key(LT_K_UP); lt_sequencer_event(sq, r, &e);
	e = key(LT_K_UP); lt_sequencer_event(sq, r, &e);
	e = key(LT_K_RIGHT); lt_sequencer_event(sq, r, &e);
	e = key(LT_K_RIGHT); lt_sequencer_event(sq, r, &e);
	e = key(LT_K_DOWN); lt_sequencer_event(sq, r, &e);
	CHECK(lt_sequencer_cursor_step(sq) == 2 && lt_sequencer_cursor_track(sq) == 1, "arrows move the keyboard cursor");
	e = key(LT_K_ENTER);
	CHECK(lt_sequencer_event(sq, r, &e) == LT_W_CHANGED && lt_sequencer_get(sq, 1, 2), "Enter toggles the step at the cursor");
	e = key(' ');
	CHECK(lt_sequencer_event(sq, r, &e) == LT_W_CHANGED && !lt_sequencer_get(sq, 1, 2), "Space toggles it too (back off, in this case)");
	e = key(LT_K_HOME); lt_sequencer_event(sq, r, &e);
	CHECK(lt_sequencer_cursor_step(sq) == 0, "Home returns the cursor to step 0");
	e = key(LT_K_END); lt_sequencer_event(sq, r, &e);
	CHECK(lt_sequencer_cursor_step(sq) == 15, "End moves it to the last step");
	lt_sequencer_set(sq, 0, 0, true);
	lt_sequencer_set(sq, 3, 15, true);
	e = key('r');
	CHECK(lt_sequencer_event(sq, r, &e) == LT_W_CHANGED && !lt_sequencer_get(sq, 0, 0) && !lt_sequencer_get(sq, 3, 15), "'r' clears the whole pattern");

	lt_sequencer_draw(sq, c, (lt_rect){0, 0, 1, 1}, &th, true);                 /* far too small: must not crash */
	lt_sequencer_free(sq);
	lt_canvas_free(c);
	printf(fails ? "sequencer: %d failed\n" : "sequencer: ok\n", fails);
	return fails != 0;
}
