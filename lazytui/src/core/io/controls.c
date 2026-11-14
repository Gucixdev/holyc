/* controls.c - button, checkbox / radio / switch, radio group, slider. Each keeps its state, draws itself into a rectangle and turns key and
 * mouse events into a result (ACTIVATE for a button, CHANGED for a value). The last drawn rectangle of the parts (the track of a slider, the
 * box of a checkbox) is remembered so a click is hit-tested against what was actually on screen. */
#include "internal.h"
#include <math.h>
#include <stdio.h>

/* ------------------------------------------------------------------ button */
struct lt_button { char *label; lt_btn_style style; };
lt_button *lt_button_new(const char *label, lt_btn_style style) { lt_button *b = calloc(1, sizeof *b); b->label = strdup(label ? label : ""); b->style = style; return b; }
void lt_button_free(lt_button *b) { if (b) { free(b->label); free(b); } }
void lt_button_set_label(lt_button *b, const char *l) { free(b->label); b->label = strdup(l ? l : ""); }
int lt_button_width(const lt_button *b) { return lt_utf8_width(b->label) + 4; }

void lt_button_draw(lt_button *b, lt_canvas *c, lt_rect r, const lt_theme *t, bool focused)
{
	if (r.w < 3 || r.h < 1) return;
	r.h = 1;
	int w = lt_button_width(b);
	if (w > r.w) w = r.w;
	lt_style st = t->s[b->style == LT_BTN_DANGER ? LT_T_ERR : b->style == LT_BTN_PRIMARY ? LT_T_ACCENT : LT_T_TEXT];
	if (b->style != LT_BTN_NORMAL) st.attr |= LT_BOLD;
	if (focused) st = t->s[LT_T_SELECT];
	lt_rect all = lt_full(c);
	lt_fill(c, (lt_rect){r.x, r.y, w, 1}, ' ', st);
	lt_put(c, all, r.x, r.y, '[', st); lt_put(c, all, r.x + w - 1, r.y, ']', st);
	lt_text(c, all, r.x + 2, r.y, b->label, st, w - 4 > 0 ? w - 4 : 0);
}

lt_wres lt_button_event(lt_button *b, lt_rect r, const lt_event *ev)
{
	r.w = r.w < lt_button_width(b) ? r.w : lt_button_width(b);
	r.h = 1;
	if (ev->kind == LT_EV_MOUSE) return ((ev->key & 0xFFFFFF) == LT_K_CLICK && lt_in(r, ev->x, ev->y)) ? LT_W_ACTIVATE : LT_W_IGNORED;
	if (ev->kind == LT_EV_KEY && !(ev->key & (LT_MOD_CTRL | LT_MOD_ALT)) && (ev->key == LT_K_ENTER || ev->key == ' ')) return LT_W_ACTIVATE;
	return LT_W_IGNORED;
}

/* ------------------------------------------------------------------ checkbox / radio / switch */
struct lt_check { char *label; lt_check_kind kind; bool on; };
lt_check *lt_check_new(const char *label, lt_check_kind kind, bool checked) { lt_check *c = calloc(1, sizeof *c); c->label = strdup(label ? label : ""); c->kind = kind; c->on = checked; return c; }
void lt_check_free(lt_check *c) { if (c) { free(c->label); free(c); } }
bool lt_check_checked(const lt_check *c) { return c->on; }
void lt_check_set(lt_check *c, bool on) { c->on = on; }

static int check_box_w(lt_check_kind k) { return k == LT_SWITCH ? 6 : 3; }

void lt_check_draw(lt_check *ck, lt_canvas *c, lt_rect r, const lt_theme *t, bool focused)
{
	if (r.w < 3 || r.h < 1) return;
	lt_rect all = lt_full(c);
	bool uni = lt_canvas_caps(c)->unicode;
	lt_style st = focused ? t->s[LT_T_SELECT] : t->s[LT_T_TEXT], mark = focused ? st : t->s[ck->on ? LT_T_OK : LT_T_DIM];
	lt_fill(c, (lt_rect){r.x, r.y, r.w, 1}, ' ', st);
	int bw = check_box_w(ck->kind);
	if (ck->kind == LT_CHECKBOX) { lt_text(c, all, r.x, r.y, ck->on ? (uni ? "[\xE2\x9C\x93]" : "[x]") : "[ ]", mark, 3); }
	else if (ck->kind == LT_RADIO) { lt_text(c, all, r.x, r.y, ck->on ? (uni ? "(\xE2\x97\x8F)" : "(o)") : "( )", mark, 3); }
	else {                                                    /* switch: [ o==] off, [==o ] on */
		lt_style track = focused ? st : t->s[ck->on ? LT_T_OK : LT_T_DIM];
		lt_text(c, all, r.x, r.y, ck->on ? (uni ? "[\xE2\x94\x81\xE2\x94\x81\xE2\x97\x8F ]" : "[==o ]") : (uni ? "[ \xE2\x97\x8F\xE2\x94\x81\xE2\x94\x81]" : "[ o==]"), track, 6);
	}
	lt_text(c, all, r.x + bw + 1, r.y, ck->label, st, r.w - bw - 1);
}

