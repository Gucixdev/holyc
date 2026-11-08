/* rain.c - the matrix look: falling columns with a bright head and a fading trail; a pure function of (position, time).
 * Needs: lazytui.h (the lib core). One widget, one file: copy it into any project that links the core. */
#include "lazytui.h"
#include <math.h>
#include <string.h>

static uint32_t hash32(uint32_t x)
{
	x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
	return x;
}

static uint32_t tick(double x) { if (!isfinite(x)) return 0; return (uint32_t)(int64_t)floor(fmod(x, 1e9)); }

static uint32_t glyph(uint32_t h, bool unicode)
{
	static const char ascii[] = "01234567890:.=*+-<>|/\\^~";
	if (!unicode) return (uint32_t)ascii[h % (sizeof ascii - 1)];
	uint32_t k = h % 60;
	if (k < 10) return '0' + k;                       /* digits */
	if (k < 14) return "=+*:"[k - 10];
	return 0xFF71 + (k - 14) % 45;                   /* halfwidth katakana U+FF71..U+FF9D */
}

void lt_rain(lt_canvas *c, lt_rect r, const lt_theme *t, double time, uint32_t seed)
{
	if (r.w < 1 || r.h < 1 || !isfinite(time)) return;
	lt_rect all = {0, 0, lt_canvas_w(c), lt_canvas_h(c)};
	bool uni = lt_canvas_caps(c)->unicode;
	lt_style head = t->s[LT_T_ACCENT], body = t->s[LT_T_TEXT];
	lt_color ground = t->s[LT_T_TEXT].bg;
	for (int x = 0; x < r.w; x++) {
		uint32_t h = hash32(seed * 2654435761U + (uint32_t)x * 97U + 13U);
		double speed = 4.0 + (h % 900) / 100.0;               /* rows per second: constant velocity, y' = speed */
		int trail = 5 + (int)((h >> 10) % 10);
		int gap = 4 + (int)((h >> 16) % 14);
		double period = r.h + trail + gap;
		double phase = fmod(time * speed + (double)((h >> 20) % 1000) / 1000.0 * period, period);
		if (phase < 0) phase += period;
		int hy = (int)floor(phase);
		for (int k = 0; k <= trail; k++) {
			int y = hy - k;
			if (y < 0 || y >= r.h) continue;
			uint32_t g = hash32(h + (uint32_t)y * 31U + tick(time * (k == 0 ? 12.0 : 2.5)) * 7U);      /* the head flickers, the trail changes slowly */
			lt_style st;
			if (k == 0) st = head;
			else {
				double f = (double)k / (trail + 1);
				st = body;
				st.fg = lt_color_mix(body.fg, ground == LT_DEFAULT ? LT_RGB(0, 0, 0) : ground, f * 0.92);
				if (k == 1) st.attr |= LT_BOLD;
			}
			lt_put(c, all, r.x + x, r.y + y, glyph(g, uni), st);
		}
	}
}
