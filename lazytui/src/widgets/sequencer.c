/* sequencer.c - a step-sequencer grid: tracks x steps, one boolean per cell, click or arrows+Enter/Space to toggle a step. A `playhead`
 * column the caller advances from its own clock/BPM is highlighted independently of the on/off state - this widget only holds the pattern
 * and draws it, it plays nothing itself (lazytui has no audio I/O; wiring the playhead to real sound is the caller's job). Steps are
 * visually grouped every `group` of them (4, typically) with a one-column gap, like a real drum machine's beat groups. */
#include "internal.h"
#include <string.h>

struct lt_sequencer {
	int tracks, steps;
	bool *cell;                        /* tracks*steps, row-major */
	char **name;                       /* one label per track, may be NULL */
	int playhead;                      /* -1 = none */
	int group;                         /* a gap every `group` steps; 0 = no grouping */
	int cx, cy;                        /* the keyboard cursor: step, track */
	int gx, gy;                        /* the last-drawn grid's origin, for hit-testing */
	int cellw;                         /* a step's width in columns (always 2; kept as a field so the x-formula helpers need no magic number) */
};

lt_sequencer *lt_sequencer_new(int tracks, int steps)
{
	if (tracks < 1) tracks = 1;
	if (steps < 1) steps = 1;
	lt_sequencer *s = calloc(1, sizeof *s);
	s->tracks = tracks; s->steps = steps;
	s->cell = calloc((size_t)tracks * (size_t)steps, sizeof *s->cell);
	s->name = calloc((size_t)tracks, sizeof *s->name);
	s->playhead = -1;
	s->cellw = 2;
	return s;
}
void lt_sequencer_free(lt_sequencer *s)
{
	if (!s) return;
	for (int i = 0; i < s->tracks; i++) free(s->name[i]);
	free(s->name); free(s->cell); free(s);
}

static bool sq_in(const lt_sequencer *s, int tr, int st) { return tr >= 0 && tr < s->tracks && st >= 0 && st < s->steps; }
bool lt_sequencer_get(const lt_sequencer *s, int tr, int st) { return sq_in(s, tr, st) && s->cell[tr * s->steps + st]; }
void lt_sequencer_set(lt_sequencer *s, int tr, int st, bool on) { if (sq_in(s, tr, st)) s->cell[tr * s->steps + st] = on; }
void lt_sequencer_toggle(lt_sequencer *s, int tr, int st) { if (sq_in(s, tr, st)) s->cell[tr * s->steps + st] = !s->cell[tr * s->steps + st]; }
void lt_sequencer_clear(lt_sequencer *s) { for (int i = 0; i < s->tracks * s->steps; i++) s->cell[i] = false; }
int lt_sequencer_tracks(const lt_sequencer *s) { return s->tracks; }
int lt_sequencer_steps(const lt_sequencer *s) { return s->steps; }
void lt_sequencer_set_track_name(lt_sequencer *s, int tr, const char *name)
{
	if (tr < 0 || tr >= s->tracks) return;
	free(s->name[tr]);
	s->name[tr] = name ? strdup(name) : NULL;
}
const char *lt_sequencer_track_name(const lt_sequencer *s, int tr) { return tr >= 0 && tr < s->tracks ? s->name[tr] : NULL; }
void lt_sequencer_set_playhead(lt_sequencer *s, int step) { s->playhead = step; }
int lt_sequencer_playhead(const lt_sequencer *s) { return s->playhead; }
void lt_sequencer_set_group(lt_sequencer *s, int group) { s->group = group < 0 ? 0 : group; }
int lt_sequencer_cursor_step(const lt_sequencer *s) { return s->cx; }
int lt_sequencer_cursor_track(const lt_sequencer *s) { return s->cy; }

/* the x offset (relative to the grid's own origin) of a given step, and its inverse - both walk the same group-gap rule, so a click always
 * lands on exactly the step it looks like it's over regardless of how the steps are grouped */
