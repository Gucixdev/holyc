/* opts.c - command line and FZF_DEFAULT_OPTS. One table describes every option: its long name, an optional short
 * form, whether it takes a value, and the setter. --no-NAME / +X undo a boolean option. */
#include "fzf.h"
#include "opts.h"

static void bad(const char *fmt, const char *a) { char m[256]; snprintf(m, sizeof m, fmt, a); die("%s", m); }

Opts *opts_new(void)
{
	Opts *o = calloc(1, sizeof *o);
	if (!o) die("out of memory");
	o->extended = o->fuzzy = o->sort = o->normalize = o->mouse = true;
	o->case_mode = CASE_SMART;
	strcpy(o->scheme, "default");
	o->tabstop = 8;
	o->prompt = xstrdup("> ");
	o->pointer = xstrdup("▌");
	o->marker = xstrdup("┃");
	o->ellipsis = xstrdup("..");
	o->query = xstrdup("");
	o->scroll_off = 3;
	o->hscroll = true;
	o->pw_pos = PW_RIGHT; o->pw_size = 50; o->pw_size_pct = true; o->pw_border = true; o->pw_border_set = true;
	o->border_label_pos = 0;
	o->history_size = 1000;
	o->min_height = 10;
	o->with_shell = xstrdup("");
	o->forward = true;
	{ const char *t = getenv("TERM"); o->ascii = t && !strncmp(t, "linux", 5); }   /* the kernel console font lacks most drawing glyphs */
	o->footer_border = -1;                            /* -1: automatic (a rule under the footer) */
	return o;
}

typedef void (*Setter)(Opts *, const char *, bool);   /* bool: the option was negated (--no-x / +x) */
typedef struct { const char *name; char sh; int arg; Setter fn; } Opt;      /* arg: 0 none, 1 required, 2 optional (=VALUE only) */

static void set_str(char **dst, const char *v) { free(*dst); *dst = xstrdup(v); }

