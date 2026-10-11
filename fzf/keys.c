/* keys.c - key names, the --bind grammar ("key:action+action(arg),key2:action"), the action table, default bindings. */
#include "ui.h"

static const struct { const char *name; ActId id; const char *arg; } acts[] = {
	{"abort", A_ABORT, NULL}, {"accept", A_ACCEPT, NULL}, {"accept-non-empty", A_ACCEPT_NON_EMPTY, NULL},
	{"accept-or-print-query", A_ACCEPT_OR_PRINT_QUERY, NULL}, {"backward-char", A_BACKWARD_CHAR, NULL},
	{"backward-delete-char", A_BACKWARD_DELETE_CHAR, NULL}, {"backward-delete-char/eof", A_BACKWARD_DELETE_CHAR_EOF, NULL},
	{"backward-kill-word", A_BACKWARD_KILL_WORD, NULL}, {"backward-word", A_BACKWARD_WORD, NULL},
	{"beginning-of-line", A_BEGINNING_OF_LINE, NULL}, {"become", A_BECOME, "arg"}, {"bell", A_BELL, NULL}, {"cancel", A_CANCEL, NULL},
	{"change-border-label", A_CHANGE_BORDER_LABEL, "arg"}, {"change-footer", A_CHANGE_FOOTER, "arg"},
	{"change-header", A_CHANGE_HEADER, "arg"}, {"change-multi", A_CHANGE_MULTI, ""}, {"change-nth", A_CHANGE_NTH, "arg"},
	{"change-preview", A_CHANGE_PREVIEW, "arg"}, {"change-preview-label", A_CHANGE_PREVIEW_LABEL, "arg"},
	{"change-preview-window", A_CHANGE_PREVIEW_WINDOW, "arg"}, {"change-prompt", A_CHANGE_PROMPT, "arg"},
	{"change-query", A_CHANGE_QUERY, "arg"}, {"clear-query", A_CLEAR_QUERY, NULL}, {"clear-screen", A_CLEAR_SCREEN, NULL},
	{"clear-selection", A_CLEAR_SELECTION, NULL}, {"close", A_CLOSE, NULL}, {"delete-char", A_DELETE_CHAR, NULL},
	{"delete-char/eof", A_DELETE_CHAR_EOF, NULL}, {"deselect", A_DESELECT, NULL}, {"deselect-all", A_DESELECT_ALL, NULL},
	{"disable-search", A_DISABLE_SEARCH, NULL}, {"down", A_DOWN, NULL}, {"enable-search", A_ENABLE_SEARCH, NULL},
	{"end-of-line", A_END_OF_LINE, NULL}, {"execute", A_EXECUTE, "arg"}, {"execute-silent", A_EXECUTE_SILENT, "arg"},
	{"first", A_FIRST, NULL}, {"forward-char", A_FORWARD_CHAR, NULL}, {"forward-word", A_FORWARD_WORD, NULL},
	{"half-page-down", A_HALF_PAGE_DOWN, NULL}, {"half-page-up", A_HALF_PAGE_UP, NULL}, {"hide-header", A_HIDE_HEADER, NULL},
	{"hide-input", A_HIDE_INPUT, NULL}, {"hide-preview", A_HIDE_PREVIEW, NULL}, {"ignore", A_IGNORE, NULL}, {"jump", A_JUMP, NULL},
	{"kill-line", A_KILL_LINE, NULL}, {"kill-word", A_KILL_WORD, NULL}, {"last", A_LAST, NULL}, {"next-history", A_NEXT_HISTORY, NULL},
	{"next-selected", A_NEXT_SELECTED, NULL}, {"page-down", A_PAGE_DOWN, NULL}, {"page-up", A_PAGE_UP, NULL}, {"pos", A_POS, "arg"},
	{"prev-history", A_PREV_HISTORY, NULL}, {"previous-history", A_PREV_HISTORY, NULL}, {"prev-selected", A_PREV_SELECTED, NULL},
	{"preview", A_PREVIEW, "arg"}, {"preview-bottom", A_PREVIEW_BOTTOM, NULL}, {"preview-down", A_PREVIEW_DOWN, NULL},
	{"preview-half-page-down", A_PREVIEW_HALF_PAGE_DOWN, NULL}, {"preview-half-page-up", A_PREVIEW_HALF_PAGE_UP, NULL},
	{"preview-page-down", A_PREVIEW_PAGE_DOWN, NULL}, {"preview-page-up", A_PREVIEW_PAGE_UP, NULL}, {"preview-top", A_PREVIEW_TOP, NULL},
	{"preview-up", A_PREVIEW_UP, NULL}, {"print", A_PRINT, "arg"}, {"put", A_PUT, ""}, {"rebind", A_REBIND, "arg"},
	{"refresh-preview", A_REFRESH_PREVIEW, NULL}, {"reload", A_RELOAD, "arg"}, {"reload-sync", A_RELOAD_SYNC, "arg"},
	{"replace-query", A_REPLACE_QUERY, NULL}, {"search", A_SEARCH, "arg"}, {"select", A_SELECT, NULL}, {"select-all", A_SELECT_ALL, NULL},
	{"show-header", A_SHOW_HEADER, NULL}, {"show-preview", A_SHOW_PREVIEW, NULL}, {"show-input", A_SHOW_INPUT, NULL}, {"toggle", A_TOGGLE, NULL}, {"toggle-all", A_TOGGLE_ALL, NULL},
	{"toggle-header", A_TOGGLE_HEADER, NULL}, {"toggle-in", A_TOGGLE_IN, NULL}, {"toggle-input", A_TOGGLE_INPUT, NULL},
	{"toggle-out", A_TOGGLE_OUT, NULL}, {"toggle-preview", A_TOGGLE_PREVIEW, NULL}, {"toggle-preview-wrap", A_TOGGLE_PREVIEW_WRAP, NULL},
	{"toggle-search", A_TOGGLE_SEARCH, NULL}, {"toggle-sort", A_TOGGLE_SORT, NULL}, {"toggle-track", A_TOGGLE_TRACK, NULL},
	{"toggle-wrap", A_TOGGLE_WRAP, NULL}, {"top", A_TOP, NULL}, {"track-current", A_TRACK_CURRENT, NULL}, {"transform", A_TRANSFORM, "arg"},
	{"transform-border-label", A_TRANSFORM_BORDER_LABEL, "arg"}, {"transform-footer", A_TRANSFORM_FOOTER, "arg"},
	{"transform-header", A_TRANSFORM_HEADER, "arg"}, {"transform-preview-label", A_TRANSFORM_PREVIEW_LABEL, "arg"},
	{"transform-prompt", A_TRANSFORM_PROMPT, "arg"}, {"transform-query", A_TRANSFORM_QUERY, "arg"}, {"unbind", A_UNBIND, "arg"},
	{"unix-line-discard", A_UNIX_LINE_DISCARD, NULL}, {"unix-word-rubout", A_UNIX_WORD_RUBOUT, NULL}, {"up", A_UP, NULL}, {"yank", A_YANK, NULL},
};

