/* opts.h - every option of the C fzf, filled by opts.c from the command line, FZF_DEFAULT_OPTS and --bind etc. */
#ifndef FZF_OPTS_H
#define FZF_OPTS_H
#include "fzf.h"

typedef struct { char *name; char *arg; } Action;    /* one action of a binding: name and its argument ("" if none) */
typedef struct { Action *a; int n; } ActionList;

typedef struct { int key; ActionList acts; } Bind;   /* key = an encoded key/event code, see keys.c */

enum { CASE_SMART, CASE_IGNORE, CASE_RESPECT };
enum { LAYOUT_DEFAULT, LAYOUT_REVERSE, LAYOUT_REVERSE_LIST };
enum { INFO_DEFAULT, INFO_INLINE, INFO_INLINE_RIGHT, INFO_RIGHT, INFO_HIDDEN };
enum { BORDER_NONE, BORDER_ROUNDED, BORDER_SHARP, BORDER_BOLD, BORDER_DOUBLE, BORDER_HORIZONTAL, BORDER_VERTICAL,
       BORDER_TOP, BORDER_BOTTOM, BORDER_LEFT, BORDER_RIGHT };
enum { PW_RIGHT, PW_LEFT, PW_UP, PW_DOWN };

typedef struct { int fg, bg; int attr; } Color;        /* fg/bg: -1 default, 0..255 palette, 0x1000000|rgb truecolor */

struct Opts {
	/* --- search --- */
	bool extended, fuzzy, sort, tac, normalize, algo_v1;
	int case_mode;
	char scheme[16];
	Crit crit[5]; int ncrit;
	bool forward, crit_pos;
	Range *nth; int nnth;
	Range *with_nth; int nwith; char *with_tmpl;
	Delim delim;
	char *filter; bool has_filter;
	char *query;
	bool select1, exit0, print_query, read0, print0, ansi, sync_;
	char *expect;                    /* comma separated keys for --expect */
	int tabstop;
	Range *id_nth; int nid;

	/* --- interface --- */
	bool multi; int multi_max;
	int layout; int info; bool no_input, cycle, mouse, no_sort_ui, hscroll;
	char *prompt, *pointer, *marker, *ellipsis;
	char *header; int header_lines; bool header_first;
	char *footer;
	int scroll_off;
	char *height_s;                  /* "40%" or "20" or "~40%" or NULL = full screen */
	int min_height;
	int border; char *border_label; int border_label_pos;
	int list_border, input_border, header_border, footer_border;
	int margin[4], padding[4];       /* top right bottom left; values >= 0 cells, < 0 = percent */
	char *history_file; int history_size;
	bool track, disabled, keep_right, highlight_line, wrap, gap;
	bool no_color; char *color_spec; Color col[40];

	/* --- preview --- */
	char *preview_cmd; char *preview_window_spec;
	int pw_pos, pw_size; bool pw_size_pct, pw_border, pw_wrap, pw_follow, pw_hidden, pw_cycle, pw_border_set;
	int pw_scroll_off; char *pw_scroll_expr; int pw_header_lines;
	char *preview_label;

	/* --- bindings --- */
	Bind *binds; int nbinds;
	char **bind_raw; int nbind_raw;              /* --bind arguments, parsed once the key tables exist */
	char **preview_raw; int npreview_raw;
	char *listen_addr; bool listen_unsafe;
	char *with_shell;
	bool ascii;                      /* --no-unicode, and the default on the Linux console */
	int hints;                       /* rows of the key hint bar, 0 = off */
};

Opts *opts_new(void);
void opts_parse(Opts *o, int argc, char **argv);   /* also reads FZF_DEFAULT_OPTS first */
void opts_finish(Opts *o);                         /* derived values: criteria, forward, ... */

#endif
