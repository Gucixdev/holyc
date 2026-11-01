/* panel.c - Panel: the bordered frame every group of widgets sits in (round/sharp/double/HUD)
 * Needs: lazytui.h (the lib core). One widget, one file: copy it into any project that links the core. */
#include "internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

lt_rect lt_panel_inner(lt_rect r) { return (lt_rect){r.x + 2, r.y + 1, r.w - 4 > 0 ? r.w - 4 : 0, r.h - 2 > 0 ? r.h - 2 : 0}; }

void lt_panel_ex(lt_canvas *c, lt_rect r, const lt_theme *t, const char *title, lt_panel_style style)
{
	lt_rect all = lt_full(c);
	if (r.w < 2 || r.h < 2) return;
	lt_style b = lt_slot_style(t, LT_T_BORDER);
	uint32_t hz = 0x2500, vt = 0x2502, tl = 0x256D, tr = 0x256E, bl = 0x2570, br = 0x256F;
	if (style == LT_PANEL_SHARP) { tl = 0x250C; tr = 0x2510; bl = 0x2514; br = 0x2518; }
	else if (style == LT_PANEL_DOUBLE) { hz = 0x2550; vt = 0x2551; tl = 0x2554; tr = 0x2557; bl = 0x255A; br = 0x255D; }
	else if (style == LT_PANEL_HUD) { tl = 0x250F; tr = 0x2513; bl = 0x2517; br = 0x251B; }
	for (int x = r.x + 1; x < r.x + r.w - 1; x++) { lt_put(c, all, x, r.y, hz, b); lt_put(c, all, x, r.y + r.h - 1, hz, b); }
	for (int y = r.y + 1; y < r.y + r.h - 1; y++) { lt_put(c, all, r.x, y, vt, b); lt_put(c, all, r.x + r.w - 1, y, vt, b); }
	lt_put(c, all, r.x, r.y, tl, b); lt_put(c, all, r.x + r.w - 1, r.y, tr, b);
	lt_put(c, all, r.x, r.y + r.h - 1, bl, b); lt_put(c, all, r.x + r.w - 1, r.y + r.h - 1, br, b);
	if (title && *title && r.w > 8) {
		bool hud = style == LT_PANEL_HUD;
		int tw = lt_utf8_width(title);
		int room = r.w - (hud ? 12 : 6);
		if (tw > room) tw = room > 0 ? room : 0;
		int x = r.x + 2;
		if (hud) x += lt_text(c, all, x, r.y, "[", b, 1);
		x += lt_text(c, all, x, r.y, " ", lt_slot_style(t, LT_T_TITLE), 1);
		x += lt_text(c, all, x, r.y, title, lt_slot_style(t, LT_T_TITLE), tw);
		x += lt_text(c, all, x, r.y, " ", lt_slot_style(t, LT_T_TITLE), 1);
		if (hud) lt_text(c, all, x, r.y, "]", b, 1);
	}
	if (style == LT_PANEL_HUD && r.w > 14) {                    /* status pips on the right of the top edge */
		lt_put(c, all, r.x + r.w - 8, r.y, 0x25AA, lt_slot_style(t, LT_T_ACCENT));
		lt_put(c, all, r.x + r.w - 6, r.y, 0x25AA, lt_slot_style(t, LT_T_OK));
		lt_put(c, all, r.x + r.w - 4, r.y, 0x25AB, lt_slot_style(t, LT_T_DIM));
	}
}
void lt_panel(lt_canvas *c, lt_rect r, const lt_theme *t, const char *title) { lt_panel_ex(c, r, t, title, LT_PANEL_ROUND); }
