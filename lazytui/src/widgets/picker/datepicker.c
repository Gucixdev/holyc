/* datepicker.c - Lt_datepicker: a one-month calendar grid. Its own civil-calendar math (Howard Hinnant's days_from_civil) instead of mktime/localtime, so it works for any year without touching libc's TZ handling.
 * Needs: lazytui.h (the lib core). One widget, one file: copy it into any project that links the core. */
#include "internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static bool leapyear(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static int mdays(int y, int m)
{
	static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	return m == 2 && leapyear(y) ? 29 : d[m - 1];
}
static long civil_days(int y, int m, int d)
{
	y -= m <= 2;
	long era = (y >= 0 ? y : y - 399) / 400;
	unsigned yoe = (unsigned)(y - era * 400);
	unsigned doy = (153 * (unsigned)(m + (m > 2 ? -3 : 9)) + 2) / 5 + (unsigned)d - 1;
	unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + (long)doe - 719468;
}
static int weekday(int y, int m, int d) { long wd = (civil_days(y, m, d) + 4) % 7; return (int)(wd < 0 ? wd + 7 : wd); }   /* 0 = Sunday */

enum { DP_DAY, DP_MONTH, DP_YEAR };                 /* GRID kind only, Tab-cycled like treeview's own modes */

struct lt_datepicker {
	int y, m, d;
	lt_datepicker_kind kind;
	int view;                           /* GRID only: DP_DAY/DP_MONTH/DP_YEAR */
	int field;                          /* COMPACT only: 0 year, 1 month, 2 day */
	int gx, gy0, first_wd, dim;         /* the last-drawn day grid: origin, first day's weekday column, days in the month */
	int mgx, mgy;                       /* the last-drawn month-pick grid's origin (4 rows x 3 cols) */
	lt_rect hdr;                        /* the last-drawn "September 2026" / year header text - click it to jump into month/year picking */
};

static void clampday(lt_datepicker *d) { int md = mdays(d->y, d->m); if (d->d < 1) d->d = 1; else if (d->d > md) d->d = md; }

lt_datepicker *lt_datepicker_new(int year, int month, int day)
{
	lt_datepicker *d = calloc(1, sizeof *d);
	lt_datepicker_set(d, year, month, day);
	return d;
}
void lt_datepicker_free(lt_datepicker *d) { free(d); }
void lt_datepicker_get(const lt_datepicker *d, int *year, int *month, int *day) { if (year) *year = d->y; if (month) *month = d->m; if (day) *day = d->d; }
void lt_datepicker_set(lt_datepicker *d, int year, int month, int day)
{
	d->y = year;
	d->m = month < 1 ? 1 : month > 12 ? 12 : month;
	d->d = day;
	clampday(d);
}
void lt_datepicker_set_kind(lt_datepicker *d, lt_datepicker_kind k) { d->kind = k; d->view = DP_DAY; d->field = 0; }

static void add_days(lt_datepicker *d, int n)
{
	int step = n > 0 ? 1 : -1;
	for (int i = 0; i < (n < 0 ? -n : n); i++) {
		d->d += step;
		if (d->d < 1) { d->m--; if (d->m < 1) { d->m = 12; d->y--; } d->d = mdays(d->y, d->m); }
		else if (d->d > mdays(d->y, d->m)) { d->d = 1; d->m++; if (d->m > 12) { d->m = 1; d->y++; } }
	}
}
static void add_months(lt_datepicker *d, int n)
{
	long total = (long)d->y * 12 + (d->m - 1) + n;
	long y = total / 12, m = total % 12;
	if (m < 0) { m += 12; y--; }
	d->y = (int)y; d->m = (int)m + 1;
	clampday(d);
}