lt_wres lt_check_event(lt_check *ck, lt_rect r, const lt_event *ev)
{
	r.h = 1;
	int w = check_box_w(ck->kind) + 1 + lt_utf8_width(ck->label);
	if (r.w > w) r.w = w;
	bool hit = false;
	if (ev->kind == LT_EV_MOUSE) hit = (ev->key & 0xFFFFFF) == LT_K_CLICK && lt_in(r, ev->x, ev->y);
	else if (ev->kind == LT_EV_KEY && !(ev->key & (LT_MOD_CTRL | LT_MOD_ALT))) hit = ev->key == ' ' || ev->key == LT_K_ENTER;
	if (!hit) return LT_W_IGNORED;
	if (ck->kind == LT_RADIO && ck->on) return LT_W_HANDLED;         /* a radio is switched off by choosing another */
	ck->on = !ck->on;
	return LT_W_CHANGED;
}

/* ------------------------------------------------------------------ radio group */
struct lt_radiogroup { char **label; int n, sel, cursor; };
lt_radiogroup *lt_radiogroup_new(const char *const *labels, int n)
{
	lt_radiogroup *g = calloc(1, sizeof *g);
	g->n = n;
	g->label = calloc((size_t)(n ? n : 1), sizeof *g->label);
	for (int i = 0; i < n; i++) g->label[i] = strdup(labels[i]);
	return g;
}
void lt_radiogroup_free(lt_radiogroup *g) { if (!g) return; for (int i = 0; i < g->n; i++) free(g->label[i]); free(g->label); free(g); }
int lt_radiogroup_selected(const lt_radiogroup *g) { return g->sel; }
void lt_radiogroup_set(lt_radiogroup *g, int i) { if (i >= 0 && i < g->n) g->sel = g->cursor = i; }

void lt_radiogroup_draw(lt_radiogroup *g, lt_canvas *c, lt_rect r, const lt_theme *t, bool focused)
{
	lt_rect all = lt_full(c);
	bool uni = lt_canvas_caps(c)->unicode;
	for (int i = 0; i < g->n && i < r.h; i++) {
		bool cur = focused && i == g->cursor;
		lt_style st = cur ? t->s[LT_T_SELECT] : t->s[LT_T_TEXT];
		lt_fill(c, (lt_rect){r.x, r.y + i, r.w, 1}, ' ', st);
		lt_text(c, all, r.x, r.y + i, i == g->sel ? (uni ? "(\xE2\x97\x8F)" : "(o)") : "( )", cur ? st : t->s[i == g->sel ? LT_T_OK : LT_T_DIM], 3);
		lt_text(c, all, r.x + 4, r.y + i, g->label[i], st, r.w - 4);
	}
}

lt_wres lt_radiogroup_event(lt_radiogroup *g, lt_rect r, const lt_event *ev)
{
	if (g->n == 0) return LT_W_IGNORED;
	int base = ev->key & 0xFFFFFF;
	if (ev->kind == LT_EV_MOUSE) {
		if (base != LT_K_CLICK || !lt_in(r, ev->x, ev->y)) return LT_W_IGNORED;
		int i = ev->y - r.y;
		if (i >= g->n) return LT_W_HANDLED;
		g->cursor = i;
		if (g->sel == i) return LT_W_HANDLED;
		g->sel = i;
		return LT_W_CHANGED;
	}
	if (ev->kind != LT_EV_KEY || (ev->key & (LT_MOD_CTRL | LT_MOD_ALT))) return LT_W_IGNORED;
	switch (base) {
	case LT_K_UP: g->cursor = lt_clampi(g->cursor - 1, 0, g->n - 1); return LT_W_HANDLED;
	case LT_K_DOWN: g->cursor = lt_clampi(g->cursor + 1, 0, g->n - 1); return LT_W_HANDLED;
	case ' ': case LT_K_ENTER: if (g->sel == g->cursor) return LT_W_HANDLED; g->sel = g->cursor; return LT_W_CHANGED;
	}
	return LT_W_IGNORED;
}

