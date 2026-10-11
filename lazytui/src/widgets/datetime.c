/* datetime.c - everything about telling and picking time, in one file: lt_clock (wall time, three display forms: digital / big block digits /
 * analog face), lt_timer (stopwatch or countdown, driven by a `now` the caller passes in), lt_datepicker (a one-month calendar grid, own
 * civil-calendar math so it never touches libc's TZ handling) and lt_timepicker (an HH:MM[:SS] field spinner). All four keep to the rest of
 * this library's rule that time is data the caller feeds in, not something a widget reaches for itself - which is also what makes them
 * trivial to test with a synthetic clock instead of the real one. */
#include "internal.h"
#include <stdio.h>
#include <time.h>

/* ================================================================== lt_clock: wall time in three forms, purely a function of "when" */

static void draw_digital(lt_canvas *c, lt_rect r, const lt_theme *t, struct tm when, bool seconds)
{
	lt_rect all = lt_full(c);
	char buf[16];
	if (seconds) snprintf(buf, sizeof buf, "%02d:%02d:%02d", when.tm_hour, when.tm_min, when.tm_sec);
	else snprintf(buf, sizeof buf, "%02d:%02d", when.tm_hour, when.tm_min);
	int w = (int)strlen(buf), x = r.x + (r.w - w) / 2, y = r.y + r.h / 2;
	if (x < r.x) x = r.x;
	lt_text(c, all, x, y, buf, LT_STYLE(t->s[LT_T_ACCENT].fg, LT_DEFAULT, LT_BOLD), r.w);
}

/* a plain 3-wide x 5-tall block font, '#' = lit; the colon is its own 1-wide glyph */
static const char *const BIGDIGIT[10][5] = {
	{"###", "# #", "# #", "# #", "###"}, {"  #", "  #", "  #", "  #", "  #"}, {"###", "  #", "###", "#  ", "###"},
	{"###", "  #", "###", "  #", "###"}, {"# #", "# #", "###", "  #", "  #"}, {"###", "#  ", "###", "  #", "###"},
	{"###", "#  ", "###", "# #", "###"}, {"###", "  #", "  #", "  #", "  #"}, {"###", "# #", "###", "# #", "###"},
	{"###", "# #", "###", "  #", "###"},
};
static const char *const BIGCOLON[5] = {" ", "#", " ", "#", " "};

static void draw_big(lt_canvas *c, lt_rect r, const lt_theme *t, struct tm when, bool seconds)
{
	lt_rect all = lt_full(c);
	bool uni = lt_canvas_caps(c)->unicode;
	int digit[9], nd = 0;
	digit[nd++] = when.tm_hour / 10; digit[nd++] = when.tm_hour % 10; digit[nd++] = -1;
	digit[nd++] = when.tm_min / 10; digit[nd++] = when.tm_min % 10;
	if (seconds) { digit[nd++] = -1; digit[nd++] = when.tm_sec / 10; digit[nd++] = when.tm_sec % 10; }
	int gw = -1;
	for (int i = 0; i < nd; i++) gw += (digit[i] < 0 ? 1 : 3) + 1;
	if (gw > r.w) { draw_digital(c, r, t, when, seconds); return; }        /* too narrow for the big font: fall back */
	int ox = r.x + (r.w - gw) / 2, oy = r.y + (r.h - 5) / 2;
	if (ox < r.x) ox = r.x;
	if (oy < r.y) oy = r.y;
	lt_color ink = t->s[LT_T_ACCENT].fg;
	uint32_t on = uni ? 0x2588 : '#';
	int x = ox;
	for (int i = 0; i < nd; i++) {
		int dw = digit[i] < 0 ? 1 : 3;
		const char *const *rows = digit[i] < 0 ? BIGCOLON : BIGDIGIT[digit[i]];
		for (int row = 0; row < 5 && oy + row < r.y + r.h; row++)
			for (int col = 0; col < dw; col++)
				if (rows[row][col] == '#') lt_put(c, all, x + col, oy + row, on, LT_STYLE(ink, LT_DEFAULT, LT_BOLD));
		x += dw + 1;
	}
}

