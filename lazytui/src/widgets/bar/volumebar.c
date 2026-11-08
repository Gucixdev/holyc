/* volumebar.c - Volumebar: a level bar (horizontal or vertical) with a label (speaker / mic), a mute state, keys and mouse.
 * Keys: Left/Right (horizontal) or Down/Up (vertical) and -/+ step the level, PgUp/PgDn step x5, Home/End jump to 0 / max, m toggles mute.
 * Mouse: click or drag on the bar sets the level, the wheel steps it, a click on the percent / MUTE text toggles mute.
 * The audio backend stays OUT of the widget: it only calls the get/set callbacks you hand it (any of them may be NULL; then the widget
 * keeps that value itself). Levels are 0..max (max = 1.0 normally, 1.5 allows boosting past 100%).
 * Needs: lazytui.h (the lib core). One widget, one file: copy it into any project that links the core. */
#include "internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct lt_volumebar {
	char *label;
	lt_volume_cb cb;
	double level, step, max;
	bool muted, vertical;
};

lt_volumebar *lt_volumebar_new(const char *label, lt_volume_cb cb)
{
	lt_volumebar *v = calloc(1, sizeof *v);
	if (!v) return NULL;
	v->label = strdup(label ? label : "");
	v->cb = cb;
	v->step = 0.05;
	v->max = 1.0;
	return v;
}
void lt_volumebar_free(lt_volumebar *v) { if (v) { free(v->label); free(v); } }
void lt_volumebar_set_vertical(lt_volumebar *v, bool vertical) { v->vertical = vertical; }
void lt_volumebar_set_step(lt_volumebar *v, double step) { if (step > 0 && step <= 1) v->step = step; }
void lt_volumebar_set_max(lt_volumebar *v, double max) { if (max >= 0.1 && max <= 4) v->max = max; }
void lt_volumebar_set_label(lt_volumebar *v, const char *label) { char *n = strdup(label ? label : ""); if (n) { free(v->label); v->label = n; } }

/* pull the backend's truth into the widget (called by draw and before every change, so an external change is never overwritten blindly) */
static void refresh(lt_volumebar *v)
{
	if (v->cb.get) v->level = v->cb.get(v->cb.ctx);
	if (v->cb.get_mute) v->muted = v->cb.get_mute(v->cb.ctx) != 0;
	if (!(v->level >= 0)) v->level = 0;                       /* NaN or negative */
	if (v->level > v->max) v->level = v->max;
}
double lt_volumebar_level(lt_volumebar *v) { refresh(v); return v->level; }
bool lt_volumebar_muted(lt_volumebar *v) { refresh(v); return v->muted; }

static void put_level(lt_volumebar *v, double x)
{
	if (!(x >= 0)) x = 0;
	if (x > v->max) x = v->max;
	v->level = x;
	if (v->cb.set) v->cb.set(v->cb.ctx, x);
}
void lt_volumebar_set_level(lt_volumebar *v, double x) { put_level(v, x); }
void lt_volumebar_set_muted(lt_volumebar *v, bool m)
{
	v->muted = m;
	if (v->cb.set_mute) v->cb.set_mute(v->cb.ctx, m);
}

/* geometry shared by draw and event */
typedef struct { lt_rect bar, label, text; } geom;
static geom layout(const lt_volumebar *v, lt_rect r)
{
	geom g = {0};
	if (v->vertical) {
		int top = r.h >= 4 ? 1 : 0, bot = (r.h >= 3 && *v->label) ? 1 : 0;
		g.text = (lt_rect){r.x, r.y, r.w, top};
		g.label = (lt_rect){r.x, r.y + r.h - bot, r.w, bot};
		int bw = r.w >= 3 ? 2 : 1;
		g.bar = (lt_rect){r.x + (r.w - bw) / 2, r.y + top, bw, r.h - top - bot};
		if (g.bar.h < 1) g.bar.h = 1;
		return g;
	}
	r = lt_one_row(r);
	int left = 0, right = 0;
	if (*v->label) {
		int lw = lt_utf8_width(v->label), maxl = r.w / 3;
		if (lw > maxl) lw = maxl;
		g.label = (lt_rect){r.x, r.y, lw, 1};
		left = lw + 1;
	}
	if (r.w - left >= 11) right = 5;                             /* " 100%" / " MUTE" */
	g.text = (lt_rect){r.x + r.w - right + (right ? 1 : 0), r.y, right ? 4 : 0, 1};
	g.bar = (lt_rect){r.x + left, r.y, r.w - left - right, 1};
	if (g.bar.w < 1) g.bar.w = 1;
	return g;
}