#define S(name) static void name(Opts *o, const char *v, bool neg)
S(o_extended) { (void)v; o->extended = !neg; }
S(o_exact) { (void)v; o->fuzzy = neg; }
S(o_icase) { (void)v; o->case_mode = neg ? CASE_RESPECT : CASE_IGNORE; }
S(o_smart) { (void)v; (void)neg; o->case_mode = CASE_SMART; }
S(o_literal) { (void)v; o->normalize = neg; }
S(o_nth) { free(o->nth); o->nth = NULL; o->nnth = ranges_parse(v, &o->nth); if (o->nnth < 0) bad("invalid field index expression: %s", v); }
S(o_withnth) {
	(void)neg;
	if (strchr(v, '{')) { set_str(&o->with_tmpl, v); return; }
	free(o->with_nth); o->with_nth = NULL;
	o->nwith = ranges_parse(v, &o->with_nth);
	if (o->nwith < 0) bad("invalid field index expression: %s", v);
}
S(o_delim) { (void)neg; delim_set(&o->delim, v); }
S(o_tac) { (void)v; o->tac = !neg; }
S(o_nosort) { (void)v; o->sort = !neg; }
S(o_query) { (void)neg; set_str(&o->query, v); }
S(o_select1) { (void)v; o->select1 = !neg; }
S(o_exit0) { (void)v; o->exit0 = !neg; }
S(o_filter) { (void)neg; set_str(&o->filter, v); o->has_filter = true; }
S(o_printq) { (void)v; o->print_query = !neg; }
S(o_expect) { (void)neg; set_str(&o->expect, v); }
S(o_read0) { (void)v; o->read0 = !neg; }
S(o_print0) { (void)v; o->print0 = !neg; }
S(o_ansi) { (void)v; o->ansi = !neg; }
S(o_sync) { (void)v; o->sync_ = !neg; }
S(o_tabstop) { (void)neg; o->tabstop = atoi(v) > 0 ? atoi(v) : 8; }
S(o_scheme) { (void)neg; if (strcmp(v, "default") && strcmp(v, "path") && strcmp(v, "history")) bad("invalid scoring scheme: %s (expected: default|path|history)", v); snprintf(o->scheme, sizeof o->scheme, "%s", v); }
S(o_algo) { (void)neg; if (!strcmp(v, "v1")) o->algo_v1 = true; else if (!strcmp(v, "v2")) o->algo_v1 = false; else bad("invalid algorithm (expected: v1 or v2): %s", v); }
S(o_tiebreak) {
	(void)neg;
	o->ncrit = 0;
	o->crit[o->ncrit++] = CR_SCORE;
	char *c = xstrdup(v), *save = NULL;
	for (char *t = strtok_r(c, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
		Crit k;
		if (!strcmp(t, "index")) continue;
		else if (!strcmp(t, "length")) k = CR_LENGTH;
		else if (!strcmp(t, "begin")) k = CR_BEGIN;
		else if (!strcmp(t, "end")) k = CR_END;
		else if (!strcmp(t, "chunk")) k = CR_CHUNK;
		else if (!strcmp(t, "pathname")) k = CR_PATHNAME;
		else bad("invalid sort criterion: %s", t);
		if (o->ncrit >= 4) die("at most 3 tiebreaks are allowed: %s", v);
		o->crit[o->ncrit++] = k;
	}
	free(c);
	o->crit_pos = true;       /* an explicit --tiebreak beats the scheme's criteria */
}
S(o_disabled) { (void)v; o->disabled = !neg; }
S(o_track) { (void)v; o->track = !neg; }
S(o_idnth) { (void)neg; o->nid = ranges_parse(v, &o->id_nth); if (o->nid < 0) bad("invalid field index expression: %s", v); }
S(o_multi) { o->multi = !neg; o->multi_max = (v && *v) ? atoi(v) : 0; }
S(o_mouse) { (void)v; o->mouse = !neg; }
S(o_cycle) { (void)v; o->cycle = !neg; }
S(o_keepright) { (void)v; o->keep_right = !neg; }
S(o_scrolloff) { (void)neg; o->scroll_off = atoi(v); }
S(o_hscroll) { (void)v; o->hscroll = !neg; }
S(o_layout) {
	(void)neg;
	if (!strcmp(v, "default")) o->layout = LAYOUT_DEFAULT;
	else if (!strcmp(v, "reverse")) o->layout = LAYOUT_REVERSE;
	else if (!strcmp(v, "reverse-list")) o->layout = LAYOUT_REVERSE_LIST;
	else bad("invalid layout (expected: default / reverse / reverse-list): %s", v);
}
S(o_reverse) { (void)v; o->layout = neg ? LAYOUT_DEFAULT : LAYOUT_REVERSE; }
S(o_height) { (void)neg; set_str(&o->height_s, v); }
S(o_minheight) { (void)neg; o->min_height = atoi(v); }
static int border_style(const char *v)
{
	if (!v || !*v || !strcmp(v, "rounded")) return BORDER_ROUNDED;
	if (!strcmp(v, "sharp")) return BORDER_SHARP;
	if (!strcmp(v, "bold")) return BORDER_BOLD;
	if (!strcmp(v, "double")) return BORDER_DOUBLE;
	if (!strcmp(v, "horizontal")) return BORDER_HORIZONTAL;
	if (!strcmp(v, "vertical")) return BORDER_VERTICAL;
	if (!strcmp(v, "top")) return BORDER_TOP;
	if (!strcmp(v, "bottom")) return BORDER_BOTTOM;
	if (!strcmp(v, "left")) return BORDER_LEFT;
	if (!strcmp(v, "right")) return BORDER_RIGHT;
	if (!strcmp(v, "none")) return BORDER_NONE;
	bad("invalid border style: %s", v);
	return 0;
}
S(o_border) { o->border = neg ? BORDER_NONE : border_style(v); }
S(o_borderlabel) { (void)neg; set_str(&o->border_label, v); }
S(o_borderlabelpos) { (void)neg; o->border_label_pos = atoi(v); }
S(o_listborder) { o->list_border = neg ? BORDER_NONE : border_style(v); }
S(o_inputborder) { o->input_border = neg ? BORDER_NONE : border_style(v); }
S(o_headerborder) { o->header_border = neg ? BORDER_NONE : border_style(v); }
S(o_footerborder) { o->footer_border = neg ? BORDER_NONE : border_style(v); }
static void parse_edges(const char *v, int *e, int def)
{
	int vals[4] = {def, def, def, def}, n = 0;
	char *c = xstrdup(v), *save = NULL;
	for (char *t = strtok_r(c, ",", &save); t && n < 4; t = strtok_r(NULL, ",", &save)) {
		int x = atoi(t);
		if (strchr(t, '%')) x = -x - 1;                 /* percentage encoded as -(pct+1) */
		vals[n++] = x;
	}
	free(c);
	if (n == 1) vals[1] = vals[2] = vals[3] = vals[0];
	else if (n == 2) { vals[2] = vals[0]; vals[3] = vals[1]; }
	else if (n == 3) vals[3] = vals[1];
	memcpy(e, vals, sizeof vals);
}
S(o_margin) { (void)neg; parse_edges(v, o->margin, 0); }
S(o_padding) { (void)neg; parse_edges(v, o->padding, 0); }
S(o_info) {
	if (neg) { o->info = INFO_HIDDEN; return; }
	if (!strcmp(v, "default")) o->info = INFO_DEFAULT;
	else if (!strcmp(v, "inline")) o->info = INFO_INLINE;
	else if (!strcmp(v, "inline-right")) o->info = INFO_INLINE_RIGHT;
	else if (!strcmp(v, "right")) o->info = INFO_RIGHT;
	else if (!strcmp(v, "hidden")) o->info = INFO_HIDDEN;
	else bad("invalid info style: %s", v);
}
S(o_inlineinfo) { (void)v; o->info = neg ? INFO_DEFAULT : INFO_INLINE; }
S(o_prompt) { (void)neg; set_str(&o->prompt, v); }
S(o_pointer) { (void)neg; set_str(&o->pointer, v); }
S(o_marker) { (void)neg; set_str(&o->marker, v); }
S(o_ellipsis) { (void)neg; set_str(&o->ellipsis, v); }
S(o_header) { (void)neg; set_str(&o->header, v); }
S(o_headerlines) { (void)neg; o->header_lines = atoi(v); }
S(o_headerfirst) { (void)v; o->header_first = !neg; }
S(o_footer) { (void)neg; set_str(&o->footer, v); }
S(o_noinput) { (void)v; o->no_input = !neg; }
S(o_color) {
	if (neg) { o->no_color = true; return; }
	if (o->color_spec) { Buf b; buf_init(&b); buf_adds(&b, o->color_spec); buf_addc(&b, ','); buf_adds(&b, v); free(o->color_spec); o->color_spec = buf_take(&b); }
	else o->color_spec = xstrdup(v);
}
S(o_nocolor) { (void)v; o->no_color = !neg; }
S(o_highlight) { (void)v; o->highlight_line = !neg; }
S(o_wrap) { (void)v; o->wrap = !neg; }
S(o_gap) { (void)v; o->gap = !neg; }
S(o_history) { (void)neg; set_str(&o->history_file, v); }
S(o_historysize) { (void)neg; o->history_size = atoi(v); }
S(o_preview) { (void)neg; if (neg) return; set_str(&o->preview_cmd, v); }
S(o_nopreview) { (void)v; (void)neg; free(o->preview_cmd); o->preview_cmd = NULL; }
S(o_previewwindow) { (void)neg; set_str(&o->preview_window_spec, v); }
S(o_previewlabel) { (void)neg; set_str(&o->preview_label, v); }
S(o_bind) { (void)neg; o->bind_raw = xrealloc(o->bind_raw, (size_t)(o->nbind_raw + 1) * sizeof(char *)); o->bind_raw[o->nbind_raw++] = xstrdup(v); }
S(o_listen) { (void)neg; set_str(&o->listen_addr, (v && *v) ? v : "localhost:0"); }
S(o_withshell) { (void)neg; set_str(&o->with_shell, v); }
S(o_unicode) { (void)v; o->ascii = neg; }
S(o_hints) { o->hints = neg ? 0 : (v && *v ? atoi(v) : 2); }
S(o_ignore) { (void)v; (void)neg; }

static void o_help(Opts *o, const char *v, bool neg)
{
	(void)o; (void)v; (void)neg;
	puts("usage: fzf [options]\n\nSee README.md in the source directory for the list of supported options.");
	exit(0);
}
static void o_version(Opts *o, const char *v, bool neg)
{
	(void)o; (void)v; (void)neg;
	puts("0.74.4-c (fzf-compatible C implementation)");
	exit(0);
}

static const Opt table[] = {
	{"extended", 'x', 0, o_extended}, {"exact", 'e', 0, o_exact}, {"ignore-case", 'i', 0, o_icase}, {"smart-case", 0, 0, o_smart},
	{"literal", 0, 0, o_literal}, {"scheme", 0, 1, o_scheme}, {"algo", 0, 1, o_algo}, {"nth", 'n', 1, o_nth}, {"with-nth", 0, 1, o_withnth},
	{"accept-nth", 0, 1, o_ignore}, {"delimiter", 'd', 1, o_delim}, {"tac", 0, 0, o_tac}, {"sort", 's', 0, o_nosort}, {"tiebreak", 0, 1, o_tiebreak},
	{"disabled", 0, 0, o_disabled}, {"track", 0, 0, o_track}, {"id-nth", 0, 1, o_idnth},
	{"query", 'q', 1, o_query}, {"select-1", '1', 0, o_select1}, {"exit-0", '0', 0, o_exit0}, {"filter", 'f', 1, o_filter},
	{"print-query", 0, 0, o_printq}, {"expect", 0, 1, o_expect}, {"read0", 0, 0, o_read0}, {"print0", 0, 0, o_print0},
	{"ansi", 0, 0, o_ansi}, {"sync", 0, 0, o_sync}, {"tabstop", 0, 1, o_tabstop},
	{"multi", 'm', 2, o_multi}, {"mouse", 0, 0, o_mouse}, {"cycle", 0, 0, o_cycle}, {"keep-right", 0, 0, o_keepright},
	{"scroll-off", 0, 1, o_scrolloff}, {"hscroll", 0, 0, o_hscroll}, {"hscroll-off", 0, 1, o_ignore}, {"filepath-word", 0, 0, o_ignore},
	{"layout", 0, 1, o_layout}, {"reverse", 0, 0, o_reverse}, {"height", 0, 1, o_height}, {"min-height", 0, 1, o_minheight},
	{"border", 0, 2, o_border}, {"border-label", 0, 1, o_borderlabel}, {"border-label-pos", 0, 1, o_borderlabelpos},
	{"list-border", 0, 2, o_listborder}, {"input-border", 0, 2, o_inputborder}, {"header-border", 0, 2, o_headerborder}, {"footer-border", 0, 2, o_footerborder},
	{"margin", 0, 1, o_margin}, {"padding", 0, 1, o_padding}, {"info", 0, 1, o_info}, {"inline-info", 0, 0, o_inlineinfo},
	{"prompt", 0, 1, o_prompt}, {"pointer", 0, 1, o_pointer}, {"marker", 0, 1, o_marker}, {"ellipsis", 0, 1, o_ellipsis},
	{"header", 0, 1, o_header}, {"header-lines", 0, 1, o_headerlines}, {"header-first", 0, 0, o_headerfirst}, {"footer", 0, 1, o_footer},
	{"no-input", 0, 0, o_noinput}, {"color", 0, 1, o_color}, {"black", 0, 0, o_nocolor}, {"highlight-line", 0, 0, o_highlight},
	{"wrap", 0, 0, o_wrap}, {"gap", 0, 2, o_gap}, {"history", 0, 1, o_history}, {"history-size", 0, 1, o_historysize},
	{"preview", 0, 1, o_preview}, {"preview-window", 0, 1, o_previewwindow}, {"preview-label", 0, 1, o_previewlabel},
	{"bind", 0, 1, o_bind}, {"hints", 0, 2, o_hints}, {"unicode", 0, 0, o_unicode}, {"listen", 0, 2, o_listen}, {"with-shell", 0, 1, o_withshell},
	{"bold", 0, 0, o_ignore}, {"scrollbar", 0, 2, o_ignore}, {"separator", 0, 1, o_ignore}, {"ghost", 0, 1, o_ignore}, {"style", 0, 1, o_ignore},
	{"tmux", 0, 2, o_ignore}, {"walker", 0, 1, o_ignore}, {"walker-root", 0, 1, o_ignore}, {"walker-skip", 0, 1, o_ignore},
	{"pointer-size", 0, 1, o_ignore}, {"gutter", 0, 1, o_ignore}, {"raw", 0, 0, o_ignore}, {"jump-labels", 0, 1, o_ignore},
	{"help", 'h', 0, o_help}, {"version", 0, 0, o_version}, {"no-preview", 0, 0, o_nopreview},
	{"bash", 0, 0, o_ignore}, {"zsh", 0, 0, o_ignore}, {"fish", 0, 0, o_ignore},
};

static const Opt *find_long(const char *name, size_t n)
{
	for (size_t i = 0; i < sizeof table / sizeof table[0]; i++)
		if (strlen(table[i].name) == n && !strncmp(table[i].name, name, n)) return &table[i];
	return NULL;
}

static void one(Opts *o, const char *arg, const char *next, int *used)
{
	*used = 1;
	if (arg[0] == '-' && arg[1] == '-') {
		const char *name = arg + 2, *eq = strchr(name, '=');
		size_t n = eq ? (size_t)(eq - name) : strlen(name);
		bool neg = false;
		const Opt *op = find_long(name, n);
		if (!op && n > 3 && !strncmp(name, "no-", 3)) { op = find_long(name + 3, n - 3); neg = op != NULL; }
		if (!op) bad("unknown option: %s", arg);
		const char *val = NULL;
		if (op->arg == 1) {
			if (eq) val = eq + 1;
			else if (next) { val = next; *used = 2; }
			else bad("option requires an argument: %s", arg);
		} else if (op->arg == 2) val = eq ? eq + 1 : "";
		else if (eq) bad("option does not take a value: %s", arg);
		op->fn(o, val, neg);
		return;
	}
	if (arg[0] == '-' || arg[0] == '+') {
		bool neg = arg[0] == '+';
		char sh = arg[1];
		for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
			if (table[i].sh != sh) continue;
			const Opt *op = &table[i];
			const char *val = NULL;
			if (op->arg == 1) {
				if (arg[2]) val = arg + 2;
				else if (next) { val = next; *used = 2; }
				else bad("option requires an argument: %s", arg);
			} else if (arg[2] && !(op->arg == 2)) bad("unknown option: %s", arg);
			else if (op->arg == 2) val = arg[2] ? arg + 2 : "";
			op->fn(o, val, neg);
			return;
		}
	}
	bad("unknown option: %s", arg);
}

