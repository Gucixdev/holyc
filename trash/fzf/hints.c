/* hints.c - the key hint bar (--hints): built-in keys plus every "--bind key:action # label", drawn as key caps.
 * With the kitty keyboard protocol only the hints of the modifiers currently held are shown (like nnn). */
#include "uistate.h"

typedef struct Hint {
	char key[32];          /* the key cap text without modifiers */
	int mods;              /* M_CTRL | M_ALT | M_SHIFT of the key */
	char *label;
	ActLoc acts;           /* what a click runs (may be empty) */
	int x, y, w;           /* where it was drawn last time; w == 0: not shown */
} Hint;

static void key_text(int key, char *out, size_t n)
{
	static const struct { int k; const char *t; } names[] = {
		{K_ENTER, "Enter"}, {K_TAB, "Tab"}, {K_BTAB, "Shift+Tab"}, {K_ESC, "Esc"}, {K_BSPACE, "Backspace"}, {K_UP, "↑"}, {K_DOWN, "↓"},
		{K_LEFT, "←"}, {K_RIGHT, "→"}, {K_HOME, "Home"}, {K_END, "End"}, {K_PGUP, "PgUp"}, {K_PGDN, "PgDn"}, {K_INS, "Ins"}, {K_DEL, "Del"},
		{K_CLICK, "Click"}, {K_DCLICK, "2×Click"}, {K_RCLICK, "RClick"}, {K_SCROLL_UP, "Wheel↑"}, {K_SCROLL_DOWN, "Wheel↓"}, {' ', "Space"}};
	int b = K_BASE(key);
	for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) if (names[i].k == b) { snprintf(out, n, "%s", names[i].t); return; }
	if (b >= K_F1 && b <= K_F12) { snprintf(out, n, "F%d", b - K_F1 + 1); return; }
	rune r = (rune)b;
	if (r >= 'a' && r <= 'z') r -= 32;
	char u[8];
	size_t l = u8enc(r, u);
	u[l] = 0;
	snprintf(out, n, "%s", u);
}

static void add(const char *keytxt, int mods, const char *label, const char *acts)
{
	U.hints = xrealloc(U.hints, (size_t)(U.nhints + 1) * sizeof(Hint));
	Hint *h = &U.hints[U.nhints++];
	memset(h, 0, sizeof *h);
	snprintf(h->key, sizeof h->key, "%s", keytxt);
	h->mods = mods;
	h->label = xstrdup(label);
	if (acts) acts_from_string(&h->acts, acts);
}

void hints_build(void)
{
	if (!U.o->hints) return;
	add("↑↓", 0, "move", NULL);
	add("Enter", 0, "accept", "accept");
	ActLoc *tb = bind_find(U.binds, U.nbinds, K_TAB);
	if (U.o->multi && tb && tb->n && tb->v[0].id == A_TOGGLE) add("Tab", 0, "mark", "toggle+down");
	if (U.pv_cmd) add("↑↓", M_SHIFT, "preview", NULL);
	for (int i = 0; i < U.nbinds; i++) {
		Binding *b = &U.binds[i];
		if (!b->label) continue;
		char kt[32];
		key_text(b->key, kt, sizeof kt);
		add(kt, K_MODS(b->key) & (M_CTRL | M_ALT | M_SHIFT), b->label, NULL);
		Hint *h = &U.hints[U.nhints - 1];
		h->acts.n = b->acts.n;
		h->acts.v = xmalloc((size_t)(b->acts.n + 1) * sizeof(Act));
		for (int k = 0; k < b->acts.n; k++) { h->acts.v[k].id = b->acts.v[k].id; h->acts.v[k].arg = b->acts.v[k].arg ? xstrdup(b->acts.v[k].arg) : NULL; }
	}
	add("Esc", 0, "quit", "abort");
	U.hint_rows = U.o->hints;
}

int hints_rows(void) { return U.nhints ? U.o->hints : 0; }

static bool visible(const Hint *h)
{
	if (!term_kitty_active()) return true;             /* cannot know what is held: show every hint */
	int held = term_mods();
	if (held) return h->mods && !(h->mods & ~held);
	return h->mods == 0;
}

static int cap(Rect r, int x, int y, const char *txt, Col c)
{
	char b[64];
	snprintf(b, sizeof b, " %s ", txt);
	return gs(r, x, y, b, c);
}

void hints_draw(Rect r)
{
	U.hint_rows = r.h;
	Col base = col_mk(COL_DEF, U.th.c[C_FG].bg, 0);
	gfill(r, base);
	Col lab = col_mk(U.th.c[C_INFO].fg, base.bg, 0);
	Col rev = col_mk(COL_DEF, base.bg, ATTR_REV);
	Col cm[3] = {col_mk(0, 4, 0), col_mk(0, 3, 0), col_mk(0, 2, 0)};   /* Ctrl blue, Alt yellow, Shift green */
	int x = r.x, row = 0;
	for (int i = 0; i < U.nhints; i++) U.hints[i].w = 0;
	for (int i = 0; i < U.nhints; i++) {
		Hint *h = &U.hints[i];
		if (!visible(h)) continue;
		int w = str_width(h->key, strlen(h->key)) + 2 + 1 + str_width(h->label, strlen(h->label)) + 3;
		if (h->mods & M_CTRL) w += 6;
		if (h->mods & M_ALT) w += 5;
		if (h->mods & M_SHIFT) w += 7;
		if (x + w - 3 > r.x + r.w && x > r.x) { if (++row >= r.h) break; x = r.x; }
		int y = r.y + row, x0 = x;
		if (h->mods & M_CTRL) x += cap(r, x, y, "Ctrl", cm[0]);
		if (h->mods & M_ALT) x += cap(r, x, y, "Alt", cm[1]);
		if (h->mods & M_SHIFT) x += cap(r, x, y, "Shift", cm[2]);
		x += cap(r, x, y, h->key, rev);
		x += gs(r, x, y, " ", lab);
		x += gs(r, x, y, h->label, lab);
		h->x = x0; h->y = y; h->w = x - x0;
		x += 3;
	}
}

bool hints_click(int x, int y)
{
	for (int i = 0; i < U.nhints; i++) {
		Hint *h = &U.hints[i];
		if (h->w && y == h->y && x >= h->x && x < h->x + h->w) {
			if (h->acts.n) { free(U.last_key); U.last_key = xstrdup("click"); act_run_list(h->acts.v, h->acts.n); }
			return true;
		}
	}
	return false;
}
