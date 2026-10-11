/* test_clock.c - lt_clock (digital/big/analog draw, never crashes on odd sizes), lt_timer (stopwatch, countdown, expiry, pause/resume/
 * reset, driven by a synthetic `now` instead of the real clock), lt_datepicker (calendar math across month/year boundaries, click/keys),
 * lt_timepicker (field cycling, wraparound, wheel). Headless. */
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
static struct tm mktm(int h, int m, int s) { struct tm tm = {0}; tm.tm_hour = h; tm.tm_min = m; tm.tm_sec = s; return tm; }

int main(void)
{
	lt_theme th = lt_theme_default();
	lt_canvas *c = lt_canvas_new(40, 20, UNI);

	/* ---- lt_clock: stateless, just draws whatever tm it's handed ---- */
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	lt_clock_draw(c, (lt_rect){0, 0, 20, 1}, &th, LT_CLOCK_DIGITAL, mktm(9, 5, 3), true);
	char *d = lt_canvas_dump(c);
	CHECK(strstr(d, "09:05:03"), "digital form shows zero-padded HH:MM:SS");
	free(d);
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	lt_clock_draw(c, (lt_rect){0, 0, 20, 1}, &th, LT_CLOCK_DIGITAL, mktm(23, 59, 0), false);
	d = lt_canvas_dump(c);
	CHECK(strstr(d, "23:59") && !strstr(d, "23:59:00"), "show_seconds=false drops the seconds field");
	free(d);
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	lt_clock_draw(c, (lt_rect){0, 0, 30, 5}, &th, LT_CLOCK_BIG, mktm(1, 2, 3), false);
	CHECK(lt_canvas_char(c, 0, 0) != 0, "big form draws something in the top-left of its block font");
	lt_clock_draw(c, (lt_rect){0, 0, 2, 5}, &th, LT_CLOCK_BIG, mktm(1, 2, 3), true);         /* too narrow for the big font: must fall back, not crash */
	lt_clock_draw(c, (lt_rect){0, 0, 30, 12}, &th, LT_CLOCK_ANALOG, mktm(3, 15, 45), false);
	lt_clock_draw(c, (lt_rect){0, 0, 1, 1}, &th, LT_CLOCK_ANALOG, mktm(3, 15, 45), false);   /* far too small: must not crash */
	lt_clock_draw(c, (lt_rect){0, 0, 0, 0}, &th, LT_CLOCK_DIGITAL, mktm(0, 0, 0), false);    /* zero-sized rect: must not crash */

	/* ---- lt_timer: a stopwatch (countdown == 0), driven by a synthetic `now` ---- */
	lt_timer *tm = lt_timer_new();
	CHECK(!lt_timer_running(tm), "a fresh timer is not running");
	CHECK(lt_timer_elapsed(tm, 100.0) == 0, "and has counted up 0 seconds regardless of `now`");
	lt_timer_start(tm, 100.0);
	CHECK(lt_timer_running(tm), "start makes it running");
	CHECK(fabs(lt_timer_elapsed(tm, 107.5) - 7.5) < 1e-9, "elapsed tracks now - start while running");
	lt_timer_pause(tm, 110.0);
	CHECK(!lt_timer_running(tm), "pause stops it");
	CHECK(fabs(lt_timer_elapsed(tm, 999.0) - 10.0) < 1e-9, "elapsed is frozen at the paused value regardless of later `now`");
	lt_timer_start(tm, 200.0);                                                  /* resume: banked time plus a new run */
	CHECK(fabs(lt_timer_elapsed(tm, 205.0) - 15.0) < 1e-9, "resuming adds the new run on top of the banked time");
	lt_timer_reset(tm, 205.0);
	CHECK(fabs(lt_timer_elapsed(tm, 205.0)) < 1e-9 && lt_timer_running(tm), "reset zeroes elapsed but keeps running if it was running");
	lt_timer_free(tm);

	/* ---- lt_timer: countdown mode + expiry ---- */
	lt_timer *cd = lt_timer_new();
	lt_timer_set_countdown(cd, 30.0);
	lt_timer_start(cd, 0.0);
	CHECK(fabs(lt_timer_elapsed(cd, 10.0) - 20.0) < 1e-9, "countdown elapsed() reports what's left, not what's passed");
	CHECK(!lt_timer_expired(cd, 10.0), "not expired with time left");
	CHECK(lt_timer_elapsed(cd, 45.0) == 0 && lt_timer_expired(cd, 45.0), "clamps to 0 and reports expired once past the target");
	lt_rect trect = {0, 0, 20, 2};
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	lt_timer_draw(cd, c, trect, &th, false, 45.0);                              /* expired: must draw without crashing (and paint a drained bar) */
	d = lt_canvas_dump(c);
	CHECK(strstr(d, "00:00"), "an expired countdown displays 00:00");
	free(d);
	lt_event e = key(' ');
	CHECK(lt_timer_event(cd, trect, &e, 45.0) == LT_W_CHANGED, "space toggles the timer");
	CHECK(!lt_timer_running(cd), "...and it was running (from set_countdown+start above), so space paused it");
	e = key('r');
	lt_timer_event(cd, trect, &e, 45.0);
	CHECK(fabs(lt_timer_elapsed(cd, 45.0) - 30.0) < 1e-9, "'r' resets the countdown back to its full duration");
	e = mouse(LT_K_CLICK, 5, 0);
	CHECK(lt_timer_event(cd, trect, &e, 45.0) == LT_W_CHANGED, "a click also toggles it");
	lt_timer_free(cd);

	/* ---- lt_timer_add_duration: dial the target without disturbing a run already in progress ---- */
	lt_timer *sw = lt_timer_new();                                             /* starts as a plain stopwatch */
	lt_timer_start(sw, 0.0);
	CHECK(fabs(lt_timer_elapsed(sw, 10.0) - 10.0) < 1e-9, "a stopwatch just counts up");
	lt_timer_add_duration(sw, 60.0);                                           /* now a countdown; the 10s already run still counts */
	CHECK(fabs(lt_timer_elapsed(sw, 10.0) - 50.0) < 1e-9, "add_duration turns a running stopwatch into a countdown without losing its elapsed time");
	lt_timer_add_duration(sw, -60.0);                                          /* target back to 0: a stopwatch again */
	CHECK(fabs(lt_timer_elapsed(sw, 10.0) - 10.0) < 1e-9, "dialing the duration back down to 0 turns it back into a stopwatch");
	lt_timer_add_duration(sw, -5.0);                                           /* dialing a stopwatch down must not go negative */
	CHECK(fabs(lt_timer_elapsed(sw, 10.0) - 10.0) < 1e-9, "add_duration never takes the countdown below 0");
	e = key(LT_K_UP);
	lt_timer_event(sw, trect, &e, 10.0);                                       /* target now 30s, 10s of which have already run: 20s left */
	CHECK(fabs(lt_timer_elapsed(sw, 10.0) - 20.0) < 1e-9, "Up adds 30s to the target");
	e = key(LT_K_PGDN);
	lt_timer_event(sw, trect, &e, 10.0);                                       /* -300s clamps the target back to 0: a stopwatch again */
	CHECK(fabs(lt_timer_elapsed(sw, 10.0) - 10.0) < 1e-9, "PgDn's -300s clamps the target back to a stopwatch instead of going negative");
	e = mouse(LT_K_WHEEL_UP, 5, 0);
	lt_timer_event(sw, trect, &e, 10.0);                                       /* target 30s again, still 10s in: 20s left */
	CHECK(fabs(lt_timer_elapsed(sw, 10.0) - 20.0) < 1e-9, "wheel-up over the timer also dials the duration up");
	lt_timer_free(sw);

	/* ---- lt_datepicker: own civil-calendar math ---- */
	lt_datepicker *dp = lt_datepicker_new(2026, 9, 27);
	int y, m, day;
	lt_datepicker_get(dp, &y, &m, &day);
	CHECK(y == 2026 && m == 9 && day == 27, "construction keeps the date given");
	lt_datepicker_set(dp, 2024, 2, 30);                                         /* Feb 2024 is a leap year: only 29 days */
	lt_datepicker_get(dp, &y, &m, &day);
	CHECK(y == 2024 && m == 2 && day == 29, "an out-of-range day is clamped to the month's real length (leap Feb)");
	lt_datepicker_set(dp, 2023, 2, 30);                                         /* 2023 is not a leap year: 28 days */
	lt_datepicker_get(dp, &y, &m, &day);
	CHECK(day == 28, "...and non-leap Feb clamps to 28");
	lt_datepicker_set(dp, 2026, 1, 31);
	lt_event ev = key(LT_K_RIGHT);
	lt_datepicker_event(dp, (lt_rect){0, 0, 21, 8}, &ev);
	lt_datepicker_get(dp, &y, &m, &day);
	CHECK(y == 2026 && m == 2 && day == 1, "Right from Jan 31 rolls into Feb 1");
	lt_datepicker_set(dp, 2026, 12, 31);
	ev = key(LT_K_RIGHT);
	lt_datepicker_event(dp, (lt_rect){0, 0, 21, 8}, &ev);
	lt_datepicker_get(dp, &y, &m, &day);
	CHECK(y == 2027 && m == 1 && day == 1, "Right from Dec 31 rolls into next January 1");
	lt_datepicker_set(dp, 2026, 1, 1);
	ev = key(LT_K_LEFT);
	lt_datepicker_event(dp, (lt_rect){0, 0, 21, 8}, &ev);
	lt_datepicker_get(dp, &y, &m, &day);
	CHECK(y == 2025 && m == 12 && day == 31, "Left from Jan 1 rolls back into last December");
	lt_datepicker_set(dp, 2026, 3, 31);
	ev = key(LT_K_PGUP);                                                        /* March 31 -> Feb, which has fewer days: must clamp */
	lt_datepicker_event(dp, (lt_rect){0, 0, 21, 8}, &ev);
	lt_datepicker_get(dp, &y, &m, &day);
	CHECK(y == 2026 && m == 2 && day == 28, "PgUp a month clamps the day into the shorter month");
	lt_datepicker_set(dp, 2026, 6, 15);
	ev = key(']');
	lt_datepicker_event(dp, (lt_rect){0, 0, 21, 8}, &ev);
	lt_datepicker_get(dp, &y, &m, &day);
	CHECK(y == 2027 && m == 6 && day == 15, "']' advances the year, same month/day");
	/* mouse: draw first so the grid rect is recorded, then click a real day cell */
	lt_datepicker_set(dp, 2026, 9, 1);                                          /* 2026-09-01 is a Tuesday: weekday()==2, so it's the 3rd column of row 0 */
	lt_rect dr = {0, 0, 21, 8};
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	lt_datepicker_draw(dp, c, dr, &th, true);
	ev = mouse(LT_K_CLICK, dr.x + 4 * 3, dr.y + 2);                             /* Sept 1 2026 is a Tuesday (weekday()==2): this cell is day 3 */
	lt_wres r1 = lt_datepicker_event(dp, dr, &ev);
	lt_datepicker_get(dp, &y, &m, &day);
	CHECK(r1 == LT_W_CHANGED && day != 1, "clicking a day cell in the drawn grid selects that day");
	ev = mouse(LT_K_CLICK, dr.x + 500, dr.y + 500);                             /* nowhere near the grid */
	CHECK(lt_datepicker_event(dp, dr, &ev) == LT_W_IGNORED, "a click far outside the grid is ignored");

	/* ---- clicking the "Month Year" / "Year" header is a mouse-discoverable shortcut for the same Tab-cycle ---- */
	lt_datepicker_set(dp, 2026, 9, 15);
	int hcx = dr.x + dr.w / 2;                                                  /* the header is centred on the rect, whatever its text width */
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	lt_datepicker_draw(dp, c, dr, &th, true);
	ev = mouse(LT_K_CLICK, hcx, dr.y);
	CHECK(lt_datepicker_event(dp, dr, &ev) == LT_W_CHANGED, "clicking the month/year header jumps into the month picker");
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	lt_datepicker_draw(dp, c, dr, &th, true);
	char *hd = lt_canvas_dump(c);
	CHECK(strstr(hd, "Jan") && strstr(hd, "Sep"), "...and it really is the month grid now");
	free(hd);
	ev = mouse(LT_K_CLICK, hcx, dr.y);                                          /* the lone "2026" header, same spot */
	CHECK(lt_datepicker_event(dp, dr, &ev) == LT_W_CHANGED, "clicking the year header from the month picker jumps into the year picker");
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	lt_datepicker_draw(dp, c, dr, &th, true);
	hd = lt_canvas_dump(c);
	CHECK(strstr(hd, "2026") && strstr(hd, "year"), "...and it really is the year picker now");
	free(hd);
	ev = mouse(LT_K_CLICK, hcx, dr.y + 3);                                      /* anywhere in the year picker, not just its header */
	CHECK(lt_datepicker_event(dp, dr, &ev) == LT_W_CHANGED, "a click anywhere in the year picker confirms it and returns to the day view");

	/* ---- Tab cycles the day grid into quick month- and year-pick views, same idiom as treeview's own modes ---- */
	lt_datepicker_set(dp, 2026, 9, 15);
	ev = key(LT_K_TAB);
	CHECK(lt_datepicker_event(dp, dr, &ev) == LT_W_HANDLED, "Tab from the day grid enters the month picker");
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	lt_datepicker_draw(dp, c, dr, &th, true);
	d = lt_canvas_dump(c);
	CHECK(strstr(d, "Sep") && strstr(d, "2026"), "the month picker shows every month abbreviation and the year");
	free(d);
	ev = key(LT_K_RIGHT);
	lt_datepicker_event(dp, dr, &ev);
	lt_datepicker_get(dp, &y, &m, &day);
	CHECK(m == 10 && day == 15, "Right in the month picker moves a month, keeping the day");
	ev = key(LT_K_ENTER);
	lt_datepicker_event(dp, dr, &ev);
	lt_datepicker_get(dp, &y, &m, &day);
	CHECK(m == 10, "Enter confirms and drops straight back to the day grid");
	ev = key(LT_K_TAB); lt_datepicker_event(dp, dr, &ev);                       /* back into the month picker */
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	lt_datepicker_draw(dp, c, dr, &th, true);                                   /* records the month grid's origin for the click below */
	ev = mouse(LT_K_CLICK, dr.x + 3 + 2 * 5, dr.y + 1);                         /* the grid centres itself in a 21-wide rect at x+3; row 0, col 2 = March (i=2) */
	lt_datepicker_event(dp, dr, &ev);
	lt_datepicker_get(dp, &y, &m, &day);
	CHECK(m == 3, "clicking a month in the picker jumps straight to it (and returns to the day grid)");
	ev = key(LT_K_TAB); lt_datepicker_event(dp, dr, &ev);                       /* day -> month */
	ev = key(LT_K_TAB); lt_datepicker_event(dp, dr, &ev);                       /* month -> year */
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	lt_datepicker_draw(dp, c, dr, &th, true);
	d = lt_canvas_dump(c);
	CHECK(strstr(d, "2026") && strstr(d, "year"), "the year picker shows the current year and its own hint");
	free(d);
	ev = key(LT_K_PGUP);
	lt_datepicker_event(dp, dr, &ev);
	lt_datepicker_get(dp, &y, &m, &day);
	CHECK(y == 2036, "PgUp in the year picker jumps +10 years");
	ev = key(LT_K_TAB);
	CHECK(lt_datepicker_event(dp, dr, &ev) == LT_W_HANDLED, "Tab from the year picker completes the cycle back to the day grid");
	lt_datepicker_set(dp, 2024, 2, 29);                                         /* leap day */
	ev = key(LT_K_TAB); lt_datepicker_event(dp, dr, &ev);                       /* into month picker */
	ev = key(LT_K_TAB); lt_datepicker_event(dp, dr, &ev);                       /* into year picker */
	ev = key(LT_K_DOWN);
	lt_datepicker_event(dp, dr, &ev);                                           /* 2024 -> 2023: no Feb 29, must clamp */
	lt_datepicker_get(dp, &y, &m, &day);
	CHECK(y == 2023 && day == 28, "stepping off a leap year clamps Feb 29 down to 28");

	/* ---- LT_DATE_COMPACT: one row of year/month/day field-spinner segments ---- */
	lt_datepicker_set_kind(dp, LT_DATE_COMPACT);
	lt_datepicker_set(dp, 2026, 9, 27);
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	lt_datepicker_draw(dp, c, dr, &th, true);
	d = lt_canvas_dump(c);
	CHECK(strstr(d, "2026-09-27"), "the compact form draws one YYYY-MM-DD row");
	free(d);
	ev = key(LT_K_UP);                                                          /* field 0 (year) is selected by default */
	lt_datepicker_event(dp, dr, &ev);
	lt_datepicker_get(dp, &y, &m, &day);
	CHECK(y == 2027, "Up on the year field bumps the year");
	ev = key(LT_K_RIGHT); lt_datepicker_event(dp, dr, &ev);                     /* -> month field */
	ev = key(LT_K_UP); lt_datepicker_event(dp, dr, &ev);
	lt_datepicker_get(dp, &y, &m, &day);
	CHECK(m == 10, "Right then Up moves to and bumps the month field");
	ev = mouse(LT_K_WHEEL_DOWN, dr.x + 9, dr.y);                                /* over the day segment */
	lt_datepicker_event(dp, dr, &ev);
	lt_datepicker_get(dp, &y, &m, &day);
	CHECK(day == 26, "wheel-down over the day segment steps the day directly, without selecting it first");
	lt_datepicker_free(dp);

	/* ---- lt_timepicker ---- */
	lt_timepicker *tp = lt_timepicker_new(9, 30, 0);
	int h, mi, s;
	lt_timepicker_get(tp, &h, &mi, &s);
	CHECK(h == 9 && mi == 30 && s == 0, "construction keeps the time given");
	lt_timepicker_set(tp, 25, 70, -3);
	lt_timepicker_get(tp, &h, &mi, &s);
	CHECK(h == 23 && mi == 59 && s == 0, "out-of-range fields are clamped to 0..23 / 0..59 / 0..59");
	lt_timepicker_set(tp, 5, 10, 20);
	lt_rect tpr = {0, 0, 10, 1};
	ev = key(LT_K_UP);
	lt_timepicker_event(tp, tpr, &ev);                                          /* field 0 = hours by default */
	lt_timepicker_get(tp, &h, &mi, &s);
	CHECK(h == 6, "Up bumps the currently selected field (hours)");
	ev = key(LT_K_RIGHT);
	lt_timepicker_event(tp, tpr, &ev);
	ev = key(LT_K_UP);
	lt_timepicker_event(tp, tpr, &ev);
	lt_timepicker_get(tp, &h, &mi, &s);
	CHECK(mi == 11, "Right moves to the next field (minutes), Up then bumps that one");
	lt_timepicker_set(tp, 5, 59, 0);                                            /* field is still 1 (minutes), from the "Right moves..." check above */
	ev = key(LT_K_UP); lt_timepicker_event(tp, tpr, &ev);
	lt_timepicker_get(tp, &h, &mi, &s);
	CHECK(mi == 0, "minutes wrap 59 -> 0 on Up");
	ev = key(LT_K_DOWN);
	lt_timepicker_event(tp, tpr, &ev);
	lt_timepicker_get(tp, &h, &mi, &s);
	CHECK(mi == 59, "and wrap back 0 -> 59 on Down");
	lt_timepicker_set_seconds(tp, true);
	lt_timepicker_set(tp, 5, 10, 20);
	ev = key(LT_K_LEFT);                                                        /* field was left at minutes (1); Left -> hours (0) */
	lt_timepicker_event(tp, tpr, &ev);
	ev = key(LT_K_LEFT);                                                        /* wraps back round to seconds (2) now that seconds is enabled */
	lt_timepicker_event(tp, tpr, &ev);
	ev = key(LT_K_UP);
	lt_timepicker_event(tp, tpr, &ev);
	lt_timepicker_get(tp, &h, &mi, &s);
	CHECK(s == 21, "Left wraps around to the seconds field once show_seconds is on, Up bumps seconds");
	ev = mouse(LT_K_WHEEL_UP, tpr.x + 1, tpr.y);                                /* over the hours field regardless of the current selection */
	lt_timepicker_event(tp, tpr, &ev);
	lt_timepicker_get(tp, &h, &mi, &s);
	CHECK(h == 6, "wheel-up over a field bumps that field directly, without needing to select it first");
	ev = mouse(LT_K_CLICK, tpr.x + 3, tpr.y);                                   /* over the minutes field */
	lt_timepicker_event(tp, tpr, &ev);
	ev = key(LT_K_UP);
	lt_timepicker_event(tp, tpr, &ev);
	lt_timepicker_get(tp, &h, &mi, &s);
	CHECK(mi == 11, "clicking a field selects it for subsequent keyboard Up/Down");
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	lt_timepicker_draw(tp, c, tpr, &th, true);
	d = lt_canvas_dump(c);
	CHECK(strstr(d, "06:11:21"), "draw shows the current HH:MM:SS");
	free(d);

	/* ---- LT_TIME_SPINBOX: a 3-row up-arrow/value/down-arrow column per field, falling back to plain fields when too short ---- */
	lt_timepicker_set_kind(tp, LT_TIME_SPINBOX);
	lt_rect spr = {0, 0, 10, 3};
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	lt_timepicker_draw(tp, c, spr, &th, true);
	d = lt_canvas_dump(c);
	CHECK(strstr(d, "06") && strstr(d, "11") && strstr(d, "21"), "the spinbox form still shows every field's value");
	free(d);
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	lt_timepicker_draw(tp, c, tpr, &th, true);                                  /* tpr is only 1 row tall: not enough room for 3 */
	d = lt_canvas_dump(c);
	CHECK(strstr(d, "06:11:21"), "spinbox falls back to the plain fields form when there isn't room for 3 rows");
	free(d);

	/* ---- LT_TIME_H12: FIELDS but 12-hour with a trailing AM/PM segment (still stored as 24h) ---- */
	lt_timepicker_set_kind(tp, LT_TIME_H12);
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	lt_timepicker_draw(tp, c, tpr, &th, true);
	d = lt_canvas_dump(c);
	CHECK(strstr(d, "06:11:21") && strstr(d, "AM"), "6 reads the same in both scales, shown here with an AM/PM segment");
	free(d);
	ev = key(LT_K_LEFT); lt_timepicker_event(tp, tpr, &ev);                     /* field was at minutes: -> hours */
	ev = key(LT_K_LEFT); lt_timepicker_event(tp, tpr, &ev);                     /* -> wraps round to the new AM/PM segment */
	ev = key(LT_K_UP); lt_timepicker_event(tp, tpr, &ev);                       /* any nudge on it just flips AM/PM */
	lt_timepicker_get(tp, &h, &mi, &s);
	CHECK(h == 18, "nudging the AM/PM segment flips between AM and PM (still stored as 24h internally)");
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	lt_timepicker_draw(tp, c, tpr, &th, true);
	d = lt_canvas_dump(c);
	CHECK(strstr(d, "06:11:21") && strstr(d, "PM"), "the display still reads 06 (12h) but now shows PM");
	free(d);
	ev = mouse(LT_K_WHEEL_DOWN, tpr.x + 9, tpr.y);                              /* over the AM/PM segment */
	lt_timepicker_event(tp, tpr, &ev);
	lt_timepicker_get(tp, &h, &mi, &s);
	CHECK(h == 6, "wheel over the AM/PM segment flips it back too, without needing to select it first");
	lt_timepicker_free(tp);

	lt_canvas_free(c);
	printf(fails ? "clock: %d failed\n" : "clock: ok\n", fails);
	return fails != 0;
}