/* ------------------------------------------------------------------ slider */
struct lt_slider { double min, max, step, v; lt_rect track; bool dragging; };
lt_slider *lt_slider_new(double min, double max, double step, double value)
{
	lt_slider *s = calloc(1, sizeof *s);
	if (!(max > min)) max = min + 1;
	s->min = min; s->max = max; s->step = step > 0 ? step : (max - min) / 100;
	lt_slider_set(s, value);
	return s;
}
void lt_slider_free(lt_slider *s) { free(s); }
double lt_slider_value(const lt_slider *s) { return s->v; }
void lt_slider_set(lt_slider *s, double v)
{
	if (!isfinite(v)) v = s->min;
	v = s->min + floor((v - s->min) / s->step + 0.5) * s->step;
	s->v = v < s->min ? s->min : v > s->max ? s->max : v;
}

void lt_slider_draw(lt_slider *s, lt_canvas *c, lt_rect r, const lt_theme *t, bool focused, const char *label)
{
	if (r.w < 8 || r.h < 1) return;
	lt_rect all = lt_full(c);
	bool uni = lt_canvas_caps(c)->unicode;
	char val[32];
	snprintf(val, sizeof val, s->step >= 1 ? "%.0f" : s->step >= 0.1 ? "%.1f" : "%.2f", s->v);
	int lw = label ? lt_utf8_width(label) + 1 : 0, vw = (int)strlen(val) + 1;
	if (lw + vw + 6 > r.w) lw = 0;
	lt_style st = t->s[LT_T_TEXT];
	lt_fill(c, (lt_rect){r.x, r.y, r.w, 1}, ' ', st);
	if (lw) lt_text(c, all, r.x, r.y, label, t->s[LT_T_LABEL], lw - 1);
	int tx = r.x + lw, tw = r.w - lw - vw;
	s->track = (lt_rect){tx, r.y, tw, 1};
	double f = (s->v - s->min) / (s->max - s->min);
	int pos = (int)floor(f * (tw - 1) + 0.5);
	lt_style on = t->s[focused ? LT_T_ACCENT : LT_T_OK], off = t->s[LT_T_DIM];
	for (int i = 0; i < tw; i++) {
		if (i == pos) lt_put(c, all, tx + i, r.y, uni ? 0x25CF : 'O', focused ? t->s[LT_T_SELECT] : t->s[LT_T_ACCENT]);
		else lt_put(c, all, tx + i, r.y, i < pos ? (uni ? 0x2501 : '=') : (uni ? 0x2500 : '-'), i < pos ? on : off);
	}
	lt_text(c, all, r.x + r.w - vw + 1, r.y, val, st, vw - 1);
}

static void slider_from_x(lt_slider *s, int x)
{
	if (s->track.w < 2) return;
	double f = (double)(x - s->track.x) / (s->track.w - 1);
	lt_slider_set(s, s->min + (f < 0 ? 0 : f > 1 ? 1 : f) * (s->max - s->min));
}

lt_wres lt_slider_event(lt_slider *s, lt_rect r, const lt_event *ev)
{
	double old = s->v;
	int base = ev->key & 0xFFFFFF;
	if (ev->kind == LT_EV_MOUSE) {
		r.h = 1;
		if (base == LT_K_CLICK && lt_in(s->track, ev->x, ev->y)) { s->dragging = true; slider_from_x(s, ev->x); return s->v != old ? LT_W_CHANGED : LT_W_HANDLED; }
		if (base == LT_K_DRAG && s->dragging) { slider_from_x(s, ev->x); return s->v != old ? LT_W_CHANGED : LT_W_HANDLED; }
		if (base == LT_K_RELEASE && s->dragging) { s->dragging = false; return LT_W_HANDLED; }
		if ((base == LT_K_WHEEL_UP || base == LT_K_WHEEL_DOWN) && lt_in(r, ev->x, ev->y)) { lt_slider_set(s, s->v + (base == LT_K_WHEEL_UP ? s->step : -s->step)); return s->v != old ? LT_W_CHANGED : LT_W_HANDLED; }
		return LT_W_IGNORED;
	}
	if (ev->kind != LT_EV_KEY || (ev->key & (LT_MOD_CTRL | LT_MOD_ALT))) return LT_W_IGNORED;
	double big = fmax(s->step, (s->max - s->min) / 10);
	switch (base) {
	case LT_K_LEFT: case LT_K_DOWN: lt_slider_set(s, s->v - s->step); break;
	case LT_K_RIGHT: case LT_K_UP: lt_slider_set(s, s->v + s->step); break;
	case LT_K_PGDN: lt_slider_set(s, s->v - big); break;
	case LT_K_PGUP: lt_slider_set(s, s->v + big); break;
	case LT_K_HOME: lt_slider_set(s, s->min); break;
	case LT_K_END: lt_slider_set(s, s->max); break;
	default: return LT_W_IGNORED;
	}
	return s->v != old ? LT_W_CHANGED : LT_W_HANDLED;
}