static void split_words(const char *s, char ***out, int *n)
{
	int cap = 8;
	char **v = xmalloc((size_t)cap * sizeof(char *));
	*n = 0;
	Buf b;
	buf_init(&b);
	bool in = false;
	char quote = 0;
	for (const char *p = s; ; p++) {
		if (*p == 0 || (!quote && (*p == ' ' || *p == '\t' || *p == '\n'))) {
			if (in) { if (*n == cap) { cap *= 2; v = xrealloc(v, (size_t)cap * sizeof(char *)); } v[(*n)++] = buf_take(&b); in = false; }
			if (!*p) break;
			continue;
		}
		in = true;
		if (quote) { if (*p == quote) quote = 0; else if (quote == '"' && *p == '\\' && p[1]) buf_addc(&b, *++p); else buf_addc(&b, *p); }
		else if (*p == '\'' || *p == '"') quote = *p;
		else if (*p == '\\' && p[1]) buf_addc(&b, *++p);
		else buf_addc(&b, *p);
	}
	buf_free(&b);
	*out = v;
}

void opts_parse(Opts *o, int argc, char **argv)
{
	const char *def = getenv("FZF_DEFAULT_OPTS");
	if (def && *def) {
		char **w;
		int n;
		split_words(def, &w, &n);
		for (int i = 0; i < n;) { int used; one(o, w[i], i + 1 < n ? w[i + 1] : NULL, &used); i += used; }
	}
	for (int i = 1; i < argc;) { int used; one(o, argv[i], i + 1 < argc ? argv[i + 1] : NULL, &used); i += used; }
}

void opts_finish(Opts *o)
{
	if (!o->crit_pos) {                                   /* criteria come from the scheme unless --tiebreak was given */
		o->ncrit = 0;
		o->crit[o->ncrit++] = CR_SCORE;
		if (!strcmp(o->scheme, "path")) { o->crit[o->ncrit++] = CR_PATHNAME; o->crit[o->ncrit++] = CR_LENGTH; }
		else if (!strcmp(o->scheme, "default")) o->crit[o->ncrit++] = CR_LENGTH;
	}
	o->forward = true;
	for (int i = o->ncrit - 1; i > 0; i--) {
		if (o->crit[i] == CR_END || o->crit[i] == CR_PATHNAME) o->forward = false;
		else if (o->crit[i] == CR_BEGIN) o->forward = true;
	}
	if (!algo_init(o->scheme)) die("invalid scoring scheme");
	if (o->with_nth && o->nwith == 0) { free(o->with_nth); o->with_nth = NULL; }
}
