/* timer.c - Lt_timer: a stopwatch (countdown == 0) or a countdown, driven entirely by the `now` (an lt_now() reading) the caller passes to every call - the widget itself never touches the clock.
 * Needs: lazytui.h (the lib core). One widget, one file: copy it into any project that links the core. */
#include "internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct lt_timer {
	double countdown;                 /* 0 = stopwatch (counts up); > 0 = counts down from this many seconds */
	double banked;                    /* seconds accumulated across previous start/pause cycles */
	double started;                   /* `now` reading when the current run began; < 0 while paused */
};

lt_timer *lt_timer_new(void)
{
	lt_timer *tm = calloc(1, sizeof *tm);
	tm->started = -1;
	return tm;
}
void lt_timer_free(lt_timer *tm) { free(tm); }

void lt_timer_set_countdown(lt_timer *tm, double seconds)
{
	tm->countdown = isfinite(seconds) && seconds > 0 ? seconds : 0;
	tm->banked = 0;
	tm->started = -1;
}
void lt_timer_start(lt_timer *tm, double now) { if (tm->started < 0) tm->started = now; }
void lt_timer_pause(lt_timer *tm, double now)
{
	if (tm->started < 0) return;
	tm->banked += now - tm->started;
	tm->started = -1;
}
void lt_timer_toggle(lt_timer *tm, double now) { if (tm->started < 0) lt_timer_start(tm, now); else lt_timer_pause(tm, now); }
void lt_timer_reset(lt_timer *tm, double now)
{
	tm->banked = 0;
	if (tm->started >= 0) tm->started = now;               /* stays running if it was, just back to zero elapsed */
}
bool lt_timer_running(const lt_timer *tm) { return tm->started >= 0; }

static double raw_elapsed(const lt_timer *tm, double now) { return tm->banked + (tm->started >= 0 ? now - tm->started : 0); }

double lt_timer_elapsed(const lt_timer *tm, double now)
{
	double e = raw_elapsed(tm, now);
	if (tm->countdown <= 0) return e;
	double rem = tm->countdown - e;
	return rem > 0 ? rem : 0;
}
bool lt_timer_expired(const lt_timer *tm, double now) { return tm->countdown > 0 && raw_elapsed(tm, now) >= tm->countdown; }

/* dials the target duration up or down without touching banked/started - unlike lt_timer_set_countdown this never resets an already-
 * running timer's progress, so it is safe to call while it counts down. A stopwatch (countdown == 0) becomes a countdown the first time
 * this pushes it above 0; bringing a countdown back down to 0 turns it back into a plain stopwatch. */
void lt_timer_add_duration(lt_timer *tm, double delta)
{
	if (!isfinite(delta)) return;
	tm->countdown = tm->countdown + delta > 0 ? tm->countdown + delta : 0;
}

void lt_timer_draw(const lt_timer *tm, lt_canvas *c, lt_rect r, const lt_theme *t, bool focused, double now)
{
	if (r.w < 5 || r.h < 1) return;
	lt_rect all = lt_full(c);
	bool uni = lt_canvas_caps(c)->unicode;
	double show = lt_timer_elapsed(tm, now);
	bool expired = lt_timer_expired(tm, now);
	int hh = (int)(show / 3600), mm = (int)(show / 60) % 60, ss = (int)show % 60;
	char buf[24];
	if (hh > 0) snprintf(buf, sizeof buf, "%d:%02d:%02d", hh, mm, ss);
	else snprintf(buf, sizeof buf, "%02d:%02d", mm, ss);
	lt_style st = expired ? t->s[LT_T_ERR] : focused ? t->s[LT_T_SELECT] : t->s[LT_T_ACCENT];
	lt_text(c, all, r.x, r.y, buf, LT_STYLE(st.fg, LT_DEFAULT, LT_BOLD), r.w);
	int bw = (int)strlen(buf);
	if (r.w > bw + 2) {
		bool running = lt_timer_running(tm);
		lt_put(c, all, r.x + r.w - 1, r.y, running ? (uni ? 0x25B6 : '>') : (uni ? 0x23F8 : '|'), st);
	}
	if (r.h >= 2 && tm->countdown > 0) {
		double frac = show / tm->countdown;                 /* drains as time runs out */
		lt_progressbar(c, (lt_rect){r.x, r.y + 1, r.w, 1}, t, frac < 0 ? 0 : frac > 1 ? 1 : frac, NULL, false);
	}
}

lt_wres lt_timer_event(lt_timer *tm, lt_rect r, const lt_event *ev, double now)
{
	if (ev->kind == LT_EV_MOUSE) {
		int base = ev->key & 0xFFFFFF;
		if (base == LT_K_CLICK && lt_in(r, ev->x, ev->y)) { lt_timer_toggle(tm, now); return LT_W_CHANGED; }
		if ((base == LT_K_WHEEL_UP || base == LT_K_WHEEL_DOWN) && lt_in(r, ev->x, ev->y)) {
			lt_timer_add_duration(tm, base == LT_K_WHEEL_UP ? 30 : -30);
			return LT_W_CHANGED;
		}
		return LT_W_IGNORED;
	}
	if (ev->kind != LT_EV_KEY || (ev->key & (LT_MOD_CTRL | LT_MOD_ALT))) return LT_W_IGNORED;
	switch (ev->key & 0xFFFFFF) {
	case ' ': case LT_K_ENTER: lt_timer_toggle(tm, now); return LT_W_CHANGED;
	case 'r': lt_timer_reset(tm, now); return LT_W_CHANGED;
	case LT_K_UP: lt_timer_add_duration(tm, 30); return LT_W_CHANGED;              /* dial the target duration: +-30s, PgUp/PgDn +-5min */
	case LT_K_DOWN: lt_timer_add_duration(tm, -30); return LT_W_CHANGED;
	case LT_K_PGUP: lt_timer_add_duration(tm, 300); return LT_W_CHANGED;
	case LT_K_PGDN: lt_timer_add_duration(tm, -300); return LT_W_CHANGED;
	}
	return LT_W_IGNORED;
}
