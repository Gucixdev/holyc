/* timepicker.c - Lt_timepicker: an HH:MM[:SS] field spinner, in three looks: FIELDS (default) - one plain "HH:MM:SS" row, the selected field highlighted, same shape as lt_clock's digital form. SPINBOX - a 3-row column per field: an up arrow, the value, a down arrow, like a classic GUI spinner. H12 - FIELDS but 12-hour with a trailing AM/PM segment (stored internally as 24h regardless of the look).
 * Needs: lazytui.h (the lib core). One widget, one file: copy it into any project that links the core. */
#include "internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
