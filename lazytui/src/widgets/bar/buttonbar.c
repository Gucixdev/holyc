/* buttonbar.c - the dmenu-style strip of buttons. Layout: [prompt ] [<] label label label ... [>] ; a label is drawn with one blank on each
 * side. The window of visible buttons starts at `first` and follows the cursor. Hit testing uses the positions recorded by the last draw. */
#include "internal.h"

struct lt_buttonbar {
	char **label; int n;
	char *prompt;
	int cursor, active, first;
	int *bx, *bw, nvis_from, nvis_to;          /* screen x and width of each drawn button, [from, to) */
	int hit_left, hit_right;                   /* x of the "<" / ">" marks, -1 when absent */
	lt_rect drawn;
};

lt_buttonbar *lt_buttonbar_new(const char *const *labels, int n)
{
	lt_buttonbar *b = calloc(1, sizeof *b);
	b->n = n;
	b->label = calloc((size_t)(n ? n : 1), sizeof *b->label);
	b->bx = calloc((size_t)(n ? n : 1), sizeof(int));
	b->bw = calloc((size_t)(n ? n : 1), sizeof(int));
	for (int i = 0; i < n; i++) b->label[i] = strdup(labels[i]);
	b->active = -1;
	b->hit_left = b->hit_right = -1;
	return b;
}
void lt_buttonbar_free(lt_buttonbar *b)
{
	if (!b) return;
	for (int i = 0; i < b->n; i++) free(b->label[i]);
	free(b->label); free(b->prompt); free(b->bx); free(b->bw); free(b);
}
void lt_buttonbar_set_prompt(lt_buttonbar *b, const char *p) { free(b->prompt); b->prompt = p && *p ? strdup(p) : NULL; }
int lt_buttonbar_cursor(const lt_buttonbar *b) { return b->cursor; }
void lt_buttonbar_set_cursor(lt_buttonbar *b, int i) { if (i >= 0 && i < b->n) b->cursor = i; }
void lt_buttonbar_set_active(lt_buttonbar *b, int i) { b->active = i >= 0 && i < b->n ? i : -1; }
int lt_buttonbar_count(const lt_buttonbar *b) { return b->n; }

static int bw_of(const lt_buttonbar *b, int i) { return lt_utf8_width(b->label[i]) + 2; }

void lt_buttonbar_draw(lt_buttonbar *b, lt_canvas *c, lt_rect r, const lt_theme *t, bool focused)
{
	b->nvis_from = b->nvis_to = 0;
	b->hit_left = b->hit_right = -1;
	if (r.w < 1 || r.h < 1) return;
	r.h = 1;
	lt_rect all = lt_full(c);
	lt_fill(c, r, ' ', t->s[LT_T_TEXT]);
	b->drawn = r;
	int x = r.x, right = r.x + r.w;
	if (b->prompt) x += lt_text(c, all, x, r.y, b->prompt, t->s[LT_T_ACCENT], r.w) + 1;
	if (b->n == 0) return;
	b->cursor = lt_clampi(b->cursor, 0, b->n - 1);
	/* the window starts at `first`, moved so that the cursor fits between the "<" and ">" marks */
	b->first = lt_clampi(b->first, 0, b->n - 1);
	if (b->cursor < b->first) b->first = b->cursor;
	for (;;) {
		int used = b->first > 0 ? 2 : 0, last = b->first;
		for (int i = b->first; i < b->n; i++) { int w = bw_of(b, i) + 1; if (x + used + w > right - 2 && i > b->first) break; used += w; last = i; }
		if (b->cursor <= last || b->first >= b->cursor) break;
		b->first++;
	}
	if (b->first > 0) { lt_put(c, all, x, r.y, lt_canvas_caps(c)->unicode ? 0x25C0 : '<', t->s[LT_T_DIM]); b->hit_left = x; x += 2; }
	int i;
	for (i = b->first; i < b->n; i++) {
		int w = bw_of(b, i);
		if (x + w > right - (i + 1 < b->n ? 2 : 0) && i > b->first) break;
		lt_style st = t->s[LT_T_TEXT];
		if (i == b->active) { st = t->s[LT_T_ACCENT]; st.attr |= LT_UNDERLINE; }
		if (i == b->cursor && focused) st = t->s[LT_T_SELECT];
		else if (i == b->cursor) st.attr |= LT_BOLD;
		lt_fill(c, (lt_rect){x, r.y, w, 1}, ' ', st);
		lt_text(c, all, x + 1, r.y, b->label[i], st, w - 2);
		b->bx[i] = x; b->bw[i] = w;
		x += w + 1;
	}
	b->nvis_from = b->first; b->nvis_to = i;
	if (i < b->n) { b->hit_right = right - 1; lt_put(c, all, right - 1, r.y, lt_canvas_caps(c)->unicode ? 0x25B6 : '>', t->s[LT_T_DIM]); }
}

lt_wres lt_buttonbar_event(lt_buttonbar *b, lt_rect r, const lt_event *ev)
{
	if (b->n == 0) return LT_W_IGNORED;
	int base = ev->key & 0xFFFFFF, mods = ev->key & ~0xFFFFFF, old = b->cursor;
	if (ev->kind == LT_EV_MOUSE) {
		r.h = 1;
		if (base == LT_K_WHEEL_UP || base == LT_K_WHEEL_DOWN) {
			if (!lt_in(r, ev->x, ev->y)) return LT_W_IGNORED;
			b->cursor = lt_clampi(b->cursor + (base == LT_K_WHEEL_UP ? -1 : 1), 0, b->n - 1);
			return b->cursor != old ? LT_W_CHANGED : LT_W_HANDLED;
		}
		if (base != LT_K_CLICK || !lt_in(r, ev->x, ev->y)) return LT_W_IGNORED;
		if (ev->x == b->hit_left && b->hit_left >= 0) { b->first = lt_clampi(b->first - 1, 0, b->n - 1); return LT_W_HANDLED; }
		if (ev->x == b->hit_right && b->hit_right >= 0) { b->first = lt_clampi(b->first + 1, 0, b->n - 1); return LT_W_HANDLED; }
		for (int i = b->nvis_from; i < b->nvis_to; i++)
			if (ev->x >= b->bx[i] && ev->x < b->bx[i] + b->bw[i]) { b->cursor = i; return LT_W_ACTIVATE; }
		return LT_W_HANDLED;
	}
	if (ev->kind != LT_EV_KEY || (mods & ~LT_MOD_SHIFT)) return LT_W_IGNORED;
	switch (ev->key) {
	case LT_K_LEFT: if (b->cursor == 0) return LT_W_IGNORED; b->cursor--; break;                  /* at an edge the key is not ours: a neighbouring bar may take it */
	case LT_K_RIGHT: if (b->cursor == b->n - 1) return LT_W_IGNORED; b->cursor++; break;
	case LT_K_HOME: b->cursor = 0; break;
	case LT_K_END: b->cursor = b->n - 1; break;
	case LT_K_ENTER: return LT_W_ACTIVATE;
	default: return LT_W_IGNORED;
	}
	return b->cursor != old ? LT_W_CHANGED : LT_W_HANDLED;
}