ActId action_id(const char *name)
{
	for (size_t i = 0; i < sizeof acts / sizeof acts[0]; i++)
		if (!strcmp(acts[i].name, name)) return acts[i].id;
	return A_NONE;
}
const char *action_takes_arg(ActId id)
{
	for (size_t i = 0; i < sizeof acts / sizeof acts[0]; i++)
		if (acts[i].id == id) return acts[i].arg;
	return NULL;
}

/* ---- keys ---- */
static int named_key(const char *n)
{
	static const struct { const char *n; int k; } t[] = {
		{"enter", K_ENTER}, {"return", K_ENTER}, {"space", ' '}, {"tab", K_TAB}, {"btab", K_BTAB}, {"shift-tab", K_BTAB}, {"esc", K_ESC},
		{"escape", K_ESC}, {"bspace", K_BSPACE}, {"bs", K_BSPACE}, {"backspace", K_BSPACE}, {"del", K_DEL}, {"delete", K_DEL},
		{"ins", K_INS}, {"insert", K_INS}, {"up", K_UP}, {"down", K_DOWN}, {"left", K_LEFT}, {"right", K_RIGHT}, {"home", K_HOME},
		{"end", K_END}, {"pgup", K_PGUP}, {"page-up", K_PGUP}, {"pgdn", K_PGDN}, {"page-down", K_PGDN}, {"click", K_CLICK},
		{"double-click", K_DCLICK}, {"right-click", K_RCLICK}, {"scroll-up", K_SCROLL_UP}, {"scroll-down", K_SCROLL_DOWN},
		{"focus", K_FOCUS_IN}, {"start", E_START}, {"load", E_LOAD}, {"resize", E_RESIZE}, {"one", E_ONE}, {"zero", E_ZERO},
		{"result", E_RESULT}, {"change", E_CHANGE}, {"backward-eof", E_BACKWARD_EOF}, {"jump", E_JUMP}, {"jump-cancel", E_JUMP_CANCEL},
		{"multi", E_MULTI}};
	for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
		if (!strcmp(t[i].n, n)) return t[i].k;
	if ((n[0] == 'f' || n[0] == 'F') && n[1] >= '1' && n[1] <= '9') {
		int v = atoi(n + 1);
		if (v >= 1 && v <= 12 && (int)strlen(n + 1) == (v < 10 ? 1 : 2)) return K_F1 + v - 1;
	}
	return -1;
}

