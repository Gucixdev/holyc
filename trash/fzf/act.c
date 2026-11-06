/* act.c - what an action does: cursor and selection moves, query editing, preview control, running commands. */
#include "uistate.h"
#include <poll.h>

/* ------------------------------------------------------------------ query editing */
static void q_grow(int extra)
{
	if (U.qn + extra + 1 > U.qcap) { U.qcap = (U.qn + extra + 1) * 2; U.q = xrealloc(U.q, (size_t)U.qcap * sizeof(rune)); }
}
static void q_insert(const rune *r, int n)
{
	q_grow(n);
	memmove(U.q + U.qc + n, U.q + U.qc, (size_t)(U.qn - U.qc) * sizeof(rune));
	memcpy(U.q + U.qc, r, (size_t)n * sizeof(rune));
	U.qn += n; U.qc += n;
	ui_query_changed();
}
static void q_delete(int from, int to)      /* [from, to) */
{
	if (from < 0) from = 0;
	if (to > U.qn) to = U.qn;
	if (to <= from) return;
	free(U.yank);
	U.nyank = to - from;
	U.yank = xmalloc((size_t)U.nyank * sizeof(rune));
	memcpy(U.yank, U.q + from, (size_t)U.nyank * sizeof(rune));
	memmove(U.q + from, U.q + to, (size_t)(U.qn - to) * sizeof(rune));
	U.qn -= to - from;
	if (U.qc > to) U.qc -= to - from; else if (U.qc > from) U.qc = from;
	ui_query_changed();
}
static void q_delete_nokill(int from, int to)
{
	if (from < 0) from = 0;
	if (to > U.qn) to = U.qn;
	if (to <= from) return;
	memmove(U.q + from, U.q + to, (size_t)(U.qn - to) * sizeof(rune));
	U.qn -= to - from;
	if (U.qc > to) U.qc -= to - from; else if (U.qc > from) U.qc = from;
	ui_query_changed();
}
static bool wordch(rune r) { return iswalnum((wint_t)r) || r == '_'; }
static int word_back(int i, bool ws)      /* start of the word before i */
{
	while (i > 0 && (ws ? rune_is_space(U.q[i - 1]) : !wordch(U.q[i - 1]))) i--;
	while (i > 0 && (ws ? !rune_is_space(U.q[i - 1]) : wordch(U.q[i - 1]))) i--;
	return i;
}
static int word_fwd(int i, bool ws)
{
	while (i < U.qn && (ws ? rune_is_space(U.q[i]) : !wordch(U.q[i]))) i++;
	while (i < U.qn && (ws ? !rune_is_space(U.q[i]) : wordch(U.q[i]))) i++;
	return i;
}

bool act_query_key(int key)
{
	if (U.o->no_input || U.hide_input) return false;
	if (key == K_PASTE) return false;
	if (K_MODS(key) & (M_CTRL | M_ALT)) return false;
	int b = K_BASE(key);
	if (b < 32 || b >= 0x110000 || b == 127) return false;
	rune r = (rune)b;
	q_insert(&r, 1);
	return true;
}

void act_paste(const char *s)
{
	if (U.o->no_input || U.hide_input) return;
	size_t n = strlen(s);
	rune *r = xmalloc((n + 1) * sizeof(rune));
	size_t rn = utf8_to_runes(s, n, r);
	size_t k = 0;
	for (size_t i = 0; i < rn; i++) { if (r[i] == '\n' || r[i] == '\r' || r[i] == '\t') r[k++] = ' '; else if (r[i] >= 32) r[k++] = r[i]; }
	if (k) q_insert(r, (int)k);
	free(r);
}

/* ------------------------------------------------------------------ helpers */
static char *trim_nl(char *s)
{
	size_t n = strlen(s);
	while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = 0;
	return s;
}
static char *run_transform(const char *arg)
{
	char *cmd = expand_template(arg, false);
	Buf out;
	buf_init(&out);
	proc_run_capture(cmd, &out);
	free(cmd);
	return trim_nl(buf_take(&out));
}
static void set_str(char **d, const char *v) { free(*d); *d = xstrdup(v); }
static bool visual_up_is_plus(void) { return U.o->layout == LAYOUT_DEFAULT; }

static void accept_now(bool need_match, bool print_query_if_none)
{
	if (need_match && U.nhits == 0 && U.nsel == 0) return;
	if (print_query_if_none && U.nhits == 0 && U.nsel == 0) { U.o->print_query = true; U.exit_code = 0; U.accepted = true; U.running = false; return; }
	U.accepted = true;
	U.exit_code = (U.nhits == 0 && U.nsel == 0) ? 1 : 0;
	U.running = false;
}

