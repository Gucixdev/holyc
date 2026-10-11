/* internal.h - helpers shared by the interactive widgets (list, tree, dropdown). Not part of the public API. */
#ifndef LT_INTERNAL_H
#define LT_INTERNAL_H
#include "lazytui.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static inline int lt_clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline bool lt_in(lt_rect r, int x, int y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }
static inline lt_rect lt_full(lt_canvas *c) { return (lt_rect){0, 0, lt_canvas_w(c), lt_canvas_h(c)}; }
static inline double lt_clamp01(double v) { if (!(v > 0)) return 0; return v > 1 ? 1 : v; }        /* NaN -> 0 */
static inline double lt_finite_or(double v, double d) { return isfinite(v) ? v : d; }
static inline lt_style lt_slot_style(const lt_theme *t, lt_slot s) { return t->s[s]; }
/* the single centred row a one-line bar draws on, out of whatever rectangle it was given */
static inline lt_rect lt_one_row(lt_rect r) { if (r.h > 1) { r.y += (r.h - 1) / 2; r.h = 1; } return r; }

/* keeps `cursor` inside the window [offset, offset + rows) and the window inside [0, total) */
static inline int lt_scroll_to(int offset, int cursor, int rows, int total)
{
	if (rows < 1) rows = 1;
	if (cursor < offset) offset = cursor;
	if (cursor >= offset + rows) offset = cursor - rows + 1;
	return lt_clampi(offset, 0, total > rows ? total - rows : 0);
}

/* the shared pointer state of a scrolling widget: double click detection and scrollbar dragging */
typedef struct { double last_t; int last_row; bool dragging; } lt_ptr;
bool lt_ptr_double(lt_ptr *p, int row);          /* call on every click: true when it is the second click on the same row */

/* a scrolling view of `total` rows in rectangle r (content + scrollbar column). Returns true if the event was a scroll/drag on it. */
bool lt_view_mouse(lt_ptr *p, lt_rect r, int total, int *offset, const lt_event *ev, bool *on_bar_click);
lt_rect lt_view_content(lt_rect r, int total);     /* r without the scrollbar column when one is needed */
#endif