int key_parse(const char *name)
{
	int mods = 0;
	const char *p = name;
	for (;;) {                                   /* leading ctrl- alt- shift- prefixes (a lone "-" is the key) */
		if (!strncmp(p, "ctrl-", 5) && p[5]) { mods |= M_CTRL; p += 5; }
		else if (!strncmp(p, "alt-", 4) && p[4]) { mods |= M_ALT; p += 4; }
		else if (!strncmp(p, "shift-", 6) && p[6] && strcmp(p, "shift-tab")) { mods |= M_SHIFT; p += 6; }
		else break;
	}
	int base = named_key(p);
	if (base < 0) {
		rune r;
		int len = u8dec(p, strlen(p), &r);
		if (len > 0 && p[len] == 0) base = (int)r; else return -1;
	}
	if (base >= E_START && base < E_LAST) return mods ? -1 : base;
	if (mods & M_CTRL) {
		if (base >= 'A' && base <= 'Z') base += 32;
		if (base == '/') base = '_';                   /* ctrl-/ sends 0x1f = ctrl-_ */
		if (base == '[' && !(mods & ~M_CTRL)) return K_ESC;
		if ((base >= 'a' && base <= 'z') || base == '_' || base == '\\' || base == ']' || base == '^' || base == ' ' || base == '@') {
			if (base == '@') base = ' ';
			if (!(mods & ~M_CTRL)) {                            /* plain ctrl-x; a few are the same byte as named keys */
				if (base == 'i') return K_TAB;
				if (base == 'm') return K_ENTER;
				return base | M_CTRL;
			}
			return base | mods;
		}
	}
	if ((mods & M_ALT) && base >= 'A' && base <= 'Z' && !(mods & M_SHIFT)) return base | mods;
	return base | mods;
}

/* ---- --bind grammar ---- */
static char closer(char o)
{
	switch (o) { case '(': return ')'; case '[': return ']'; case '{': return '}'; case '<': return '>'; default: return o; }
}
static bool is_opener(char c) { return c && strchr("([{<~!@#$%^&*;/|", c) != NULL; }

static void act_push(ActLoc *a, ActId id, char *arg)
{
	a->v = xrealloc(a->v, (size_t)(a->n + 1) * sizeof(Act));
	a->v[a->n].id = id; a->v[a->n].arg = arg; a->n++;
}
void acts_free(ActLoc *a) { for (int i = 0; i < a->n; i++) free(a->v[i].arg); free(a->v); a->v = NULL; a->n = 0; }

/* parses "up+execute(ls)+accept" from s (stopping at a top-level ',' when stop_comma); returns the index after it */
static size_t parse_acts(const char *s, size_t i, size_t n, bool stop_comma, ActLoc *out)
{
	while (i < n) {
		while (i < n && s[i] == ' ') i++;
		size_t b = i;
		while (i < n && (isalnum((unsigned char)s[i]) || s[i] == '-' || (s[i] == '/' && s[i + 1] == 'e'))) i++;
		if (i == b) break;
		char *name = xstrndup(s + b, i - b);
		ActId id = action_id(name);
		if (id == A_NONE) die("unknown action: %s", name);
		char *arg = NULL;
		if (i < n && s[i] == ':' && action_takes_arg(id)) {            /* colon form: the rest of the string is the argument */
			arg = xstrndup(s + i + 1, n - i - 1);
			i = n;
		} else if (i < n && is_opener(s[i]) && action_takes_arg(id)) {
			char o = s[i], c = closer(o);
			size_t j = i + 1;
			int depth = 1;
			while (j < n) {
				if (o != c && s[j] == o) depth++;
				else if (s[j] == c) { if (--depth == 0) break; }
				j++;
			}
			if (j >= n) die("unterminated action argument: %s", name);
			arg = xstrndup(s + i + 1, j - i - 1);
			i = j + 1;
		} else if (action_takes_arg(id) && strcmp(action_takes_arg(id), "")) die("action requires an argument: %s", name);
		act_push(out, id, arg);
		free(name);
		while (i < n && s[i] == ' ') i++;
		if (i < n && s[i] == '+') { i++; continue; }
		break;
	}
	(void)stop_comma;
	return i;
}

void acts_from_string(ActLoc *out, const char *s)
{
	out->v = NULL; out->n = 0;
	size_t n = strlen(s), i = parse_acts(s, 0, n, false, out);
	if (i < n) die("unexpected characters in action list: %s", s + i);
}