static void select_move_to(int dir)         /* dir > 0: next selected in list order */
{
	if (!U.nsel || !U.nhits) return;
	size_t i = U.cur;
	for (size_t k = 0; k < U.nhits; k++) {
		if (dir > 0) i = (i + 1) % U.nhits; else i = (i + U.nhits - 1) % U.nhits;
		if (U.items.v[U.hits[i].idx]->flags & ITEM_SEL) { ui_set_cursor(i); return; }
	}
}

static int page_rows(void) { int h = U.list_rect.h; return h > 1 ? h - 1 : 1; }

static void hist_go(int dir)
{
	if (!U.nhist) return;
	if (U.hist_pos == U.nhist && dir < 0) { free(U.hist_saved); U.hist_saved = ui_query_str(); }
	int np = U.hist_pos + dir;
	if (np < 0) np = 0;
	if (np > U.nhist) np = U.nhist;
	U.hist_pos = np;
	ui_query_set(np == U.nhist ? (U.hist_saved ? U.hist_saved : "") : U.hist[np]);
}

static void unbind_keys(const char *spec, bool on)
{
	char *c = xstrdup(spec), *sv = NULL;
	for (char *t = strtok_r(c, ",", &sv); t; t = strtok_r(NULL, ",", &sv)) {
		int k = key_parse(t);
		if (k < 0) continue;
		for (int i = 0; i < U.nunbound; i++) if (U.unbound[i] == k) { U.unbound[i] = U.unbound[--U.nunbound]; break; }
		if (!on) { U.unbound = xrealloc(U.unbound, (size_t)(U.nunbound + 1) * sizeof(int)); U.unbound[U.nunbound++] = k; }
	}
	free(c);
}