static int step_x(const lt_sequencer *s, int step)
{
	int x = 0;
	for (int st = 0; st < step; st++) {
		x += s->cellw;
		if (s->group > 0 && (st + 1) % s->group == 0) x += 1;
	}
	return x;
}
static int step_at_x(const lt_sequencer *s, int rel_x)
{
	if (rel_x < 0) return -1;
	for (int st = 0; st < s->steps; st++) {
		int x0 = step_x(s, st);
		if (rel_x >= x0 && rel_x < x0 + s->cellw) return st;
	}
	return -1;
}

void lt_sequencer_draw(lt_sequencer *s, lt_canvas *c, lt_rect r, const lt_theme *t, bool focused)
{
	if (r.w < 4 || r.h < 1) return;
	lt_rect all = lt_full(c);
	bool uni = lt_canvas_caps(c)->unicode;
	int lw = 0;
	for (int tr = 0; tr < s->tracks; tr++) { int w = s->name[tr] ? lt_utf8_width(s->name[tr]) : 0; if (w > lw) lw = w; }
	if (lw > r.w - s->cellw) lw = r.w - s->cellw > 0 ? r.w - s->cellw : 0;
	s->gx = r.x + (lw > 0 ? lw + 1 : 0);
	s->gy = r.y;
	for (int tr = 0; tr < s->tracks && tr < r.h; tr++) {
		int y = r.y + tr;
		if (lw > 0 && s->name[tr]) lt_text(c, all, r.x, y, s->name[tr], t->s[LT_T_LABEL], lw);
		lt_color trackcol = lt_theme_cat(t, tr);
		for (int st = 0; st < s->steps; st++) {
			int x = s->gx + step_x(s, st);
			if (x + s->cellw > r.x + r.w) break;
			bool on = lt_sequencer_get(s, tr, st);
			bool head = st == s->playhead;
			bool cur = focused && tr == s->cy && st == s->cx;
			lt_color bg = head ? lt_color_mix(t->s[LT_T_TEXT].bg != LT_DEFAULT ? t->s[LT_T_TEXT].bg : LT_RGB(10, 10, 10), trackcol, 0.25) : LT_DEFAULT;
			lt_style cell_st = cur ? t->s[LT_T_SELECT] : LT_STYLE(on ? trackcol : t->s[LT_T_DIM].fg, bg, on ? LT_BOLD : 0);
			uint32_t glyph = on ? (uni ? 0x2588 : '#') : (uni ? 0xB7 : '.');
			lt_put(c, all, x, y, glyph, cell_st);
			lt_put(c, all, x + 1, y, glyph, cell_st);
		}
	}
}

lt_wres lt_sequencer_event(lt_sequencer *s, lt_rect r, const lt_event *ev)
{
	int base = ev->key & 0xFFFFFF;
	if (ev->kind == LT_EV_MOUSE) {
		if (base != LT_K_CLICK || !lt_in(r, ev->x, ev->y)) return LT_W_IGNORED;
		int tr = ev->y - s->gy, st = step_at_x(s, ev->x - s->gx);
		if (!sq_in(s, tr, st)) return LT_W_IGNORED;
		s->cy = tr; s->cx = st;
		lt_sequencer_toggle(s, tr, st);
		return LT_W_CHANGED;
	}
	if (ev->kind != LT_EV_KEY || (ev->key & (LT_MOD_CTRL | LT_MOD_ALT))) return LT_W_IGNORED;
	switch (base) {
	case LT_K_LEFT: s->cx = lt_clampi(s->cx - 1, 0, s->steps - 1); return LT_W_HANDLED;
	case LT_K_RIGHT: s->cx = lt_clampi(s->cx + 1, 0, s->steps - 1); return LT_W_HANDLED;
	case LT_K_UP: s->cy = lt_clampi(s->cy - 1, 0, s->tracks - 1); return LT_W_HANDLED;
	case LT_K_DOWN: s->cy = lt_clampi(s->cy + 1, 0, s->tracks - 1); return LT_W_HANDLED;
	case LT_K_HOME: s->cx = 0; return LT_W_HANDLED;
	case LT_K_END: s->cx = s->steps - 1; return LT_W_HANDLED;
	case ' ': case LT_K_ENTER: lt_sequencer_toggle(s, s->cy, s->cx); return LT_W_CHANGED;
	case 'r': lt_sequencer_clear(s); return LT_W_CHANGED;
	}
	return LT_W_IGNORED;
}
