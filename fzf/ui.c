/* ui.c - the interactive fzf: start-up, the event loop, search results, cursor and selection. */
#include "uistate.h"
#include <fcntl.h>
#include <poll.h>
#include <sys/time.h>
#include <time.h>

UI U;

double now_sec(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

void itemvec_push(ItemVec *v, Item *it)
{
	if (v->n == v->cap) { v->cap = v->cap ? v->cap * 2 : 1024; v->v = xrealloc(v->v, v->cap * sizeof(Item *)); }
	it->index = (uint32_t)v->n;
	v->v[v->n++] = it;
}

static char *last_q; static size_t last_n; static unsigned last_gen; static int last_sig;
static bool dirty = true, auto_checked = false, load_pending;
void ui_dirty(void) { dirty = true; }

/* ------------------------------------------------------------------ query */
char *ui_query_str(void)
{
	Buf b;
	buf_init(&b);
	for (int i = 0; i < U.qn; i++) { char u[4]; size_t n = u8enc(U.q[i], u); buf_addn(&b, u, n); }
	return buf_take(&b);
}

static bool q_changed_flag;
void ui_query_changed(void)
{
	U.need_search = true;
	q_changed_flag = true;
	U.hist_pos = U.nhist;
	dirty = true;
}

void ui_query_set(const char *s)
{
	size_t n = strlen(s);
	free(U.q);
	U.q = xmalloc((n + 1) * sizeof(rune));
	U.qn = (int)utf8_to_runes(s, n, U.q);
	U.qcap = (int)n + 1;
	U.qc = U.qn;
	ui_query_changed();
}

/* ------------------------------------------------------------------ results, cursor, selection */
Item *ui_current(void)
{
	if (!U.nhits || U.cur >= U.nhits) return NULL;
	return U.items.v[U.hits[U.cur].idx];
}

void ui_set_cursor(size_t i)
{
	if (!U.nhits) { U.cur = 0; return; }
	if (i >= U.nhits) i = U.nhits - 1;
	Item *before = ui_current();
	U.cur = i;
	if (ui_current() != before) {
		pv_schedule(); ui_fire(E_FOCUS);
		if (U.track_on && ui_current()) { free(U.track_key); U.track_key = item_key(ui_current()); }
	}
	dirty = true;
}

void ui_move(int d)
{
	if (!U.nhits) return;
	long i = (long)U.cur + d;
	if (i < 0) i = U.o->cycle && U.cur == 0 ? (long)U.nhits - 1 : 0;
	else if (i >= (long)U.nhits) i = U.o->cycle && U.cur == U.nhits - 1 ? 0 : (long)U.nhits - 1;
	ui_set_cursor((size_t)i);
}

void ui_toggle_sel(Item *it, bool on)
{
	bool is = (it->flags & ITEM_SEL) != 0;
	if (is == on) return;
	if (on) {
		if (U.o->multi_max && (int)U.nsel >= U.o->multi_max) return;
		it->flags |= ITEM_SEL;
		if (U.nsel == U.selcap) { U.selcap = U.selcap ? U.selcap * 2 : 16; U.sel = xrealloc(U.sel, U.selcap * sizeof(Item *)); }
		U.sel[U.nsel++] = it;
	} else {
		it->flags &= (uint8_t)~ITEM_SEL;
		for (size_t i = 0; i < U.nsel; i++) if (U.sel[i] == it) { memmove(U.sel + i, U.sel + i + 1, (U.nsel - i - 1) * sizeof(Item *)); U.nsel--; break; }
	}
	dirty = true;
	ui_fire(E_MULTI);
}
void ui_select_all(bool on) { for (size_t i = 0; i < U.nhits; i++) ui_toggle_sel(U.items.v[U.hits[i].idx], on); }
void ui_clear_sel(void)
{
	for (size_t i = 0; i < U.nsel; i++) U.sel[i]->flags &= (uint8_t)~ITEM_SEL;
	U.nsel = 0;
	dirty = true;
}

void ui_items_clear(void)
{
	U.gen++;
	for (size_t i = 0; i < U.items.n; i++) item_free(U.items.v[i]);
	free(U.items.v);
	U.items = (ItemVec){0};
	for (size_t i = 0; i < U.hdr.n; i++) item_free(U.hdr.v[i]);
	free(U.hdr.v);
	U.hdr = (ItemVec){0};
	free(U.hits); U.hits = NULL; U.nhits = 0;
	U.nsel = 0;
	U.cur = U.top = 0;
	dirty = true;
}

static int cmp_idx(const void *a, const void *b)
{
	uint32_t x = ((const Hit *)a)->idx, y = ((const Hit *)b)->idx;
	return x < y ? -1 : x > y;
}

void ui_run_search(void)
{
	U.need_search = false;
	char *q = ui_query_str();
	Pattern *np = pattern_new(U.o, U.o->disabled ? "" : q);
	free(q);
	char *keep = (U.track_on || U.track_have) && U.track_key ? xstrdup(U.track_key) : NULL;
	Item *old = ui_current();
	size_t oldpos = U.cur;
	Hit *nh;
	size_t nn;
	const char *qs = U.o->disabled ? "" : NULL;
	char *qcopy = qs ? xstrdup(qs) : ui_query_str();
	int sig = (U.o->sort ? 1 : 0) | (U.o->disabled ? 2 : 0) | (U.o->tac ? 4 : 0) | (U.o->nnth << 3);
	bool narrow = last_q && *last_q && last_n == U.items.n && last_gen == U.gen && last_sig == sig && U.hits &&
	              !strpbrk(qcopy, "!|$") && !strpbrk(last_q, "!|$") && !strncmp(qcopy, last_q, strlen(last_q));
	if (narrow) {                              /* the new query only adds to the old one: search the old matches, in input order */
		size_t k = U.nhits;
		Hit *sorted_prev = xmalloc((k + 1) * sizeof(Hit));
		memcpy(sorted_prev, U.hits, k * sizeof(Hit));
		qsort(sorted_prev, k, sizeof(Hit), cmp_idx);
		Item **sub = xmalloc((k + 1) * sizeof(Item *));
		for (size_t i = 0; i < k; i++) sub[i] = U.items.v[sorted_prev[i].idx];
		nh = search_items(U.o, np, sub, k, &nn);
		for (size_t i = 0; i < nn; i++) nh[i].idx = sorted_prev[nh[i].idx].idx;
		free(sub); free(sorted_prev);
	} else nh = search_items(U.o, np, U.items.v, U.items.n, &nn);
	free(U.hits);
	U.hits = nh; U.nhits = nn;
	free(last_q); last_q = qcopy; last_n = U.items.n; last_gen = U.gen; last_sig = sig;
	if (U.pat) pattern_free(U.pat);
	U.pat = np;
	if (q_changed_flag) U.cur = U.top = 0;
	if (keep) {
		for (size_t i = 0; i < U.nhits; i++) {
			char *k = item_key(U.items.v[U.hits[i].idx]);
			bool same = !strcmp(k, keep);
			free(k);
			if (same) { U.cur = i; break; }
		}
		free(keep);
	}
	if (U.cur >= U.nhits) U.cur = U.nhits ? U.nhits - 1 : 0;
	(void)oldpos;
	if (U.track_on && !U.track_key && ui_current()) U.track_key = item_key(ui_current());
	bool qch = q_changed_flag;
	q_changed_flag = false;
	if (ui_current() != old) pv_schedule();
	dirty = true;
	if (qch && !U.in_change_event) { U.in_change_event = true; ui_fire(E_CHANGE); U.in_change_event = false; }
	ui_fire(E_RESULT);
	if (load_pending && !U.reload.live) { load_pending = false; ui_fire(E_LOAD); }
	if (U.nhits == 1 && qch) ui_fire(E_ONE);
	if (U.nhits == 0) ui_fire(E_ZERO);
}

/* ------------------------------------------------------------------ events */
void ui_fire(int ev)
{
	ActLoc *a = bind_find(U.binds, U.nbinds, ev);
	if (!a) return;
	for (int i = 0; i < U.nunbound; i++) if (U.unbound[i] == ev) return;
	free(U.last_key); U.last_key = xstrdup("");
	act_run_list(a->v, a->n);
}


void ui_after_feed(void)
{
	dirty = true;
	load_pending = true;               /* fired once the search over the final items has run */
}

/* --preview-window */
static void pw_defaults(PW *p)
{
	memset(p, 0, sizeof *p);
	p->pos = PW_RIGHT; p->size = 50; p->pct = true; p->border = BORDER_ROUNDED; p->info = true;
}
static bool is_offset_token(const char *t)
{
	if (*t == '+' || *t == '/' ) return true;
	if (*t == '-' && (t[1] == '/' || isdigit((unsigned char)t[1]))) return true;
	return false;
}
static void pw_parse(PW *p, const char *in)
{
	const char *s = in;
	while (*s) {
		while (*s == ',' || *s == ':') s++;
		if (!*s) break;
		if (*s == '<') {                                       /* <N(alt) */
			int th = atoi(s + 1);
			const char *open = strchr(s, '('), *close = open ? strchr(open, ')') : NULL;
			if (!open || !close) die("invalid preview window option: %s", s);
			char *alt = xstrndup(open + 1, (size_t)(close - open - 1));
			p->threshold = th;
			p->alt = xmalloc(sizeof *p->alt);
			*p->alt = *p; p->alt->alt = NULL; p->alt->hidden = false;
			pw_parse(p->alt, alt);
			free(alt);
			s = close + 1;
			continue;
		}
		const char *e = s;
		while (*e && *e != ',' && *e != ':') e++;
		char *t = xstrndup(s, (size_t)(e - s));
		s = e;
		#define IS(x) (!strcmp(t, x))
		if (IS("default")) { PW keep_alt = *p; pw_defaults(p); (void)keep_alt; }
		else if (IS("hidden")) p->hidden = true;
		else if (IS("nohidden")) p->hidden = false;
		else if (IS("wrap") || IS("wrap-word")) p->wrap = true;
		else if (IS("nowrap")) p->wrap = false;
		else if (IS("cycle")) p->cycle = true;
		else if (IS("nocycle")) p->cycle = false;
		else if (IS("up") || IS("top")) p->pos = PW_UP;
		else if (IS("down") || IS("bottom")) p->pos = PW_DOWN;
		else if (IS("left")) p->pos = PW_LEFT;
		else if (IS("right")) p->pos = PW_RIGHT;
		else if (IS("border") || IS("rounded") || IS("border-rounded")) p->border = BORDER_ROUNDED;
		else if (IS("sharp") || IS("border-sharp")) p->border = BORDER_SHARP;
		else if (IS("border-bold")) p->border = BORDER_BOLD;
		else if (IS("border-double")) p->border = BORDER_DOUBLE;
		else if (IS("noborder") || IS("border-none")) p->border = BORDER_NONE;
		else if (IS("border-horizontal")) p->border = BORDER_HORIZONTAL;
		else if (IS("border-vertical")) p->border = BORDER_VERTICAL;
		else if (IS("border-up") || IS("border-top")) p->border = BORDER_TOP;
		else if (IS("border-down") || IS("border-bottom")) p->border = BORDER_BOTTOM;
		else if (IS("border-left")) p->border = BORDER_LEFT;
		else if (IS("border-right")) p->border = BORDER_RIGHT;
		else if (IS("follow")) p->follow = true;
		else if (IS("nofollow")) p->follow = false;
		else if (IS("info")) p->info = true;
		else if (IS("noinfo")) p->info = false;
		else if (t[0] == '~' && isdigit((unsigned char)t[1])) p->header_lines = atoi(t + 1);
		else if (isdigit((unsigned char)t[0]) && strspn(t, "0123456789") + (t[strlen(t) - 1] == '%') >= strlen(t)) {
			p->size = atoi(t); p->pct = t[strlen(t) - 1] == '%';
		} else if (is_offset_token(t)) { free(p->scroll); p->scroll = xstrdup(t); }
		else die("invalid preview window option: %s", t);
		#undef IS
		free(t);
	}
}

void ui_pw_apply(const char *spec)
{
	static int cycle_i;
	const char *bar = strchr(spec, '|');
	if (!bar) { pw_parse(&U.pw, spec); }
	else {                                                    /* alternatives: each call advances to the next one */
		char *c = xstrdup(spec), *sv = NULL;
		int n = 0, want = cycle_i++;
		for (char *t = strtok_r(c, "|", &sv); t; t = strtok_r(NULL, "|", &sv)) n++;
		free(c);
		c = xstrdup(spec);
		int k = 0;
		for (char *t = strtok_r(c, "|", &sv); t; t = strtok_r(NULL, "|", &sv), k++)
			if (k == want % n) { pw_parse(&U.pw, t); break; }
		free(c);
	}
	U.pv_hidden = U.pw.hidden;
	U.pv_wrap = U.pw.wrap;
	dirty = true;
	pv_schedule();
}

/* ------------------------------------------------------------------ geometry */
static int height_rows(int rows)
{
	const char *s = U.o->height_s;
	if (!s || !*s) return rows;
	if (*s == '~') s++;
	int h;
	bool neg = *s == '-';
	if (neg) s++;
	char *e;
	long v = strtol(s, &e, 10);
	if (*e == '%') h = (int)((long)rows * v / 100);
	else h = (int)v;
	if (neg) h = rows - h;
	if (*e == '%' && h < U.o->min_height) h = U.o->min_height;
	if (h > rows) h = rows;
	if (h < 3) h = 3 < rows ? 3 : rows;
	return h;
}

void ui_resize(void)
{
	int rows, cols;
	term_size(&rows, &cols);
	U.rows = rows; U.cols = cols;
	int h = U.fullscreen ? rows : height_rows(rows);
	if (!U.fullscreen && U.top_row + h > rows) U.top_row = rows - h;
	if (U.top_row < 0) U.top_row = 0;
	grid_init(cols, h);
	dirty = true;
}

static void reserve_inline(void)
{
	int rows, cols;
	term_size(&rows, &cols);
	int h = height_rows(rows);
	int r = term_cursor_row();
	if (r < 1) r = rows;
	term_outs("\r");
	for (int i = 0; i < h - 1; i++) term_outs("\n");
	term_flush();
	int last = r + h - 1;
	if (last > rows) last = rows;
	U.top_row = last - h;
}

static void clear_inline(void)
{
	if (U.fullscreen) return;
	for (int y = 0; y < U.g.h; y++) term_printf("\033[%d;1H\033[2K", U.top_row + y + 1);
	term_printf("\033[%d;1H", U.top_row + 1);
}

/* ------------------------------------------------------------------ events from the terminal */
static bool is_unbound(int k) { for (int i = 0; i < U.nunbound; i++) if (U.unbound[i] == k) return true; return false; }

static char *key_name(int k)
{
	char b[48];
	int base = K_BASE(k), m = K_MODS(k);
	const char *n = NULL;
	static const struct { int k; const char *n; } t[] = {{K_ENTER, "enter"}, {K_TAB, "tab"}, {K_BTAB, "btab"}, {K_ESC, "esc"}, {K_BSPACE, "bspace"},
		{K_UP, "up"}, {K_DOWN, "down"}, {K_LEFT, "left"}, {K_RIGHT, "right"}, {K_HOME, "home"}, {K_END, "end"}, {K_PGUP, "pgup"}, {K_PGDN, "pgdn"},
		{K_INS, "ins"}, {K_DEL, "del"}, {K_CLICK, "click"}, {K_DCLICK, "double-click"}, {K_RCLICK, "right-click"}, {K_SCROLL_UP, "scroll-up"},
		{K_SCROLL_DOWN, "scroll-down"}, {' ', "space"}};
	for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (t[i].k == base) n = t[i].n;
	char tmp[8];
	if (!n) {
		if (base >= K_F1 && base <= K_F12) { snprintf(tmp, sizeof tmp, "f%d", base - K_F1 + 1); n = tmp; }
		else { size_t l = u8enc((rune)base, tmp); tmp[l] = 0; n = tmp; }
	}
	snprintf(b, sizeof b, "%s%s%s%s", m & M_CTRL ? "ctrl-" : "", m & M_ALT ? "alt-" : "", m & M_SHIFT ? "shift-" : "", n);
	return xstrdup(b);
}

static bool in_rect(Rect r, int x, int y) { return r.w > 0 && x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

static bool click_list(int x, int y)
{
	Rect r = U.list_rect;
	if (!in_rect(r, x, y)) return false;
	int row = U.o->layout == LAYOUT_DEFAULT ? r.y + r.h - 1 - y : y - r.y;
	size_t hi = U.top + (size_t)row;
	if (U.o->wrap && U.rowmap) { hi = U.rowmap[y - r.y]; if (hi == (size_t)-1) return true; }
	if (hi < U.nhits) ui_set_cursor(hi);
	return true;
}

static void handle_key(int key)
{
	if (U.jump_mode) {
		U.jump_mode = false;
		static const char *labels = "asdfghjklqwertyuiopzxcvbnm1234567890";
		int b = K_BASE(key);
		if (key == K_ESC) { ui_fire(E_JUMP_CANCEL); return; }
		const char *p = b < 128 ? strchr(labels, b) : NULL;
		if (p) {
			size_t i = (size_t)(p - labels);
			size_t hi = U.top + i;
			if (hi < U.nhits) { ui_set_cursor(hi); ui_fire(E_JUMP); }
		} else ui_fire(E_JUMP_CANCEL);
		dirty = true;
		return;
	}
	if (!is_unbound(key)) {
		ActLoc *a = bind_find(U.binds, U.nbinds, key);
		if (a) {
			free(U.last_key); U.last_key = key_name(key);
			for (int i = 0; i < U.nexpects; i++) if (U.expects[i] && !strcmp(U.expects[i], U.last_key)) { free(U.expect_key); U.expect_key = xstrdup(U.last_key); }
			if (U.expect_key) { U.accepted = true; U.exit_code = (U.nhits == 0 && U.nsel == 0) ? 1 : 0; U.running = false; return; }
			act_run_list(a->v, a->n);
			dirty = true;
			return;
		}
	}
	free(U.last_key); U.last_key = key_name(key);
	for (int i = 0; i < U.nexpects; i++) if (U.expects[i] && !strcmp(U.expects[i], U.last_key)) {
		free(U.expect_key); U.expect_key = xstrdup(U.last_key);
		U.accepted = true; U.exit_code = (U.nhits == 0 && U.nsel == 0) ? 1 : 0; U.running = false; return;
	}
	if (act_query_key(key)) dirty = true;
}

static void handle_mouse(const Ev *e)
{
	int k = e->key;
	int base = K_BASE(k);
	int x = e->x, y = e->y - U.top_row;
	if (base == K_SCROLL_UP || base == K_SCROLL_DOWN) {
		if (in_rect(U.pv_rect, x, y) && U.pv_cmd && !U.pv_hidden) { pv_scroll(base == K_SCROLL_UP ? -3 : 3); dirty = true; return; }
		handle_key(k);
		return;
	}
	if (base == K_CLICK && hints_click(x, y)) { dirty = true; return; }
	if (base == K_CLICK) {
		double t = now_sec();
		bool dbl = t - U.last_click_at < 0.4 && U.click_x == x && U.click_y == y;
		U.last_click_at = dbl ? 0 : t; U.click_x = x; U.click_y = y;
		if (in_rect(U.prompt_rect, x, y) && !U.hide_input) {
			int pw = str_width(U.prompt, strlen(U.prompt));
			int cx = x - U.prompt_rect.x - pw, acc = 0, i = 0;
			while (i < U.qn && acc + rune_width(U.q[i]) <= cx) acc += rune_width(U.q[i++]);
			U.qc = i;
			dirty = true;
		} else if (click_list(x, y)) {
			if (dbl) { handle_key(K_DCLICK | K_MODS(k)); return; }
		}
		handle_key(k);
		return;
	}
	if (base == K_RCLICK) {
		if (click_list(x, y) && U.o->multi) { Item *it = ui_current(); if (it) ui_toggle_sel(it, !(it->flags & ITEM_SEL)); }
		handle_key(k);
		return;
	}
	handle_key(k);
}

/* ------------------------------------------------------------------ output */
static void print_item(const Item *it)
{
	if (U.o->ansi) {
		Buf b;
		buf_init(&b);
		ansi_extract(it->orig, it->olen, &b, NULL, NULL, NULL);
		fwrite(b.p ? b.p : "", 1, b.len, stdout);
		buf_free(&b);
	} else fwrite(it->orig, 1, it->olen, stdout);
	fputc(U.o->print0 ? 0 : '\n', stdout);
}

static void print_result(void)
{
	Opts *o = U.o;
	if (U.printq.len) fwrite(U.printq.p, 1, U.printq.len, stdout);
	if (!U.accepted) { if (U.printq.len) fflush(stdout); return; }
	if (o->print_query) { char *q = ui_query_str(); fputs(q, stdout); fputc(o->print0 ? 0 : '\n', stdout); free(q); }
	if (o->expect) { fputs(U.expect_key ? U.expect_key : "", stdout); fputc(o->print0 ? 0 : '\n', stdout); }
	if (U.nsel) { for (size_t i = 0; i < U.nsel; i++) print_item(U.sel[i]); }
	else { Item *c = ui_current(); if (c) print_item(c); }
	fflush(stdout);
}

/* ------------------------------------------------------------------ start-up and loop */
static void setup_binds(void)
{
	Opts *o = U.o;
	bind_defaults(&U.binds, &U.nbinds);
	if (o->multi) {
		ActLoc a;
		acts_from_string(&a, "toggle+down");
		Binding *b = NULL; int n = 0;
		(void)b; (void)n;
		U.binds = xrealloc(U.binds, (size_t)(U.nbinds + 2) * sizeof(Binding));
		U.binds[U.nbinds].key = K_TAB; U.binds[U.nbinds].label = NULL; U.binds[U.nbinds++].acts = a;
		acts_from_string(&a, "toggle+up");
		U.binds[U.nbinds].key = K_BTAB; U.binds[U.nbinds].label = NULL; U.binds[U.nbinds++].acts = a;
	}
	if (o->history_file) {
		bind_parse(&U.binds, &U.nbinds, "ctrl-p:prev-history,ctrl-n:next-history");
	}
	for (int i = 0; i < o->nbind_raw; i++) bind_parse(&U.binds, &U.nbinds, o->bind_raw[i]);
	if (o->expect) {
		char *c = xstrdup(o->expect), *sv = NULL;
		for (char *t = strtok_r(c, ",", &sv); t; t = strtok_r(NULL, ",", &sv)) {
			int k = key_parse(t);
			if (k < 0) die("unknown key: %s", t);
			U.expects = xrealloc(U.expects, (size_t)(U.nexpects + 1) * sizeof(char *));
			char *canon = key_name(k);
			U.expects[U.nexpects++] = canon;
		}
		free(c);
	}
}

int ui_main(Opts *o)
{
	memset(&U, 0, sizeof U);
	U.o = o;
	theme_default(&U.th);
	if (o->no_color || getenv("NO_COLOR")) U.th.colored = false;
	theme_load_file(&U.th);
	if (o->color_spec) theme_apply(&U.th, o->color_spec);
	theme_resolve(&U.th);
	if (o->ascii && !strcmp(o->pointer, "▌")) { free(o->pointer); o->pointer = xstrdup(">"); }
	if (o->ascii && !strcmp(o->marker, "┃")) { free(o->marker); o->marker = xstrdup(">"); }
	U.sc = scratch_new();
	U.prompt = xstrdup(o->prompt); U.header = o->header ? xstrdup(o->header) : NULL; U.footer = o->footer ? xstrdup(o->footer) : NULL;
	U.border_label = o->border_label ? xstrdup(o->border_label) : NULL; U.preview_label = o->preview_label ? xstrdup(o->preview_label) : NULL;
	U.hdr_left = o->header_lines;
	U.pv_cmd = o->preview_cmd ? xstrdup(o->preview_cmd) : NULL;
	pw_defaults(&U.pw);
	if (o->preview_window_spec) pw_parse(&U.pw, o->preview_window_spec);
	U.pv_hidden = U.pw.hidden; U.pv_wrap = U.pw.wrap;
	U.pw_active = U.pw;
	U.searching_disabled = o->disabled;
	U.track_on = o->track;
	U.running = true; U.exit_code = 130;
	buf_init(&U.pv_out); buf_init(&U.printq); buf_init(&U.reload_part);
	U.reload.fd = U.pv.fd = -1;
	ui_query_set(o->query);
	q_changed_flag = false;
	setup_binds();
	history_load();
	U.hide_input = o->no_input; o->no_input = false;
	listen_start();
	feed_setup();

	U.fullscreen = !o->height_s || !*o->height_s || !strcmp(o->height_s, "100%");
	const char *tt = getenv("TERM");
	bool no_altscreen = tt && !strncmp(tt, "linux", 5);      /* the Linux console has no alternate screen: draw inline and clean up on exit */
	if (no_altscreen && U.fullscreen) U.fullscreen = false;
	if (!term_open(U.fullscreen, o->mouse)) die("cannot open /dev/tty");
	if (!U.fullscreen) reserve_inline();
	if (o->hints && !no_altscreen) term_kitty(true);       /* CSI ? u would restore the cursor on the Linux console */
	hints_build();
	ui_resize();
	ui_run_search();
	bool started = true;
	ui_fire(E_START);
	bool wait_input = o->select1 || o->exit0 || o->sync_;

	while (U.running) {
		U.now = now_sec();
		struct pollfd pf[7];
		int np = 0;
		pf[np++] = (struct pollfd){term_fd(), POLLIN, 0};
		int i_feed = -1, i_rel = -1, i_pv = -1, i_lis = -1;
		if (U.listen_fd > 0) { i_lis = np; pf[np++] = (struct pollfd){U.listen_fd, POLLIN, 0}; }
		if (U.feed.live) { i_feed = np; pf[np++] = (struct pollfd){U.feed.fd, POLLIN, 0}; }
		if (U.reload.live) { i_rel = np; pf[np++] = (struct pollfd){U.reload.fd, POLLIN, 0}; }
		if (U.pv.live) { i_pv = np; pf[np++] = (struct pollfd){U.pv.fd, POLLIN, 0}; }
		int timeout = -1;
		if (U.pv_pending) { int t = (int)((U.pv_start_at - U.now) * 1000) + 1; timeout = t < 0 ? 0 : t; }
		if (!U.feed_done && !wait_input) { if (timeout < 0 || timeout > 100) timeout = 100; }
		if (U.need_search && U.feed.live && U.search_at > U.now) { int t = (int)((U.search_at - U.now) * 1000) + 1; if (timeout < 0 || t < timeout) timeout = t; }
		if (term_pending()) timeout = 0;
		poll(pf, (nfds_t)np, timeout);
		U.now = now_sec();

		if (term_resized()) ui_resize();
		if (term_mods_dirty()) dirty = true;
		Ev e;
		int guard = 0;
		while (U.running && guard++ < 200 && term_read(&e)) {
			if (e.key == K_PASTE) { act_paste(e.paste ? e.paste : ""); free(e.paste); dirty = true; continue; }
			if (e.key == K_FOCUS_IN || e.key == K_FOCUS_OUT) continue;
			if (e.mouse) { if (o->mouse) handle_mouse(&e); continue; }
			handle_key(e.key);
		}
		if (term_mods_dirty()) dirty = true;
		if (!U.running) break;
		if (i_lis >= 0 && (pf[i_lis].revents & POLLIN)) { listen_poll(); dirty = true; }
		if (i_feed >= 0 && (pf[i_feed].revents & (POLLIN | POLLHUP | POLLERR))) feed_read();
		if (i_rel >= 0 && (pf[i_rel].revents & (POLLIN | POLLHUP | POLLERR))) reload_read();
		if (i_pv >= 0 && (pf[i_pv].revents & (POLLIN | POLLHUP | POLLERR))) { pv_poll(); dirty = true; }
		if (U.need_search) {
			bool stream = U.feed.live || U.reload.live;
			if (stream && U.items.n > 20000 && U.now < U.search_at) { /* wait: throttled while a big input streams in */ }
			else { ui_run_search(); U.search_at = U.now + (stream ? 0.1 : 0); }
		}
		if (wait_input && !auto_checked && U.feed_done && !U.need_search) {
			auto_checked = true;
			if (o->select1 && U.nhits == 1) { U.accepted = true; U.exit_code = 0; break; }
			if (o->exit0 && U.nhits == 0) { U.accepted = false; U.exit_code = 1; break; }
		}
		if (U.pv_pending && U.now >= U.pv_start_at) { pv_start(); dirty = true; }
		if (!U.feed_done && !wait_input) dirty = true;             /* spinner */
		if (dirty && (!wait_input || auto_checked)) {
			dirty = false;
			draw_all();
			present();
			if (!started) { started = true; ui_fire(E_START); }
		}
	}
	proc_kill(&U.pv); proc_kill(&U.reload); proc_kill(&U.feed);
	if (U.accepted && o->history_file) { char *q = ui_query_str(); history_save(q); free(q); }
	if (!U.fullscreen) clear_inline();
	term_close();
	if (U.become_cmd) { proc_become(U.become_cmd); return 127; }
	print_result();
	return U.exit_code;
}