static void polar_pt(double cx, double cy, double R, double deg, double *x, double *y)
{
	double a = deg * M_PI / 180.0;
	*x = cx + sin(a) * R;
	*y = cy - cos(a) * R;
}

static void wire_ring(lt_canvas *c, double ox, double oy, double cx, double cy, double R, lt_color col)
{
	int seg = (int)(R * 6) + 20;
	if (seg > 720) seg = 720;
	for (int i = 0; i < seg; i++) {
		double a0 = 2 * M_PI * i / seg, a1 = 2 * M_PI * (i + 1) / seg;
		lt_wire_line(c, ox + cx + R * cos(a0), oy + (cy + R * sin(a0)) / 2.0, ox + cx + R * cos(a1), oy + (cy + R * sin(a1)) / 2.0, col);
	}
}

static void draw_analog(lt_canvas *c, lt_rect r, const lt_theme *t, struct tm when)
{
	if (r.w < 5 || r.h < 3) return;
	lt_rect all = lt_full(c);
	bool uni = lt_canvas_caps(c)->unicode;
	double PW = r.w, PH = 2.0 * r.h, R = fmin(PW, PH) / 2.0 - 0.5, cx = PW / 2.0, cy = PH / 2.0;
	if (R < 1) return;
	lt_color face = t->s[LT_T_BORDER].fg, accent = t->s[LT_T_ACCENT].fg;
	wire_ring(c, r.x, r.y, cx, cy, R, face);
	for (int h = 0; h < 12; h += 3) {
		double x, y;
		polar_pt(cx, cy, R + (R > 4 ? 1.1 : 0.6), h * 30.0, &x, &y);
		char lab[3];
		snprintf(lab, sizeof lab, "%d", h == 0 ? 12 : h);
		int px = r.x + (int)floor(x + 0.5) - (int)strlen(lab) / 2, py = r.y + (int)floor(y / 2.0 + 0.5);
		lt_text(c, all, px, py, lab, t->s[LT_T_DIM], 2);
	}
	double hour_deg = fmod(when.tm_hour, 12) * 30.0 + when.tm_min / 2.0;
	double min_deg = when.tm_min * 6.0 + when.tm_sec / 10.0;
	double sec_deg = when.tm_sec * 6.0;
	double x, y;
	polar_pt(cx, cy, R * 0.5, hour_deg, &x, &y);
	lt_wire_line(c, r.x + cx, r.y + cy / 2.0, r.x + x, r.y + y / 2.0, accent);
	polar_pt(cx, cy, R * 0.8, min_deg, &x, &y);
	lt_wire_line(c, r.x + cx, r.y + cy / 2.0, r.x + x, r.y + y / 2.0, t->s[LT_T_TEXT].fg);
	polar_pt(cx, cy, R * 0.92, sec_deg, &x, &y);
	lt_wire_line(c, r.x + cx, r.y + cy / 2.0, r.x + x, r.y + y / 2.0, t->s[LT_T_ERR].fg);
	lt_put(c, all, r.x + (int)floor(cx + 0.5), r.y + (int)floor(cy / 2.0 + 0.5), uni ? 0x25CF : 'o', LT_STYLE(accent, LT_DEFAULT, LT_BOLD));
}

void lt_clock_draw(lt_canvas *c, lt_rect r, const lt_theme *t, lt_clock_kind kind, struct tm when, bool show_seconds)
{
	if (r.w < 1 || r.h < 1) return;
	if (kind == LT_CLOCK_ANALOG) { draw_analog(c, r, t, when); return; }
	if (kind == LT_CLOCK_BIG && r.h >= 5) { draw_big(c, r, t, when, show_seconds); return; }
	draw_digital(c, r, t, when, show_seconds);
}

