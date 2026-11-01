/* scramble.c - text that resolves out of random glyphs as progress goes 0..1; a pure function of (text, progress, time).
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

void lt_scramble(lt_canvas *c, lt_rect r, const lt_theme *t, const char *text, double progress, double time)
{
	if (r.w < 1 || r.h < 1 || !text) return;
	lt_rect all = {0, 0, lt_canvas_w(c), lt_canvas_h(c)};
	bool uni = lt_canvas_caps(c)->unicode;
	if (!(progress > 0)) progress = 0;
	if (progress > 1) progress = 1;
	int total = 0;
	for (const char *s = text; *s;) { uint32_t q; s += lt_utf8_decode(s, &q); total++; }
	int done = (int)floor(progress * total + 1e-9);
	int i = 0, x = 0;
	for (const char *s = text; *s && x < r.w; i++) {
		uint32_t q;
		s += lt_utf8_decode(s, &q);
		if (i < done) lt_put(c, all, r.x + x, r.y, q, t->s[LT_T_TITLE]);
		else if (i == done && progress < 1) {
			lt_style cur = t->s[LT_T_ACCENT]; cur.attr |= LT_REVERSE;
			lt_put(c, all, r.x + x, r.y, glyph(hash32((uint32_t)i * 977U + tick(time * 20.0)), uni), cur);
		} else if (q != ' ') lt_put(c, all, r.x + x, r.y, glyph(hash32((uint32_t)i * 977U + tick(time * 12.0)), uni), t->s[LT_T_DIM]);
		x++;
	}
}