void lt_volumebar_draw(lt_volumebar *v, lt_canvas *c, lt_rect r, const lt_theme *t, bool focused)
{
	refresh(v);
	lt_rect all = lt_full(c);
	geom g = layout(v, r);
	double f = v->max > 0 ? lt_clamp01(v->level / v->max) : 0;
	int pct = (int)floor(v->level * 100 + 0.5);
	char txt[16];
	if (v->muted) snprintf(txt, sizeof txt, "MUTE"); else snprintf(txt, sizeof txt, "%3d%%", pct);
	lt_style lab = focused ? t->s[LT_T_SELECT] : lt_slot_style(t, LT_T_LABEL);
	lt_style on = v->muted ? t->s[LT_T_DIM] : lt_slot_style(t, LT_T_BAR);
	if (!v->muted && v->level > 1.0001) on = t->s[LT_T_WARN];                 /* boosted past 100% */
	lt_style bg = lt_slot_style(t, LT_T_BAR_BG);
	on = LT_STYLE(on.fg, bg.bg, on.attr);
	lt_style off = LT_STYLE(lt_slot_style(t, LT_T_BAR).fg, bg.bg, bg.attr);
	if (v->muted) off = LT_STYLE(t->s[LT_T_DIM].fg, bg.bg, bg.attr);
	lt_style txst = v->muted ? t->s[LT_T_ERR] : t->s[LT_T_TEXT];
	bool uni = lt_canvas_caps(c)->unicode;
	if (g.label.w > 0 && g.label.h > 0) lt_text(c, all, g.label.x, g.label.y, v->label, lab, g.label.w);
	if (g.text.w > 0 && g.text.h > 0) lt_text(c, all, g.text.x, g.text.y, txt, txst, g.text.w);
	if (v->vertical) {
		static const uint32_t up8[9] = {' ', 0x2581, 0x2582, 0x2583, 0x2584, 0x2585, 0x2586, 0x2587, 0x2588};
		double cells = f * g.bar.h;
		int whole = (int)floor(cells), eighth = (int)floor((cells - whole) * 8 + 0.5);
		if (eighth == 8) { whole++; eighth = 0; }
		for (int j = 0; j < g.bar.h; j++) {
			int fromb = g.bar.h - 1 - j;                          /* rows counted from the bottom */
			for (int i = 0; i < g.bar.w; i++) {
				uint32_t ch; lt_style st = on;
				if (!uni) ch = fromb < whole ? '#' : (fromb == whole && eighth >= 4 ? '=' : ' ');
				else if (fromb < whole) ch = 0x2588;
				else if (fromb == whole && eighth > 0) ch = up8[eighth];
				else { ch = ' '; st = off; }
				if (!uni && ch == ' ') st = off;
				lt_put(c, all, g.bar.x + i, g.bar.y + j, ch, st);
			}
		}
		return;
	}
	static const uint32_t eighths[9] = {' ', 0x258F, 0x258E, 0x258D, 0x258C, 0x258B, 0x258A, 0x2589, 0x2588};
	lt_rect b = g.bar;
	if (!uni) {
		if (b.w < 4) { lt_fill(c, b, f >= 1 ? '#' : '-', on); return; }
		int inner = b.w - 2, fill = (int)floor(f * inner);
		lt_put(c, all, b.x, b.y, '[', off);
		for (int i = 0; i < inner; i++) lt_put(c, all, b.x + 1 + i, b.y, i < fill ? '=' : (i == fill && f < 1 ? '>' : ' '), i <= fill && f > 0 ? on : off);
		lt_put(c, all, b.x + b.w - 1, b.y, ']', off);
		return;
	}
	double cells = f * b.w;
	int whole = (int)floor(cells), eighth = (int)floor((cells - whole) * 8 + 0.5);
	if (eighth == 8) { whole++; eighth = 0; }
	for (int i = 0; i < b.w; i++) {
		if (i < whole) lt_put(c, all, b.x + i, b.y, 0x2588, on);
		else if (i == whole && eighth > 0) lt_put(c, all, b.x + i, b.y, eighths[eighth], on);
		else lt_put(c, all, b.x + i, b.y, ' ', off);
	}
}