/* ================================================================== lt_timer: a stopwatch (countdown == 0) or a countdown, driven entirely
 * by the `now` (an lt_now() reading) the caller passes to every call - the widget itself never touches the clock. */

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

/* ================================================================== lt_datepicker: a one-month calendar grid. Its own civil-calendar math
 * (Howard Hinnant's days_from_civil) instead of mktime/localtime, so it works for any year without touching libc's TZ handling. */

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

/* ================================================================== lt_timepicker: an HH:MM[:SS] field spinner, in three looks:
 * FIELDS (default) - one plain "HH:MM:SS" row, the selected field highlighted, same shape as lt_clock's digital form.
 * SPINBOX - a 3-row column per field: an up arrow, the value, a down arrow, like a classic GUI spinner.
 * H12 - FIELDS but 12-hour with a trailing AM/PM segment (stored internally as 24h regardless of the look). */

struct lt_timepicker { int h, m, s; bool seconds; int field; lt_timepicker_kind kind; };

static void clamp_hms(lt_timepicker *tp)
{
	tp->h = lt_clampi(tp->h, 0, 23);
	tp->m = lt_clampi(tp->m, 0, 59);
	tp->s = lt_clampi(tp->s, 0, 59);
}
static int tp_nfields(const lt_timepicker *tp) { int nf = tp->seconds ? 3 : 2; return tp->kind == LT_TIME_H12 ? nf + 1 : nf; }   /* +1: the AM/PM segment */

lt_timepicker *lt_timepicker_new(int h, int m, int s)
{
	lt_timepicker *tp = calloc(1, sizeof *tp);
	tp->h = h; tp->m = m; tp->s = s;
	clamp_hms(tp);
	return tp;
}
void lt_timepicker_free(lt_timepicker *tp) { free(tp); }
void lt_timepicker_get(const lt_timepicker *tp, int *h, int *m, int *s) { if (h) *h = tp->h; if (m) *m = tp->m; if (s) *s = tp->s; }
void lt_timepicker_set(lt_timepicker *tp, int h, int m, int s) { tp->h = h; tp->m = m; tp->s = s; clamp_hms(tp); }
void lt_timepicker_set_seconds(lt_timepicker *tp, bool show) { tp->seconds = show; if (!show && tp->field > 1) tp->field = 1; }
void lt_timepicker_set_kind(lt_timepicker *tp, lt_timepicker_kind k) { tp->kind = k; if (tp->field >= tp_nfields(tp)) tp->field = 0; }

static void draw_fields(const lt_timepicker *tp, lt_canvas *c, lt_rect r, const lt_theme *t, bool focused)
{
	lt_rect all = lt_full(c);
	int vals[3] = {tp->h, tp->m, tp->s}, nf = tp->seconds ? 3 : 2, x = r.x;
	bool h12 = tp->kind == LT_TIME_H12;
	if (h12) { int h = tp->h % 12; vals[0] = h == 0 ? 12 : h; }
	for (int i = 0; i < nf; i++) {
		char buf[3];
		snprintf(buf, sizeof buf, "%02d", vals[i]);
		lt_text(c, all, x, r.y, buf, (focused && i == tp->field) ? t->s[LT_T_SELECT] : t->s[LT_T_TEXT], 2);
		x += 2;
		if (i < nf - 1) { lt_put(c, all, x, r.y, ':', t->s[LT_T_DIM]); x += 1; }
	}
	if (h12) { x += 1; lt_text(c, all, x, r.y, tp->h < 12 ? "AM" : "PM", (focused && tp->field == nf) ? t->s[LT_T_SELECT] : t->s[LT_T_TEXT], 2); }
}