static void bind_add(Binding **v, int *n, int key, ActLoc *a, char *label)
{
	for (int i = 0; i < *n; i++)
		if ((*v)[i].key == key) { acts_free(&(*v)[i].acts); (*v)[i].acts = *a; if (label || !(*v)[i].label) { free((*v)[i].label); (*v)[i].label = label; } return; }
	*v = xrealloc(*v, (size_t)(*n + 1) * sizeof(Binding));
	(*v)[*n].key = key; (*v)[*n].acts = *a; (*v)[*n].label = label; (*n)++;
}

void bind_parse(Binding **v, int *n, const char *spec)
{
	size_t len = strlen(spec), i = 0;
	while (i < len) {
		while (i < len && (spec[i] == ' ' || spec[i] == ',')) i++;
		if (i >= len) break;
		size_t k = i;
		if (strchr(",:+", spec[i]) && i + 1 < len && spec[i + 1] == ':') k = i + 1;   /* a punctuation key: ",:abort" */
		else while (k < len && spec[k] != ':') k++;
		if (k >= len) die("bind action not specified: %s", spec + i);
		char *kn = xstrndup(spec + i, k - i);
		int key = key_parse(kn);
		if (key < 0) die("unknown key: %s", kn);
		free(kn);
		ActLoc a = {NULL, 0};
		size_t j = parse_acts(spec, k + 1, len, true, &a);
		if (a.n == 0) die("bind action not specified: %s", spec + i);
		char *label = NULL;
		size_t q = j;
		while (q < len && spec[q] == ' ') q++;
		if (q < len && spec[q] == '#') {                    /* "key:action # label" : the label shows in the --hints bar */
			size_t e = q + 1;
			while (e < len && spec[e] != ',') e++;
			size_t b = q + 1;
			while (b < e && spec[b] == ' ') b++;
			size_t t = e;
			while (t > b && spec[t - 1] == ' ') t--;
			if (t > b) label = xstrndup(spec + b, t - b);
			j = e;
		}
		bind_add(v, n, key, &a, label);
		i = j;
		while (i < len && spec[i] == ' ') i++;
		if (i < len && spec[i] != ',') die("unexpected characters after action: %s", spec + i);
	}
}

ActLoc *bind_find(Binding *v, int n, int key)
{
	for (int i = 0; i < n; i++) if (v[i].key == key) return &v[i].acts;
	return NULL;
}

static void def(Binding **v, int *n, int key, const char *acts_s)
{
	ActLoc a;
	acts_from_string(&a, acts_s);
	bind_add(v, n, key, &a, NULL);
}
void bind_defaults(Binding **v, int *n)
{
	def(v, n, M_CTRL | 'a', "beginning-of-line"); def(v, n, M_CTRL | 'b', "backward-char"); def(v, n, M_CTRL | 'c', "abort");
	def(v, n, M_CTRL | 'd', "delete-char/eof"); def(v, n, M_CTRL | 'e', "end-of-line"); def(v, n, M_CTRL | 'f', "forward-char");
	def(v, n, M_CTRL | 'g', "abort"); def(v, n, M_CTRL | 'h', "backward-delete-char"); def(v, n, M_CTRL | 'j', "down");
	def(v, n, M_CTRL | 'k', "up"); def(v, n, M_CTRL | 'l', "clear-screen"); def(v, n, K_ENTER, "accept"); def(v, n, M_CTRL | 'n', "down");
	def(v, n, M_CTRL | 'p', "up"); def(v, n, M_CTRL | 'q', "abort"); def(v, n, M_CTRL | 'u', "unix-line-discard");
	def(v, n, M_CTRL | 'w', "unix-word-rubout"); def(v, n, M_CTRL | 'y', "yank"); def(v, n, K_ESC, "abort");
	def(v, n, M_ALT | 'b', "backward-word"); def(v, n, M_ALT | 'f', "forward-word"); def(v, n, M_ALT | 'd', "kill-word");
	def(v, n, M_ALT | K_BSPACE, "backward-kill-word"); def(v, n, K_BSPACE, "backward-delete-char"); def(v, n, K_DEL, "delete-char");
	def(v, n, K_UP, "up"); def(v, n, K_DOWN, "down"); def(v, n, K_LEFT, "backward-char"); def(v, n, K_RIGHT, "forward-char");
	def(v, n, K_HOME, "beginning-of-line"); def(v, n, K_END, "end-of-line"); def(v, n, K_PGUP, "page-up"); def(v, n, K_PGDN, "page-down");
	def(v, n, K_SCROLL_UP, "up"); def(v, n, K_SCROLL_DOWN, "down");
	def(v, n, M_SHIFT | K_UP, "preview-up"); def(v, n, M_SHIFT | K_DOWN, "preview-down");
	def(v, n, K_DCLICK, "accept");
}
