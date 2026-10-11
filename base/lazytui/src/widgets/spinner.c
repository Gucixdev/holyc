/* spinner.c - six looks for "something is happening": LINE/DOTS/ARC rotate one glyph, WAVE breathes five bars, BOUNCE runs a ball with a
 * fading trail, PARALLAX slides layers of dots at different speeds (near = big + fast). All keep no state; feed them the time. */
#include "lazytui.h"
#include "displayutil.h"
#include <math.h>

static double fin(double v, double d) { return isfinite(v) ? v : d; }

void lt_spinner(lt_canvas *c, lt_rect r, const lt_theme *t, lt_spin_kind kind, double time, const char *label)
{
	if (r.w < 1 || r.h < 1) return;
	lt_rect all = {0, 0, lt_canvas_w(c), lt_canvas_h(c)};
	bool uni = lt_canvas_caps(c)->unicode;
	time = fin(time, 0);
	lt_style acc = t->s[LT_T_ACCENT], dim = t->s[LT_T_DIM], txt = t->s[LT_T_TEXT];
	int used = 0;
	switch (kind) {
	case LT_SPIN_LINE: {
		static const char *asc[4] = {"|", "/", "-", "\\"}, *ux[4] = {"\xE2\x94\x80", "\xE2\x95\xB2", "\xE2\x94\x82", "\xE2\x95\xB1"};
		int f = ((int)floor(time * 10)) & 3;
		used = lt_text(c, all, r.x, r.y, uni ? ux[f] : asc[f], acc, 1);
		break;
	}
	case LT_SPIN_DOTS: {
		static const char *fr[10] = {"\xE2\xA0\x8B", "\xE2\xA0\x99", "\xE2\xA0\xB9", "\xE2\xA0\xB8", "\xE2\xA0\xBC", "\xE2\xA0\xB4", "\xE2\xA0\xA6", "\xE2\xA0\xA7", "\xE2\xA0\x87", "\xE2\xA0\x8F"};
		static const char *asc[4] = {".  ", ".. ", "...", " .."};
		int f = (int)floor(time * 12);
		if (uni) used = lt_text(c, all, r.x, r.y, fr[((f % 10) + 10) % 10], acc, 1);
		else used = lt_text(c, all, r.x, r.y, asc[((f / 2) % 4 + 4) % 4], acc, 3);
		break;
	}
	case LT_SPIN_ARC: {
		static const char *fr[6] = {"\xE2\x97\x9C", "\xE2\x97\xA0", "\xE2\x97\x9D", "\xE2\x97\x9E", "\xE2\x97\xA1", "\xE2\x97\x9F"};
		static const char *asc[4] = {"(", "^", ")", "v"};
		int f = (int)floor(time * 10);
		used = lt_text(c, all, r.x, r.y, uni ? fr[((f % 6) + 6) % 6] : asc[((f % 4) + 4) % 4], acc, 1);
		break;
	}
	case LT_SPIN_WAVE: {                                              /* five bars breathing out of phase */
		static const uint32_t blocks[8] = {0x2581, 0x2582, 0x2583, 0x2584, 0x2585, 0x2586, 0x2587, 0x2588};
		for (int i = 0; i < 5 && i < r.w; i++) {
			double v = 0.5 + 0.5 * sin(time * 5.0 + i * 0.9);
			int lv = (int)floor(v * 7 + 0.5);
			lt_style st = acc;
			st.fg = ramp_or(t, v, LT_T_ACCENT);
			lt_put(c, all, r.x + i, r.y, uni ? blocks[lv] : (uint32_t)"_.:-=+*#"[lv], st);
		}
		used = r.w < 5 ? r.w : 5;
		break;
	}
	case LT_SPIN_BOUNCE: {                                            /* a ball ping-ponging along a track, with a fading trail */
		int lw = label && *label ? lt_utf8_width(label) + 1 : 0;
		int w = r.w - lw;
		if (w > 12) w = 12;
		if (w < 4) w = r.w < 4 ? r.w : 4;
		double span = w - 1, ph = fmod(time * 1.6, 2.0), pos = (ph < 1 ? ph : 2 - ph) * span;
		for (int i = 0; i < w; i++) lt_put(c, all, r.x + i, r.y, uni ? 0x2500 : '-', dim);
		for (int k = 3; k >= 0; k--) {                                 /* trail: where the ball was a moment ago */
			double pt = fmod(time * 1.6 - k * 0.05 + 4.0, 2.0), pp = (pt < 1 ? pt : 2 - pt) * span;
			int x = (int)floor(pp + 0.5);
			lt_style st = k ? dim : acc;
			if (k) st.fg = lt_color_mix(dim.fg == LT_DEFAULT ? acc.fg : dim.fg, acc.fg == LT_DEFAULT ? dim.fg : acc.fg, 1.0 - k * 0.25);
			lt_put(c, all, r.x + x, r.y, k ? (uni ? 0x2022 : '.') : (uni ? 0x25CF : 'O'), st);
		}
		(void)pos;
		used = w;
		break;
	}
	case LT_SPIN_PARALLAX: {                                          /* layers of dots sliding at different speeds: near = big + fast */
		int rows = r.h > 4 ? 4 : r.h;
		for (int d = 0; d < rows; d++) {
			double speed = 3.0 + 6.5 * d;                              /* cells per second */
			int shift = (int)floor(time * speed), period = 5 - d > 2 ? 5 - d : 2, w = r.w;
			int layer = rows - 1 - d;                                  /* row 0 is the far layer */
			for (int x = 0; x < w; x++) {
				int wx = x + shift;
				if (hash32((unsigned)wx, (unsigned)d + 7) % (unsigned)(period + 1) != 0) continue;
				lt_style st = layer == 0 ? acc : layer == 1 ? txt : dim;
				uint32_t ch = uni ? (layer == 0 ? 0x25CF : layer == 1 ? 0x2022 : 0xB7) : (layer == 0 ? 'o' : layer == 1 ? '*' : '.');
				lt_put(c, all, r.x + x, r.y + d, ch, st);
			}
		}
		used = r.w;
		break;
	}
	}
	if (label && *label && kind != LT_SPIN_PARALLAX && r.w > used + 1) lt_text(c, all, r.x + used + 1, r.y, label, txt, r.w - used - 1);
}
