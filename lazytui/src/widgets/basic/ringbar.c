/* ringbar.c - Ringbar: a marquee - text scrolling through the whole width as an infinite ring, no bar underneath
 * Needs: lazytui.h (the lib core). One widget, one file: copy it into any project that links the core. */
#include "internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void lt_ringbar(lt_canvas *c, lt_rect r, const lt_theme *t, double phase, const char *text, lt_color color)
{
	lt_rect all = lt_full(c);
	r = lt_one_row(r);
	if (r.w < 1) return;
	phase = fmod(lt_finite_or(phase, 0), 1e6);
	if (!text || !*text) return;
	/* the ring: the text, three blanks, and again - repeated to any width */
	int n = 0;
	lt_arena_mark sm_ = lt_arena_save(lt_scratch());
	uint32_t *ring = lt_arena_alloc(lt_scratch(), (strlen(text) + 4) * sizeof *ring);
	for (const char *s = text; *s;) { uint32_t q; s += lt_utf8_decode(s, &q); ring[n++] = q; }
	for (int i = 0; i < 3; i++) ring[n++] = ' ';
	int cells = 0;                                            /* the ring in cells (wide characters count 2) */
	for (int i = 0; i < n; i++) { char tmp[5]; size_t l = lt_utf8_encode(ring[i], tmp); tmp[l] = 0; int w = lt_utf8_width(tmp); cells += w ? w : 1; }
	int off = cells ? ((int)floor(phase) % cells + cells) % cells : 0;
	lt_style st = lt_slot_style(t, LT_T_ACCENT);
	if (color != LT_DEFAULT) st.fg = color;
	int idx = 0, skip = off, col = 0;
	while (col < r.w) {                                        /* walk the ring starting `off` cells in */
		char tmp[5]; size_t l = lt_utf8_encode(ring[idx % n], tmp); tmp[l] = 0;
		int w = lt_utf8_width(tmp); if (w == 0) w = 1;
		if (skip >= w) { skip -= w; idx++; continue; }
		if (skip > 0 || col + w > r.w) { lt_put(c, all, r.x + col, r.y, ' ', st); col++; skip = 0; if (w == 2) idx++; continue; }
		lt_put(c, all, r.x + col, r.y, ring[idx % n], st);
		col += w; idx++;
	}
 	lt_arena_restore(lt_scratch(), sm_);
}
