/* tagcloud.c - words centred on wrapped lines, weight -> size/emphasis.
 * Needs: lazytui.h (the lib core). One widget, one file: copy it into any project that links the core. */
#include "lazytui.h"
#include <math.h>

/* the breathing room around a word: heavier words get more of it, light ones get crammed tight - a "size" signal that reads even in a
 * low-contrast or monochrome theme, where the colour/bold emphasis below can wash out */
static int tagcloud_pad(double f) { return f > 0.66 ? 3 : f > 0.33 ? 2 : 1; }

void lt_tagcloud(lt_canvas *c, lt_rect r, const lt_theme *t, const char *const *words, const double *weights, int n)
{
	if (r.w < 4 || r.h < 1 || !words || n <= 0) return;
	lt_rect all = {0, 0, lt_canvas_w(c), lt_canvas_h(c)};
	double mx = 0, mn = INFINITY;
	for (int i = 0; weights && i < n; i++) if (isfinite(weights[i])) { if (weights[i] > mx) mx = weights[i]; if (weights[i] < mn) mn = weights[i]; }
	if (!isfinite(mn)) mn = 0;
	/* pass 1: the words of each line; pass 2: draw the line centred */
	int i = 0, y = r.y;
	while (i < n && y < r.y + r.h) {
		int j = i, used = 0;
		while (j < n) {
			double f = weights && mx > mn && isfinite(weights[j]) ? (weights[j] - mn) / (mx - mn) : 0.5;
			int w = lt_utf8_width(words[j] ? words[j] : "") + (j > i ? tagcloud_pad(f) : 0);
			if (used + w > r.w && j > i) break;
			used += w; j++;
		}
		int x = r.x + (r.w - used) / 2;
		for (int k = i; k < j; k++) {
			const char *word = words[k] ? words[k] : "";
			double f = weights && mx > mn && isfinite(weights[k]) ? (weights[k] - mn) / (mx - mn) : 0.5;
			lt_style st = f > 0.66 ? t->s[LT_T_ACCENT] : f > 0.33 ? t->s[LT_T_TEXT] : t->s[LT_T_DIM];
			if (t->ngrad > 0) st.fg = lt_theme_ramp(t, f);
			if (f > 0.66) st.attr |= LT_BOLD;
			int w = lt_utf8_width(word);
			if (w > r.w) w = r.w;
			lt_text(c, all, x, y, word, st, w);
			x += w + tagcloud_pad(f);
		}
		i = j; y++;
	}
}