static const char *const MONTH_NAME[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
static const char *const MONTH_ABBR[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
static const char *const DOW[] = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};

static void draw_day(lt_datepicker *d, lt_canvas *c, lt_rect r, const lt_theme *t, bool focused)
{
	if (r.w < 21 || r.h < 3) return;
	lt_rect all = lt_full(c);
	char hdr[32];
	snprintf(hdr, sizeof hdr, "%s %d", MONTH_NAME[d->m - 1], d->y);
	int hw = (int)strlen(hdr), hx = r.x + (r.w - hw) / 2;
	if (hx < r.x) hx = r.x;
	lt_text(c, all, hx, r.y, hdr, t->s[LT_T_TITLE], r.w);
	d->hdr = (lt_rect){hx, r.y, hw, 1};                     /* click it to jump straight into the month picker */
	int gx = r.x + (r.w - 20) / 2;
	if (gx < r.x) gx = r.x;
	int gy = r.y + 1;
	for (int i = 0; i < 7 && gy < r.y + r.h; i++) lt_text(c, all, gx + i * 3, gy, DOW[i], t->s[LT_T_DIM], 2);
	d->gx = gx; d->gy0 = gy + 1; d->first_wd = weekday(d->y, d->m, 1); d->dim = mdays(d->y, d->m);
	int row = 0, col = d->first_wd;
	for (int day = 1; day <= d->dim; day++) {
		int y = d->gy0 + row;
		if (y >= r.y + r.h) break;
		char num[8];
		snprintf(num, sizeof num, "%2d", day % 100);          /* always 1..31; the %100 just gives gcc a provable bound to stop warning */
		bool sel = day == d->d;
		lt_text(c, all, gx + col * 3, y, num, sel ? (focused ? t->s[LT_T_SELECT] : t->s[LT_T_ACCENT]) : t->s[LT_T_TEXT], 2);
		if (++col > 6) { col = 0; row++; }
	}
	int fy = d->gy0 + row + (col > 0 ? 1 : 0);
	if (fy < r.y + r.h) lt_text(c, all, r.x, fy, "(Tab or click the month/year: pick it directly)", t->s[LT_T_DIM], r.w);
}
static lt_wres event_day(lt_datepicker *d, const lt_event *ev)
{
	if (ev->kind == LT_EV_MOUSE) {
		if ((ev->key & 0xFFFFFF) != LT_K_CLICK) return LT_W_IGNORED;
		if (lt_in(d->hdr, ev->x, ev->y)) { d->view = DP_MONTH; return LT_W_CHANGED; }        /* click "September 2026" to jump into picking */
		int col = (ev->x - d->gx) / 3, row = ev->y - d->gy0;
		if (col < 0 || col > 6 || row < 0) return LT_W_IGNORED;
		int day = row * 7 + col - d->first_wd + 1;
		if (day < 1 || day > d->dim) return LT_W_IGNORED;
		if (day == d->d) return LT_W_HANDLED;
		d->d = day;
		return LT_W_CHANGED;
	}
	if (ev->kind != LT_EV_KEY || (ev->key & (LT_MOD_CTRL | LT_MOD_ALT))) return LT_W_IGNORED;
	switch (ev->key & 0xFFFFFF) {
	case LT_K_LEFT: add_days(d, -1); return LT_W_CHANGED;
	case LT_K_RIGHT: add_days(d, 1); return LT_W_CHANGED;
	case LT_K_UP: add_days(d, -7); return LT_W_CHANGED;
	case LT_K_DOWN: add_days(d, 7); return LT_W_CHANGED;
	case LT_K_PGUP: add_months(d, -1); return LT_W_CHANGED;
	case LT_K_PGDN: add_months(d, 1); return LT_W_CHANGED;
	case '[': d->y--; clampday(d); return LT_W_CHANGED;
	case ']': d->y++; clampday(d); return LT_W_CHANGED;
	case LT_K_HOME: d->d = 1; return LT_W_CHANGED;
	case LT_K_END: d->d = d->dim; return LT_W_CHANGED;
	case LT_K_TAB: d->view = DP_MONTH; return LT_W_HANDLED;                 /* starts the month/year quick-pick cycle */
	}
	return LT_W_IGNORED;
}

/* the month/year quick-pick views: Tab cycles day -> month -> year -> day, same idiom as treeview's own mode-cycling */
static void draw_month_pick(lt_datepicker *d, lt_canvas *c, lt_rect r, const lt_theme *t, bool focused)
{
	lt_rect all = lt_full(c);
	char hdr[16];
	snprintf(hdr, sizeof hdr, "%d", d->y);
	int hw = (int)strlen(hdr), hx = r.x + (r.w - hw) / 2;
	if (hx < r.x) hx = r.x;
	lt_text(c, all, hx, r.y, hdr, t->s[LT_T_TITLE], r.w);
	d->hdr = (lt_rect){hx, r.y, hw, 1};                     /* click the year to jump straight into the year picker */
	int gx = r.x + (r.w - 15) / 2;
	if (gx < r.x) gx = r.x;
	int gy = r.y + 1;
	d->mgx = gx; d->mgy = gy;
	for (int i = 0; i < 12 && gy + i / 3 < r.y + r.h; i++) {
		int row = i / 3, col = i % 3;
		bool sel = (i + 1) == d->m;
		lt_text(c, all, gx + col * 5, gy + row, MONTH_ABBR[i], sel ? (focused ? t->s[LT_T_SELECT] : t->s[LT_T_ACCENT]) : t->s[LT_T_TEXT], 3);
	}
	if (gy + 4 < r.y + r.h) lt_text(c, all, r.x, gy + 4, "Enter: day view, Tab: pick year", t->s[LT_T_DIM], r.w);
}
static lt_wres event_month_pick(lt_datepicker *d, const lt_event *ev)
{
	if (ev->kind == LT_EV_MOUSE) {
		if ((ev->key & 0xFFFFFF) != LT_K_CLICK) return LT_W_IGNORED;
		if (lt_in(d->hdr, ev->x, ev->y)) { d->view = DP_YEAR; return LT_W_CHANGED; }          /* click the year to jump into the year picker */
		int col = (ev->x - d->mgx) / 5, row = ev->y - d->mgy;
		if (col < 0 || col > 2 || row < 0 || row > 3) return LT_W_IGNORED;
		int i = row * 3 + col;
		if (i < 0 || i > 11) return LT_W_IGNORED;
		d->m = i + 1;
		clampday(d);
		d->view = DP_DAY;
		return LT_W_CHANGED;
	}
	if (ev->kind != LT_EV_KEY || (ev->key & (LT_MOD_CTRL | LT_MOD_ALT))) return LT_W_IGNORED;
	switch (ev->key & 0xFFFFFF) {
	case LT_K_LEFT: add_months(d, -1); return LT_W_CHANGED;
	case LT_K_RIGHT: add_months(d, 1); return LT_W_CHANGED;
	case LT_K_UP: add_months(d, -3); return LT_W_CHANGED;
	case LT_K_DOWN: add_months(d, 3); return LT_W_CHANGED;
	case LT_K_ENTER: d->view = DP_DAY; return LT_W_CHANGED;
	case LT_K_TAB: d->view = DP_YEAR; return LT_W_HANDLED;
	}
	return LT_W_IGNORED;
}
static void draw_year_pick(lt_datepicker *d, lt_canvas *c, lt_rect r, const lt_theme *t, bool focused)
{
	lt_rect all = lt_full(c);
	char buf[16];
	snprintf(buf, sizeof buf, "%d", d->y);
	int w = (int)strlen(buf), x = r.x + (r.w - w) / 2;
	lt_text(c, all, x < r.x ? r.x : x, r.y + r.h / 2, buf, focused ? t->s[LT_T_SELECT] : t->s[LT_T_ACCENT], r.w);
	lt_text(c, all, r.x, r.y, "pick a year: Up/Down +-1, PgUp/PgDn +-10", t->s[LT_T_DIM], r.w);
	if (r.h > 2) lt_text(c, all, r.x, r.y + r.h - 1, "Enter/Tab: back to day view", t->s[LT_T_DIM], r.w);
}
static lt_wres event_year_pick(lt_datepicker *d, const lt_event *ev)
{
	if (ev->kind == LT_EV_MOUSE) {
		if ((ev->key & 0xFFFFFF) != LT_K_CLICK) return LT_W_IGNORED;
		d->view = DP_DAY;                                                       /* a click anywhere confirms and returns, like Enter */
		return LT_W_CHANGED;
	}
	if (ev->kind != LT_EV_KEY || (ev->key & (LT_MOD_CTRL | LT_MOD_ALT))) return LT_W_IGNORED;
	switch (ev->key & 0xFFFFFF) {
	case LT_K_UP: case LT_K_RIGHT: d->y++; clampday(d); return LT_W_CHANGED;
	case LT_K_DOWN: case LT_K_LEFT: d->y--; clampday(d); return LT_W_CHANGED;
	case LT_K_PGUP: d->y += 10; clampday(d); return LT_W_CHANGED;
	case LT_K_PGDN: d->y -= 10; clampday(d); return LT_W_CHANGED;
	case LT_K_ENTER: d->view = DP_DAY; return LT_W_CHANGED;
	case LT_K_TAB: d->view = DP_DAY; return LT_W_HANDLED;                   /* completes the day -> month -> year -> day cycle */
	}
	return LT_W_IGNORED;
}

/* the COMPACT kind: one row, "YYYY-MM-DD" as three field-spinner segments - same interaction language as lt_timepicker */
static void draw_compact(lt_datepicker *d, lt_canvas *c, lt_rect r, const lt_theme *t, bool focused)
{
	lt_rect all = lt_full(c);
	char buf[8];
	int x = r.x;
	snprintf(buf, sizeof buf, "%04d", d->y);
	lt_text(c, all, x, r.y, buf, (focused && d->field == 0) ? t->s[LT_T_SELECT] : t->s[LT_T_TEXT], 4); x += 4;
	lt_put(c, all, x, r.y, '-', t->s[LT_T_DIM]); x += 1;
	snprintf(buf, sizeof buf, "%02d", d->m);
	lt_text(c, all, x, r.y, buf, (focused && d->field == 1) ? t->s[LT_T_SELECT] : t->s[LT_T_TEXT], 2); x += 2;
	lt_put(c, all, x, r.y, '-', t->s[LT_T_DIM]); x += 1;
	snprintf(buf, sizeof buf, "%02d", d->d);
	lt_text(c, all, x, r.y, buf, (focused && d->field == 2) ? t->s[LT_T_SELECT] : t->s[LT_T_TEXT], 2);
}
static lt_wres event_compact(lt_datepicker *d, lt_rect r, const lt_event *ev)
{
	if (ev->kind == LT_EV_MOUSE) {
		int base = ev->key & 0xFFFFFF;
		if (base == LT_K_CLICK && lt_in(r, ev->x, ev->y)) {
			int rel = ev->x - r.x;
			d->field = rel < 5 ? 0 : rel < 8 ? 1 : 2;
			return LT_W_HANDLED;
		}
		if ((base == LT_K_WHEEL_UP || base == LT_K_WHEEL_DOWN) && lt_in(r, ev->x, ev->y)) {
			bool up = base == LT_K_WHEEL_UP;
			int rel = ev->x - r.x, f = rel < 5 ? 0 : rel < 8 ? 1 : 2;    /* which segment is under the pointer, not which was last selected */
			if (f == 0) { d->y += up ? 1 : -1; clampday(d); }
			else if (f == 1) add_months(d, up ? 1 : -1);
			else add_days(d, up ? 1 : -1);
			return LT_W_CHANGED;
		}
		return LT_W_IGNORED;
	}
	if (ev->kind != LT_EV_KEY || (ev->key & (LT_MOD_CTRL | LT_MOD_ALT))) return LT_W_IGNORED;
	switch (ev->key & 0xFFFFFF) {
	case LT_K_LEFT: d->field = (d->field + 2) % 3; return LT_W_HANDLED;
	case LT_K_RIGHT: case LT_K_TAB: d->field = (d->field + 1) % 3; return LT_W_HANDLED;
	case LT_K_UP:
		if (d->field == 0) { d->y++; clampday(d); } else if (d->field == 1) add_months(d, 1); else add_days(d, 1);
		return LT_W_CHANGED;
	case LT_K_DOWN:
		if (d->field == 0) { d->y--; clampday(d); } else if (d->field == 1) add_months(d, -1); else add_days(d, -1);
		return LT_W_CHANGED;
	}
	return LT_W_IGNORED;
}

void lt_datepicker_draw(lt_datepicker *d, lt_canvas *c, lt_rect r, const lt_theme *t, bool focused)
{
	if (r.w < 5 || r.h < 1) return;
	if (d->kind == LT_DATE_COMPACT) { draw_compact(d, c, r, t, focused); return; }
	if (d->view == DP_MONTH) { if (r.w >= 15 && r.h >= 4) draw_month_pick(d, c, r, t, focused); return; }
	if (d->view == DP_YEAR) { draw_year_pick(d, c, r, t, focused); return; }
	draw_day(d, c, r, t, focused);
}

lt_wres lt_datepicker_event(lt_datepicker *d, lt_rect r, const lt_event *ev)
{
	if (d->kind == LT_DATE_COMPACT) return event_compact(d, r, ev);
	if (d->view == DP_MONTH) return event_month_pick(d, ev);
	if (d->view == DP_YEAR) return event_year_pick(d, ev);
	return event_day(d, ev);
}
