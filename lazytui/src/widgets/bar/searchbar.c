/* searchbar.c - Searchbar: the query line's drawing (editing lives in input.c's lt_input / lt_searchbar_event)
 * Needs: lazytui.h (the lib core). One widget, one file: copy it into any project that links the core. */
#include "internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void lt_searchbar(lt_canvas *c, lt_rect r, const lt_theme *t, const char *prompt, const char *query, int cursor)
{
	lt_rect all = lt_full(c);
	r = lt_one_row(r);
	lt_fill(c, r, ' ', lt_slot_style(t, LT_T_TEXT));
	int x = r.x + lt_text(c, all, r.x, r.y, prompt ? prompt : "> ", lt_slot_style(t, LT_T_ACCENT), r.w);
	int cx = x, col = 0, ci = 0;
	for (const char *s = query ? query : ""; *s; ci++) {
		uint32_t q;
		size_t l = lt_utf8_decode(s, &q);
		if (ci == cursor) cx = x + col;
		lt_put(c, all, x + col, r.y, q, lt_slot_style(t, LT_T_TEXT));
		char tmp[5]; memcpy(tmp, s, l); tmp[l] = 0;
		col += lt_utf8_width(tmp);
		s += l;
	}
	if (cursor < 0) return;                                      /* no cursor: the line is not focused */
	if (cursor >= ci) cx = x + col;
	lt_style cur = lt_slot_style(t, LT_T_TEXT);
	cur.attr ^= LT_REVERSE;
	lt_put(c, all, cx, r.y, ' ', cur);                          /* a blank cell drawn reversed; text under it is redrawn */
	int k = 0;
	for (const char *s = query ? query : ""; *s; k++) {
		uint32_t q; size_t l = lt_utf8_decode(s, &q);
		if (k == cursor) lt_put(c, all, cx, r.y, q, cur);
		s += l;
	}
}
