/* textbar.c - Textbar: a full-width line of text in a slot's colour (a status line, a footer)
 * Needs: lazytui.h (the lib core). One widget, one file: copy it into any project that links the core. */
#include "internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void lt_textbar(lt_canvas *c, lt_rect r, const lt_theme *t, const char *text, lt_slot slot)
{
	r = lt_one_row(r);
	lt_style st = lt_slot_style(t, slot);
	lt_fill(c, r, ' ', st);
	lt_text(c, lt_full(c), r.x + 1, r.y, text ? text : "", st, r.w - 2 > 0 ? r.w - 2 : 0);
}