static void draw_spinbox(const lt_timepicker *tp, lt_canvas *c, lt_rect r, const lt_theme *t, bool focused)
{
	if (r.h < 3) { draw_fields(tp, c, r, t, focused); return; }              /* not tall enough for the 3-row look: fall back */
	lt_rect all = lt_full(c);
	bool uni = lt_canvas_caps(c)->unicode;
	int vals[3] = {tp->h, tp->m, tp->s}, nf = tp->seconds ? 3 : 2, x = r.x;
	uint32_t up = uni ? 0x25B2 : '^', dn = uni ? 0x25BC : 'v';
	for (int i = 0; i < nf; i++) {
		bool sel = focused && i == tp->field;
		lt_style ast = sel ? t->s[LT_T_ACCENT] : t->s[LT_T_DIM], vst = sel ? t->s[LT_T_SELECT] : t->s[LT_T_TEXT];
		lt_put(c, all, x, r.y, up, ast); lt_put(c, all, x + 1, r.y, up, ast);
		char buf[3];
		snprintf(buf, sizeof buf, "%02d", vals[i]);
		lt_text(c, all, x, r.y + 1, buf, vst, 2);
		lt_put(c, all, x, r.y + 2, dn, ast); lt_put(c, all, x + 1, r.y + 2, dn, ast);
		x += 2;
		if (i < nf - 1) { lt_put(c, all, x, r.y + 1, ':', t->s[LT_T_DIM]); x += 1; }
	}
}

void lt_timepicker_draw(const lt_timepicker *tp, lt_canvas *c, lt_rect r, const lt_theme *t, bool focused)
{
	if (r.w < 5 || r.h < 1) return;
	if (tp->kind == LT_TIME_SPINBOX) draw_spinbox(tp, c, r, t, focused);
	else draw_fields(tp, c, r, t, focused);
}

lt_wres lt_timepicker_event(lt_timepicker *tp, lt_rect r, const lt_event *ev)
{
	int nf = tp_nfields(tp), base_nf = tp->seconds ? 3 : 2;    /* base_nf excludes the AM/PM segment; nf includes it for LT_TIME_H12 */
	int *v[3] = {&tp->h, &tp->m, &tp->s};
	int lim[3] = {23, 59, 59};
	if (ev->kind == LT_EV_MOUSE) {
		int base = ev->key & 0xFFFFFF;
		int f = (ev->x - r.x) / 3;
		if (base == LT_K_CLICK && lt_in(r, ev->x, ev->y)) { if (f >= 0 && f < nf) tp->field = f; return LT_W_HANDLED; }
		if ((base == LT_K_WHEEL_UP || base == LT_K_WHEEL_DOWN) && lt_in(r, ev->x, ev->y)) {
			if (f < 0 || f >= nf) f = tp->field;
			if (f == base_nf) tp->h = (tp->h + 12) % 24;                              /* the AM/PM segment: any nudge just flips it */
			else *v[f] = base == LT_K_WHEEL_UP ? (*v[f] + 1) % (lim[f] + 1) : (*v[f] + lim[f]) % (lim[f] + 1);
			return LT_W_CHANGED;
		}
		return LT_W_IGNORED;
	}
	if (ev->kind != LT_EV_KEY || (ev->key & (LT_MOD_CTRL | LT_MOD_ALT))) return LT_W_IGNORED;
	int base = ev->key & 0xFFFFFF;
	switch (base) {
	case LT_K_LEFT: tp->field = (tp->field + nf - 1) % nf; return LT_W_HANDLED;
	case LT_K_RIGHT: case LT_K_TAB: tp->field = (tp->field + 1) % nf; return LT_W_HANDLED;
	case LT_K_UP: case LT_K_DOWN:
		if (tp->field == base_nf) { tp->h = (tp->h + 12) % 24; return LT_W_CHANGED; }
		*v[tp->field] = base == LT_K_UP ? (*v[tp->field] + 1) % (lim[tp->field] + 1) : (*v[tp->field] + lim[tp->field]) % (lim[tp->field] + 1);
		return LT_W_CHANGED;
	}
	return LT_W_IGNORED;
}