static double at_point(const lt_volumebar *v, const geom *g, int x, int y)
{
	double f;
	if (v->vertical) f = g->bar.h > 1 ? (double)(g->bar.y + g->bar.h - 1 - y) / (g->bar.h - 1) : 1;
	else f = g->bar.w > 1 ? (double)(x - g->bar.x) / (g->bar.w - 1) : 1;
	return lt_clamp01(f) * v->max;
}
static double snap(double x) { return floor(x * 1000 + 0.5) / 1000; }        /* kill float dust so 0.05 steps stay 0.05 steps */

lt_wres lt_volumebar_event(lt_volumebar *v, lt_rect r, const lt_event *ev)
{
	geom g = layout(v, r);
	if (ev->kind == LT_EV_MOUSE) {
		int base = ev->key & 0xFFFFFF;
		if (base != LT_K_CLICK && base != LT_K_DRAG && base != LT_K_WHEEL_UP && base != LT_K_WHEEL_DOWN) return LT_W_IGNORED;
		bool in_bar = ev->x >= g.bar.x && ev->x < g.bar.x + g.bar.w && ev->y >= g.bar.y && ev->y < g.bar.y + g.bar.h;
		if (!lt_in(r, ev->x, ev->y)) return LT_W_IGNORED;
		refresh(v);
		if (base == LT_K_WHEEL_UP || base == LT_K_WHEEL_DOWN) {
			put_level(v, snap(v->level + (base == LT_K_WHEEL_UP ? v->step : -v->step)));
			return LT_W_CHANGED;
		}
		if (base == LT_K_CLICK && g.text.w > 0 && ev->x >= g.text.x && ev->x < g.text.x + g.text.w && ev->y >= g.text.y && ev->y < g.text.y + g.text.h) {
			lt_volumebar_set_muted(v, !v->muted);
			return LT_W_TOGGLE;
		}
		if (!in_bar && !(v->vertical ? (ev->y >= g.bar.y && ev->y < g.bar.y + g.bar.h) : (ev->x >= g.bar.x && ev->x < g.bar.x + g.bar.w))) return LT_W_HANDLED;
		put_level(v, snap(at_point(v, &g, ev->x, ev->y)));
		return LT_W_CHANGED;
	}
	if (ev->kind != LT_EV_KEY || (ev->key & (LT_MOD_CTRL | LT_MOD_ALT))) return LT_W_IGNORED;
	int base = ev->key & 0xFFFFFF;
	double d = 0;
	switch (base) {
	case 'm': case 'M': refresh(v); lt_volumebar_set_muted(v, !v->muted); return LT_W_TOGGLE;
	case '+': case '=': d = v->step; break;
	case '-': case '_': d = -v->step; break;
	case LT_K_RIGHT: if (v->vertical) return LT_W_IGNORED; d = v->step; break;
	case LT_K_LEFT: if (v->vertical) return LT_W_IGNORED; d = -v->step; break;
	case LT_K_UP: if (!v->vertical) return LT_W_IGNORED; d = v->step; break;
	case LT_K_DOWN: if (!v->vertical) return LT_W_IGNORED; d = -v->step; break;
	case LT_K_PGUP: d = v->step * 5; break;
	case LT_K_PGDN: d = -v->step * 5; break;
	case LT_K_HOME: refresh(v); put_level(v, 0); return LT_W_CHANGED;
	case LT_K_END: refresh(v); put_level(v, v->max); return LT_W_CHANGED;
	default: return LT_W_IGNORED;
	}
	refresh(v);
	put_level(v, snap(v->level + d));
	return LT_W_CHANGED;
}
