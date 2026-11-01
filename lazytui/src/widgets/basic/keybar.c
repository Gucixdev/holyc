/* keybar.c - Keybar: the fzf-style hint strip, Ctrl/Alt/Shift shown as their own coloured tags
 * Needs: lazytui.h (the lib core). One widget, one file: copy it into any project that links the core. */
#include "internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char text[32]; int mods; char *label; int id; int x, y, w; } khint;
struct lt_keybar { khint *h; int n; };

static void pretty(int key, char *out, size_t n)
{
	static const struct { int k; const char *t; } names[] = {
		{LT_K_ENTER, "Enter"}, {LT_K_TAB, "Tab"}, {LT_K_BTAB, "Tab"}, {LT_K_ESC, "Esc"}, {LT_K_BSPACE, "Backspace"}, {LT_K_UP, "\xE2\x86\x91"},
		{LT_K_DOWN, "\xE2\x86\x93"}, {LT_K_LEFT, "\xE2\x86\x90"}, {LT_K_RIGHT, "\xE2\x86\x92"}, {LT_K_HOME, "Home"}, {LT_K_END, "End"},
		{LT_K_PGUP, "PgUp"}, {LT_K_PGDN, "PgDn"}, {LT_K_INS, "Ins"}, {LT_K_DEL, "Del"}, {LT_K_CLICK, "Click"}, {LT_K_DCLICK, "2\xC3\x97" "Click"},
		{LT_K_RCLICK, "RClick"}, {LT_K_WHEEL_UP, "Wheel\xE2\x86\x91"}, {LT_K_WHEEL_DOWN, "Wheel\xE2\x86\x93"}, {' ', "Space"}};
	int b = key & 0xFFFFFF;
	for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) if (names[i].k == b) { snprintf(out, n, "%s", names[i].t); return; }
	if (b >= LT_K_F1 && b <= LT_K_F12) { snprintf(out, n, "F%d", b - LT_K_F1 + 1); return; }
	uint32_t r = (uint32_t)b;
	if (r >= 'a' && r <= 'z') r -= 32;
	char u[8];
	size_t l = lt_utf8_encode(r, u);
	u[l] = 0;
	snprintf(out, n, "%s", u);
}

lt_keybar *lt_keybar_new(const lt_hint *hints, int n)
{
	lt_keybar *k = calloc(1, sizeof *k);
	k->h = calloc((size_t)(n ? n : 1), sizeof *k->h);
	for (int i = 0; i < n; i++) {
		khint *h = &k->h[k->n++];
		int key = lt_key_parse(hints[i].key);
		if (key < 0 || strchr(hints[i].key, '/') || (strlen(hints[i].key) > 1 && key >= 0 && (key & 0xFFFFFF) < 32)) { snprintf(h->text, sizeof h->text, "%s", hints[i].key); h->mods = 0; }
		else { pretty(key, h->text, sizeof h->text); h->mods = key & (LT_MOD_CTRL | LT_MOD_ALT | LT_MOD_SHIFT); if ((key & 0xFFFFFF) == LT_K_BTAB) h->mods |= LT_MOD_SHIFT; }
		h->label = strdup(hints[i].label ? hints[i].label : "");
		h->id = hints[i].id;
	}
	return k;
}
void lt_keybar_free(lt_keybar *k) { if (!k) return; for (int i = 0; i < k->n; i++) free(k->h[i].label); free(k->h); free(k); }

/* shown when nothing is held: the plain keys; when modifiers are held: the keys that need exactly (a subset of) those */
static bool visible(const khint *h, int held, bool know)
{
	if (!know) return true;
	if (held) return h->mods && !(h->mods & ~held);
	return h->mods == 0;
}
static int hint_width(const khint *h)
{
	int w = lt_utf8_width(h->text) + 2 + 1 + lt_utf8_width(h->label);
	if (h->mods & LT_MOD_CTRL) w += 6;
	if (h->mods & LT_MOD_ALT) w += 5;
	if (h->mods & LT_MOD_SHIFT) w += 7;
	return w;
}

int lt_keybar_rows_needed(const lt_keybar *k, int width, int held, bool know)
{
	int rows = 1, x = 0;
	for (int i = 0; i < k->n; i++) {
		if (!visible(&k->h[i], held, know)) continue;
		int w = hint_width(&k->h[i]);
		if (x > 0 && x + w > width) { rows++; x = 0; }
		x += w + 3;
	}
	return rows;
}

void lt_keybar_draw(lt_keybar *k, lt_canvas *c, lt_rect r, const lt_theme *t, int held, bool know)
{
	lt_rect all = lt_full(c);
	lt_fill(c, r, ' ', lt_slot_style(t, LT_T_TEXT));
	int x = r.x, row = 0;
	for (int i = 0; i < k->n; i++) k->h[i].w = 0;
	for (int i = 0; i < k->n; i++) {
		khint *h = &k->h[i];
		if (!visible(h, held, know)) continue;
		int w = hint_width(h);
		if (x > r.x && x + w > r.x + r.w) { if (++row >= r.h) break; x = r.x; }
		int y = r.y + row, x0 = x;
		char cap[48];
		if (h->mods & LT_MOD_CTRL) x += lt_text(c, all, x, y, " Ctrl ", lt_slot_style(t, LT_T_MOD_CTRL), 6);
		if (h->mods & LT_MOD_ALT) x += lt_text(c, all, x, y, " Alt ", lt_slot_style(t, LT_T_MOD_ALT), 5);
		if (h->mods & LT_MOD_SHIFT) x += lt_text(c, all, x, y, " Shift ", lt_slot_style(t, LT_T_MOD_SHIFT), 7);
		snprintf(cap, sizeof cap, " %s ", h->text);
		x += lt_text(c, all, x, y, cap, lt_slot_style(t, LT_T_KEYCAP), 40);
		x += lt_text(c, all, x, y, " ", lt_slot_style(t, LT_T_LABEL), 1);
		x += lt_text(c, all, x, y, h->label, lt_slot_style(t, LT_T_LABEL), r.x + r.w - x);
		h->x = x0; h->y = y; h->w = x - x0;
		x += 3;
	}
}

int lt_keybar_hit(const lt_keybar *k, int x, int y)
{
	for (int i = 0; i < k->n; i++) {
		const khint *h = &k->h[i];
		if (h->w && y == h->y && x >= h->x && x < h->x + h->w) return h->id;
	}
	return -1;
}