/* ------------------------------------------------------------------ the dispatcher */
static void run_one(const Act *a)
{
	const char *arg = a->arg ? a->arg : "";
	switch (a->id) {
	case A_NONE: case A_IGNORE: break;
	case A_ABORT: U.accepted = false; U.exit_code = 130; U.running = false; break;
	case A_CANCEL: if (U.qn) { U.qc = U.qn; q_delete(0, U.qn); } else { U.exit_code = 130; U.running = false; } break;
	case A_ACCEPT: accept_now(false, false); break;
	case A_ACCEPT_NON_EMPTY: accept_now(true, false); break;
	case A_ACCEPT_OR_PRINT_QUERY: accept_now(false, true); break;
	case A_CLOSE: if (U.pv_cmd && !U.pv_hidden) { pv_toggle(); } else { U.exit_code = 130; U.running = false; } break;
	case A_BELL: term_outs("\007"); term_flush(); break;
	case A_CLEAR_SCREEN: ui_dirty(); break;

	/* cursor */
	case A_UP: ui_move(visual_up_is_plus() ? 1 : -1); break;
	case A_DOWN: ui_move(visual_up_is_plus() ? -1 : 1); break;
	case A_PAGE_UP: ui_move((visual_up_is_plus() ? 1 : -1) * page_rows()); break;
	case A_PAGE_DOWN: ui_move((visual_up_is_plus() ? -1 : 1) * page_rows()); break;
	case A_HALF_PAGE_UP: ui_move((visual_up_is_plus() ? 1 : -1) * (page_rows() / 2 ? page_rows() / 2 : 1)); break;
	case A_HALF_PAGE_DOWN: ui_move((visual_up_is_plus() ? -1 : 1) * (page_rows() / 2 ? page_rows() / 2 : 1)); break;
	case A_FIRST: case A_TOP: ui_set_cursor(0); break;
	case A_LAST: if (U.nhits) ui_set_cursor(U.nhits - 1); break;
	case A_POS: {
		int p = atoi(arg);
		if (U.nhits) { long i = p > 0 ? p - 1 : (long)U.nhits + p; if (i < 0) i = 0; if (i >= (long)U.nhits) i = (long)U.nhits - 1; ui_set_cursor((size_t)i); }
		break;
	}
	case A_NEXT_SELECTED: select_move_to(visual_up_is_plus() ? -1 : 1); break;
	case A_PREV_SELECTED: select_move_to(visual_up_is_plus() ? 1 : -1); break;

	/* selection */
	case A_TOGGLE: { Item *it = ui_current(); if (it && U.o->multi) ui_toggle_sel(it, !(it->flags & ITEM_SEL)); break; }
	case A_SELECT: { Item *it = ui_current(); if (it && U.o->multi) ui_toggle_sel(it, true); break; }
	case A_DESELECT: { Item *it = ui_current(); if (it) ui_toggle_sel(it, false); break; }
	case A_SELECT_ALL: if (U.o->multi) ui_select_all(true); break;
	case A_DESELECT_ALL: case A_CLEAR_SELECTION: ui_clear_sel(); break;
	case A_TOGGLE_ALL: if (U.o->multi) for (size_t i = 0; i < U.nhits; i++) { Item *it = U.items.v[U.hits[i].idx]; ui_toggle_sel(it, !(it->flags & ITEM_SEL)); } break;
	case A_TOGGLE_IN: { Item *it = ui_current(); if (it && U.o->multi) ui_toggle_sel(it, !(it->flags & ITEM_SEL)); ui_move(U.o->layout == LAYOUT_REVERSE ? 1 : -1); break; }
	case A_TOGGLE_OUT: { Item *it = ui_current(); if (it && U.o->multi) ui_toggle_sel(it, !(it->flags & ITEM_SEL)); ui_move(U.o->layout == LAYOUT_REVERSE ? -1 : 1); break; }
	case A_CHANGE_MULTI: U.o->multi = true; U.o->multi_max = atoi(arg); if (!*arg) U.o->multi_max = 0; ui_dirty(); break;

	/* query */
	case A_BACKWARD_CHAR: if (U.qc > 0) U.qc--; break;
	case A_FORWARD_CHAR: if (U.qc < U.qn) U.qc++; break;
	case A_BEGINNING_OF_LINE: U.qc = 0; break;
	case A_END_OF_LINE: U.qc = U.qn; break;
	case A_BACKWARD_WORD: U.qc = word_back(U.qc, false); break;
	case A_FORWARD_WORD: U.qc = word_fwd(U.qc, false); break;
	case A_BACKWARD_DELETE_CHAR: if (U.qc > 0) q_delete_nokill(U.qc - 1, U.qc); break;
	case A_BACKWARD_DELETE_CHAR_EOF: if (U.qn == 0) { U.exit_code = 130; U.running = false; ui_fire(E_BACKWARD_EOF); } else if (U.qc > 0) q_delete_nokill(U.qc - 1, U.qc); break;
	case A_DELETE_CHAR: if (U.qc < U.qn) q_delete_nokill(U.qc, U.qc + 1); break;
	case A_DELETE_CHAR_EOF: if (U.qn == 0) { U.exit_code = 130; U.running = false; } else if (U.qc < U.qn) q_delete_nokill(U.qc, U.qc + 1); break;
	case A_KILL_LINE: q_delete(U.qc, U.qn); break;
	case A_UNIX_LINE_DISCARD: q_delete(0, U.qc); break;
	case A_UNIX_WORD_RUBOUT: q_delete(word_back(U.qc, true), U.qc); break;
	case A_BACKWARD_KILL_WORD: q_delete(word_back(U.qc, false), U.qc); break;
	case A_KILL_WORD: q_delete(U.qc, word_fwd(U.qc, false)); break;
	case A_YANK: if (U.nyank) q_insert(U.yank, U.nyank); break;
	case A_CLEAR_QUERY: U.qc = U.qn; q_delete_nokill(0, U.qn); break;
	case A_PUT: {
		if (*arg) act_paste(arg);
		else if (U.last_key) { int k = key_parse(U.last_key); if (k >= 0) act_query_key(k); }
		break;
	}
	case A_REPLACE_QUERY: { Item *it = ui_current(); if (it) { char *s = xstrndup(it->text, it->tlen); ui_query_set(s); free(s); } break; }
	case A_CHANGE_QUERY: ui_query_set(arg); break;
	case A_TRANSFORM_QUERY: { char *s = run_transform(arg); ui_query_set(s); free(s); break; }
	case A_PREV_HISTORY: hist_go(-1); break;
	case A_NEXT_HISTORY: hist_go(1); break;
	case A_SEARCH: { free(NULL); ui_query_set(arg); break; }
	case A_DISABLE_SEARCH: U.searching_disabled = true; U.o->disabled = true; U.need_search = true; break;
	case A_ENABLE_SEARCH: U.searching_disabled = false; U.o->disabled = false; U.need_search = true; break;
	case A_TOGGLE_SEARCH: U.searching_disabled = !U.searching_disabled; U.o->disabled = U.searching_disabled; U.need_search = true; break;
	case A_TOGGLE_SORT: U.o->sort = !U.o->sort; U.need_search = true; break;
	case A_TOGGLE_TRACK: U.track_on = !U.track_on; if (U.track_on && ui_current()) { free(U.track_key); U.track_key = item_key(ui_current()); } break;
	case A_TRACK_CURRENT: { Item *it = ui_current(); if (it) { free(U.track_key); U.track_key = item_key(it); U.track_have = true; } break; }
	case A_HIDE_INPUT: U.hide_input = true; break;
	case A_SHOW_INPUT: U.hide_input = false; break;
	case A_TOGGLE_INPUT: U.hide_input = !U.hide_input; break;

	/* text areas */
	case A_CHANGE_PROMPT: set_str(&U.prompt, arg); break;
	case A_TRANSFORM_PROMPT: { char *s = run_transform(arg); set_str(&U.prompt, s); free(s); break; }
	case A_CHANGE_HEADER: set_str(&U.header, arg); break;
	case A_TRANSFORM_HEADER: { char *s = run_transform(arg); set_str(&U.header, s); free(s); break; }
	case A_CHANGE_FOOTER: set_str(&U.footer, arg); break;
	case A_TRANSFORM_FOOTER: { char *s = run_transform(arg); set_str(&U.footer, s); free(s); break; }
	case A_CHANGE_BORDER_LABEL: set_str(&U.border_label, arg); break;
	case A_TRANSFORM_BORDER_LABEL: { char *s = run_transform(arg); set_str(&U.border_label, s); free(s); break; }
	case A_CHANGE_PREVIEW_LABEL: set_str(&U.preview_label, arg); break;
	case A_TRANSFORM_PREVIEW_LABEL: { char *s = run_transform(arg); set_str(&U.preview_label, s); free(s); break; }
	case A_HIDE_HEADER: case A_SHOW_HEADER: case A_TOGGLE_HEADER: break;
	case A_CHANGE_NTH: { free(U.o->nth); U.o->nth = NULL; U.o->nnth = *arg ? ranges_parse(arg, &U.o->nth) : 0; if (U.o->nnth < 0) U.o->nnth = 0; U.need_search = true; break; }

	/* preview */
	case A_TOGGLE_PREVIEW: pv_toggle(); break;
	case A_SHOW_PREVIEW: if (U.pv_hidden) pv_toggle(); break;
	case A_HIDE_PREVIEW: if (!U.pv_hidden) pv_toggle(); break;
	case A_PREVIEW_UP: pv_scroll(-1); break;
	case A_PREVIEW_DOWN: pv_scroll(1); break;
	case A_PREVIEW_PAGE_UP: pv_page(-1, false); break;
	case A_PREVIEW_PAGE_DOWN: pv_page(1, false); break;
	case A_PREVIEW_HALF_PAGE_UP: pv_page(-1, true); break;
	case A_PREVIEW_HALF_PAGE_DOWN: pv_page(1, true); break;
	case A_PREVIEW_TOP: U.pv_scroll = 0; break;
	case A_PREVIEW_BOTTOM: pv_scroll(1 << 28); break;
	case A_REFRESH_PREVIEW: pv_schedule(); break;
	case A_TOGGLE_PREVIEW_WRAP: U.pv_wrap = !U.pv_wrap; break;
	case A_CHANGE_PREVIEW: set_str(&U.pv_cmd, arg); pv_schedule(); break;
	case A_PREVIEW: set_str(&U.pv_cmd, arg); if (U.pv_hidden) U.pv_hidden = false; pv_schedule(); break;
	case A_CHANGE_PREVIEW_WINDOW: ui_pw_apply(arg); break;

	/* running things */
	case A_EXECUTE: { char *c = expand_template(arg, false); proc_execute(c, false); free(c); ui_dirty(); break; }
	case A_EXECUTE_SILENT: { char *c = expand_template(arg, false); proc_execute(c, true); free(c); break; }
	case A_BECOME: { char *c = expand_template(arg, false); U.become_cmd = c; U.running = false; break; }
	case A_RELOAD: case A_RELOAD_SYNC: { char *c = expand_template(arg, false); reload_start(c, a->id == A_RELOAD_SYNC); free(c);
		if (a->id == A_RELOAD_SYNC) {                      /* the actions after it wait for the new list */
			while (U.reload.live) { struct pollfd pf = {U.reload.fd, POLLIN, 0}; poll(&pf, 1, 100); reload_read(); }
			ui_run_search();
		}
		break; }
	case A_PRINT: { char *c = expand_template(arg, false); buf_adds(&U.printq, c); buf_addc(&U.printq, U.o->print0 ? 0 : '\n'); free(c); break; }
	case A_TRANSFORM: {
		char *s = run_transform(arg);
		ActLoc l = {NULL, 0};
		acts_from_string(&l, s);
		act_run_list(l.v, l.n);
		acts_free(&l);
		free(s);
		break;
	}
	case A_UNBIND: unbind_keys(arg, false); break;
	case A_REBIND: unbind_keys(arg, true); break;
	case A_JUMP: U.jump_mode = true; break;
	default: break;
	}
}

void act_run_list(const Act *v, int n)
{
	for (int i = 0; i < n && U.running; i++) run_one(&v[i]);
}

void act_run(const ActLoc *a, int key)
{
	(void)key;
	act_run_list(a->v, a->n);
}
