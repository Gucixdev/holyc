/* fzf - a fuzzy finder in C, in one file. It was fzf/ (fzf.h, opts.h, term.h, ui.h, uistate.h, normtab.h and
 * the .c files, in that order below); two static helpers were renamed where two files had one of the same
 * name (ui_print_item_, act_set_str_). Build: make fzf (bin/fzf), or cc -std=gnu11 -O2 -pthread -o fzf fzf.c -lm */

/* ======== fzf.h ======== */
/* fzf.h - shared declarations of the C fzf (see README.md). Plain C99 + POSIX, libc only. */
#ifndef FZF_H
#define FZF_H
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <regex.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

typedef uint32_t rune;

/* ---------------------------------------------------------------- util.c */
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

typedef struct { char *p; size_t len, cap; } Buf;
void buf_init(Buf *b);
void buf_free(Buf *b);
void buf_reset(Buf *b);
void buf_addn(Buf *b, const char *s, size_t n);
void buf_adds(Buf *b, const char *s);
void buf_addc(Buf *b, char c);
void buf_printf(Buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
char *buf_take(Buf *b);            /* NUL-terminated, caller frees, buffer reset */

int u8dec(const char *s, size_t n, rune *out);       /* bytes consumed (>=1); invalid -> U+FFFD, 1 byte */
size_t u8enc(rune r, char *out);
int rune_width(rune r);                              /* 0, 1 or 2 cells */
int str_width(const char *s, size_t n);
size_t utf8_to_runes(const char *s, size_t n, rune *out);   /* out needs n slots; returns count */
size_t utf8_len(const char *s, size_t n);                    /* number of runes */
rune rune_lower(rune r);
rune rune_upper(rune r);
bool rune_is_space(rune r);
void locale_init(void);

/* ---------------------------------------------------------------- algo.c */
typedef struct { int start, end, score; } MRes;      /* start < 0 = no match */
typedef struct { const rune *r; int n; } Runes;
typedef struct Scratch Scratch;
Scratch *scratch_new(void);
void scratch_free(Scratch *s);
bool algo_init(const char *scheme);                  /* "default", "path", "history" */
rune normalize_rune(rune r);
void normalize_runes(rune *r, int n);
typedef MRes (*AlgoFn)(bool cs, bool norm, bool fwd, Runes text, const rune *pat, int m, int *pos, int *npos, Scratch *sc);
MRes fuzzy_v2(bool cs, bool norm, bool fwd, Runes text, const rune *pat, int m, int *pos, int *npos, Scratch *sc);
MRes fuzzy_v1(bool cs, bool norm, bool fwd, Runes text, const rune *pat, int m, int *pos, int *npos, Scratch *sc);
MRes exact_naive(bool cs, bool norm, bool fwd, Runes text, const rune *pat, int m, int *pos, int *npos, Scratch *sc);
MRes exact_boundary(bool cs, bool norm, bool fwd, Runes text, const rune *pat, int m, int *pos, int *npos, Scratch *sc);
MRes prefix_match(bool cs, bool norm, bool fwd, Runes text, const rune *pat, int m, int *pos, int *npos, Scratch *sc);
MRes suffix_match(bool cs, bool norm, bool fwd, Runes text, const rune *pat, int m, int *pos, int *npos, Scratch *sc);
MRes equal_match(bool cs, bool norm, bool fwd, Runes text, const rune *pat, int m, int *pos, int *npos, Scratch *sc);
int leading_spaces(Runes t);
int trailing_spaces(Runes t);

/* ---------------------------------------------------------------- items and search (pattern.c) */
typedef struct { int begin, end; } Range;            /* 1-based, negative from the end, 0 = open end (..) */
typedef struct { char *str; size_t len; regex_t re; bool has_re; } Delim;   /* str NULL and no regex = the awk default */
bool range_parse(const char *s, Range *r);
int ranges_parse(const char *spec, Range **out);     /* comma separated; -1 on error */
void delim_set(Delim *d, const char *s);

typedef struct { uint32_t off, len; uint32_t fg, bg; uint16_t attr; } AnsiSpan;   /* colour of a byte range of Item.text */

typedef struct Item {
	char *orig;  uint32_t olen;      /* the line as it was read (printed as the result) */
	char *text;  uint32_t tlen;      /* what is shown and matched: --with-nth applied, ANSI codes removed */
	uint32_t index;                  /* input order */
	uint32_t nrunes;                 /* runes in text */
	uint16_t trim;                   /* length without surrounding blanks, in runes */
	AnsiSpan *ansi; uint32_t nansi;  /* colours found in the line (--ansi) */
	char *id;                        /* --id-nth key, NULL = the text */
	uint8_t flags;
} Item;
#define ITEM_SEL 1

typedef enum { CR_SCORE, CR_CHUNK, CR_LENGTH, CR_BEGIN, CR_END, CR_PATHNAME } Crit;

typedef struct Opts Opts;                            /* defined in opts.h */
typedef struct Pattern Pattern;
typedef struct { uint16_t pts[4]; int *pos; int npos; int minb, mine, maxe; } MatchOut;

Pattern *pattern_new(const Opts *o, const char *query);
void pattern_free(Pattern *p);
bool pattern_empty(const Pattern *p);
bool pattern_sortable(const Pattern *p);
bool pattern_match(const Pattern *p, Item *it, Scratch *sc, bool want_pos, MatchOut *out);

typedef struct { uint32_t idx; uint64_t key; } Hit;
/* runs the pattern over items[0..n), returns the hits sorted by rank (or in input order when the pattern is empty
 * or sorting is off). *nhits is the count; the caller frees the array. */
Hit *search_items(const Opts *o, Pattern *p, Item **items, size_t n, size_t *nhits);

bool ansi_extract(const char *s, size_t n, Buf *out, AnsiSpan **spans, uint32_t *nspans, uint32_t *cap);
rune *scratch_rbuf(Scratch *s, size_t n);
int *scratch_ibuf(Scratch *s, int which, size_t n);   /* three reusable int buffers per scratch */
Item *item_new(const Opts *o, const char *line, size_t len);
void item_free(Item *it);

#endif

/* ======== opts.h ======== */
/* opts.h - every option of the C fzf, filled by opts.c from the command line, FZF_DEFAULT_OPTS and --bind etc. */
#ifndef FZF_OPTS_H
#define FZF_OPTS_H

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

/* ======== term.h ======== */
/* term.h - the terminal: raw mode on /dev/tty, buffered output, keys, mouse. */
#ifndef FZF_TERM_H
#define FZF_TERM_H

/* A key is a code point, or a special key, plus modifier bits. */
enum {
	K_ENTER = 0x110000, K_TAB, K_BTAB, K_ESC, K_BSPACE, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_PGUP, K_PGDN, K_INS, K_DEL,
	K_F1, K_F2, K_F3, K_F4, K_F5, K_F6, K_F7, K_F8, K_F9, K_F10, K_F11, K_F12,
	K_CLICK, K_DCLICK, K_RCLICK, K_SCROLL_UP, K_SCROLL_DOWN, K_FOCUS_IN, K_FOCUS_OUT, K_PASTE,
	K_LAST
};
#define M_ALT (1 << 24)
#define M_CTRL (1 << 25)
#define M_SHIFT (1 << 26)
#define K_BASE(k) ((k) & 0xFFFFFF)
#define K_MODS(k) ((k) & ~0xFFFFFF)

/* events that are not keys (used by --bind) */
enum { E_START = 0x120000, E_LOAD, E_RESIZE, E_FOCUS, E_ONE, E_ZERO, E_RESULT, E_CHANGE, E_BACKWARD_EOF, E_JUMP, E_JUMP_CANCEL, E_MULTI, E_LAST };

typedef struct { int key; int x, y; int mouse; char *paste; } Ev;    /* mouse: 1 when key is a mouse event; x,y 0-based cells */

bool term_open(bool alt_screen, bool mouse);
void term_close(void);
int term_fd(void);
void term_size(int *rows, int *cols);
int term_read(Ev *e);                    /* reads what is available and decodes one event; 1 = got one, 0 = none yet */
bool term_pending(void);                 /* undecoded bytes are still buffered */
void term_out(const char *s, size_t n);
void term_outs(const char *s);
void term_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void term_flush(void);
bool term_resized(void);                 /* SIGWINCH since the last call */
int term_cursor_row(void);               /* asks the terminal where the cursor is (1-based row), -1 if it does not answer */
void term_set_mouse(bool on);
bool term_kitty(bool on);                /* kitty keyboard protocol (held modifiers); false if the terminal lacks it */
bool term_kitty_active(void);            /* modifier tracking works: hints can be filtered by held keys */
int term_mods(void);                     /* M_CTRL|M_ALT|M_SHIFT currently held (only with the protocol) */
bool term_mods_dirty(void);              /* the held set changed since the last call */
#endif

/* ======== ui.h ======== */
/* ui.h - theme, key bindings and actions shared between keys.c, theme.c and ui.c. */
#ifndef FZF_UI_H
#define FZF_UI_H

/* ---- theme: one slot per thing that can be coloured ---- */
enum {
	C_FG, C_BG, C_HL, C_FGP, C_BGP, C_HLP, C_INFO, C_BORDER, C_PROMPT, C_POINTER, C_MARKER, C_SPINNER, C_HEADER, C_FOOTER,
	C_GUTTER, C_QUERY, C_DISABLED, C_PFG, C_PBG, C_LABEL, C_SEP, C_SCROLL, C_PSCROLL, C_PBORDER, C_PLABEL, C_SELFG, C_SELBG,
	C_GHOST, C_NTH, C_COUNT
};
#define COL_UNDEF (-2)          /* not set: inherits */
#define COL_DEF (-1)            /* the terminal's default colour */
#define ATTR_BOLD 1
#define ATTR_DIM 2
#define ATTR_ITAL 4
#define ATTR_UL 8
#define ATTR_BLINK 16
#define ATTR_REV 32
#define ATTR_STRIKE 64
typedef struct { int fg, bg; int attr; } Col;
typedef struct { Col c[C_COUNT]; bool colored; } Theme;
void theme_default(Theme *t);
void theme_apply(Theme *t, const char *spec);    /* --color value(s) */
void theme_load_file(Theme *t);                  /* ~/.config/fzf/theme */
void theme_resolve(Theme *t);                    /* fills inherited slots */
void sgr_emit(Buf *b, Col c);                    /* \e[0;...m for a colour (starts with a reset) */

/* ---- actions ---- */
typedef enum {
	A_NONE, A_ABORT, A_ACCEPT, A_ACCEPT_NON_EMPTY, A_ACCEPT_OR_PRINT_QUERY, A_BACKWARD_CHAR, A_BACKWARD_DELETE_CHAR,
	A_BACKWARD_DELETE_CHAR_EOF, A_BACKWARD_KILL_WORD, A_BACKWARD_WORD, A_BEGINNING_OF_LINE, A_BECOME, A_BELL, A_CANCEL,
	A_CHANGE_BORDER_LABEL, A_CHANGE_FOOTER, A_CHANGE_HEADER, A_CHANGE_MULTI, A_CHANGE_NTH, A_CHANGE_PREVIEW,
	A_CHANGE_PREVIEW_LABEL, A_CHANGE_PREVIEW_WINDOW, A_CHANGE_PROMPT, A_CHANGE_QUERY, A_CLEAR_QUERY, A_CLEAR_SCREEN,
	A_CLEAR_SELECTION, A_CLOSE, A_DELETE_CHAR, A_DELETE_CHAR_EOF, A_DESELECT, A_DESELECT_ALL, A_DISABLE_SEARCH, A_DOWN,
	A_ENABLE_SEARCH, A_END_OF_LINE, A_EXECUTE, A_EXECUTE_SILENT, A_FIRST, A_FORWARD_CHAR, A_FORWARD_WORD, A_HALF_PAGE_DOWN,
	A_HALF_PAGE_UP, A_HIDE_HEADER, A_HIDE_INPUT, A_HIDE_PREVIEW, A_IGNORE, A_JUMP, A_KILL_LINE, A_KILL_WORD, A_LAST,
	A_NEXT_HISTORY, A_NEXT_SELECTED, A_PAGE_DOWN, A_PAGE_UP, A_POS, A_PREV_HISTORY, A_PREV_SELECTED, A_PREVIEW,
	A_PREVIEW_BOTTOM, A_PREVIEW_DOWN, A_PREVIEW_HALF_PAGE_DOWN, A_PREVIEW_HALF_PAGE_UP, A_PREVIEW_PAGE_DOWN,
	A_PREVIEW_PAGE_UP, A_PREVIEW_TOP, A_PREVIEW_UP, A_PRINT, A_PUT, A_REBIND, A_REFRESH_PREVIEW, A_RELOAD, A_RELOAD_SYNC,
	A_REPLACE_QUERY, A_SEARCH, A_SELECT, A_SELECT_ALL, A_SHOW_HEADER, A_SHOW_INPUT, A_SHOW_PREVIEW, A_TOGGLE,
	A_TOGGLE_ALL, A_TOGGLE_HEADER, A_TOGGLE_IN, A_TOGGLE_INPUT, A_TOGGLE_OUT, A_TOGGLE_PREVIEW, A_TOGGLE_PREVIEW_WRAP,
	A_TOGGLE_SEARCH, A_TOGGLE_SORT, A_TOGGLE_TRACK, A_TOGGLE_WRAP, A_TRACK_CURRENT, A_TRANSFORM, A_TRANSFORM_BORDER_LABEL,
	A_TRANSFORM_FOOTER, A_TRANSFORM_HEADER, A_TRANSFORM_PREVIEW_LABEL, A_TRANSFORM_PROMPT, A_TRANSFORM_QUERY, A_UNBIND,
	A_UNIX_LINE_DISCARD, A_UNIX_WORD_RUBOUT, A_UP, A_YANK, A_TOP, A_LAST_ACTION
} ActId;
typedef struct { ActId id; char *arg; } Act;
typedef struct { Act *v; int n; } ActLoc;
typedef struct { int key; ActLoc acts; char *label; } Binding;   /* label: from "key:action # label" */

int key_parse(const char *name);                 /* "ctrl-x", "alt-enter", "f5", "click", "start", ... ; -1 if unknown */
ActId action_id(const char *name);               /* -1 style: A_NONE if unknown */
const char *action_takes_arg(ActId id);          /* NULL = none, "" = optional, "arg" = required */
void bind_defaults(Binding **v, int *n);
void bind_parse(Binding **v, int *n, const char *spec);       /* --bind value */
ActLoc *bind_find(Binding *v, int n, int key);
void acts_from_string(ActLoc *out, const char *s);            /* "up+up+execute(ls)": used by --listen and transform */
void acts_free(ActLoc *a);

#endif

/* ======== uistate.h ======== */
/* uistate.h - the state of the interactive interface, shared by ui.c (loop, layout, drawing), act.c (actions)
 * and proc.c (child processes: preview, reload, execute, placeholders, --listen). */
#ifndef FZF_UISTATE_H
#define FZF_UISTATE_H
#include <signal.h>
#include <sys/types.h>

typedef struct { Item **v; size_t n, cap; } ItemVec;
void itemvec_push(ItemVec *v, Item *it);

typedef struct { int x, y, w, h; } Rect;
typedef struct { rune ch; Col c; } Cell;          /* ch == 0: the right half of a double-width cell */
typedef struct { Cell *cell; int w, h; } Grid;

/* --preview-window, parsed */
typedef struct PW {
	int pos, size; bool pct;              /* PW_* */
	int border;                           /* BORDER_* */
	bool wrap, follow, cycle, hidden, info;
	int header_lines;
	char *scroll;                         /* "+{2}+3/3" */
	int threshold; struct PW *alt;        /* <N(alt) : used when the terminal has fewer columns than N */
} PW;

/* a child process whose stdout we read (preview, reload, the input command) */
typedef struct { int fd; pid_t pid; bool live; } Proc;

typedef struct { char *name; char *fmt; } Kv;

typedef struct {
	Opts *o;
	Theme th;
	Grid g;
	int rows, cols, top_row;              /* screen size, first screen row of our area (0-based; --height) */
	Rect area;                            /* the whole interface (after --height, margin) */
	bool fullscreen;

	/* candidates */
	ItemVec items;                        /* searchable */
	ItemVec hdr;                          /* --header-lines */
	int hdr_left;                         /* header lines still to take from the input */
	Hit *hits; size_t nhits;
	Pattern *pat; Scratch *sc;
	bool need_search; double search_at; unsigned gen;
	size_t cur, top;                      /* cursor and first visible row, as indexes into hits */
	Item **sel; size_t nsel, selcap;      /* selection order */
	bool sort;                            /* --no-sort / toggle-sort */
	bool searching_disabled;
	char *track_key; bool track_on, track_have;

	/* input feed */
	Proc feed; Buf feed_part; bool feed_done, feed_replace, feed_sync_wait; bool got_first_input;
	Proc reload; Buf reload_part; bool reload_sync;
	FILE *unused;

	/* query line */
	rune *q; int qn, qcap, qc;
	rune *yank; int nyank;
	char *prompt, *header, *footer, *border_label, *preview_label; char *ghost;
	bool hide_input;
	char **hist; int nhist, hist_pos; char *hist_saved;

	/* preview */
	PW pw, pw_active; bool pv_hidden;
	char *pv_cmd;
	Proc pv; Buf pv_out; size_t *pv_lines; size_t pv_nl, pv_cap; bool pv_done;
	int pv_scroll, pv_hscroll; double pv_start_at; bool pv_pending; uint32_t pv_item;
	bool pv_wrap; char *pv_title;
	Rect pv_rect, list_rect, prompt_rect, hdr_rect, info_rect;

	/* bindings */
	Binding *binds; int nbinds;
	Binding *events; int nevents;         /* start/load/... */
	int *unbound; int nunbound;
	char *last_key;                       /* name of the key that triggered the current actions, for FZF_KEY */
	char *expect_key;                     /* set when an --expect key ended the session */
	char **expects; int nexpects;

	/* misc */
	bool running; int exit_code;
	bool accepted;
	Buf printq;                           /* print(...) actions output */
	bool jump_mode; char *jump_labels;
	double now;
	int click_x, click_y;
	bool spinner_on; int listen_fd; int *clients; int nclients;
	char *listen_port; char *api_key;
	char *become_cmd;
	int mouse_last_x, mouse_last_y; double last_click_at;
	bool in_change_event;
	bool list_padbar, info_rule; size_t *rowmap;
	struct Hint *hints; int nhints, hint_rows;
} UI;

/* ui.c */
extern UI U;
double now_sec(void);
void ui_dirty(void);
void ui_query_changed(void);
void ui_run_search(void);
void ui_move(int delta);
void ui_set_cursor(size_t idx);
Item *ui_current(void);
void ui_query_set(const char *s);
char *ui_query_str(void);
void ui_fire(int ev);
void ui_pw_apply(const char *spec);
void ui_resize(void);
void ui_status(const char *fmt, ...);
void ui_toggle_sel(Item *it, bool on);
void ui_select_all(bool on);
void ui_clear_sel(void);
void ui_items_clear(void);
void ui_after_feed(void);
int ui_main(Opts *o);

/* act.c */
void act_run(const ActLoc *a, int key);
void act_run_list(const Act *v, int n);
void act_paste(const char *s);
bool act_query_key(int key);          /* printable text to the query */

/* proc.c */
bool proc_start(Proc *p, const char *cmd, int cap_stdin_fd);
void proc_kill(Proc *p);
void proc_reap(Proc *p);
char *proc_quote(const char *s);
char *expand_template(const char *tmpl, bool for_preview);
int proc_run_capture(const char *cmd, Buf *out);
void proc_execute(const char *cmd, bool silent);
void proc_become(const char *cmd);
void pv_kill(void);
void pv_schedule(void);
void pv_poll(void);
void pv_start(void);
void pv_scroll(int lines);
void pv_page(int dir, bool half);
void pv_toggle(void);
int pv_nfds(void);
void listen_start(void);
void listen_poll(void);
void feed_setup(void);
void feed_read(void);
void reload_start(const char *cmd, bool sync);
void reload_read(void);
void history_load(void);
void history_save(const char *q);
char *env_setup(void);
char *item_key(const Item *it);

/* draw.c helpers used by hints.c */
Col col_mk(int fg, int bg, int attr);
int gs(Rect clip, int x, int y, const char *s, Col c);
void gfill(Rect r, Col c);

/* hints.c */
void hints_build(void);
void hints_draw(Rect r);
bool hints_click(int x, int y);
int hints_rows(void);

/* draw.c */
void draw_all(void);
void grid_init(int w, int h);
void present(void);
int border_edges(int shape, int *t, int *r, int *b, int *l);
extern const char *pw_default_spec;

#endif

/* ======== normtab.h ======== */
/* normtab.h - accent folding table (lowercase Latin letters to ASCII), generated from fzf's documented
 * normalization table by tests/gen-normtab. Sorted by code point for binary search. */
static const struct { uint32_t from; uint8_t to; } norm_tab[] = {
	{0x00C0,'A'}, {0x00C1,'A'}, {0x00C2,'A'}, {0x00C3,'A'}, {0x00C4,'A'}, {0x00C5,'A'},
	{0x00C7,'C'}, {0x00C8,'E'}, {0x00C9,'E'}, {0x00CA,'E'}, {0x00CB,'E'}, {0x00CC,'I'},
	{0x00CD,'I'}, {0x00CE,'I'}, {0x00CF,'I'}, {0x00D1,'N'}, {0x00D2,'O'}, {0x00D3,'O'},
	{0x00D4,'O'}, {0x00D5,'O'}, {0x00D6,'O'}, {0x00D8,'O'}, {0x00D9,'U'}, {0x00DA,'U'},
	{0x00DB,'U'}, {0x00DC,'U'}, {0x00DD,'Y'}, {0x00DF,'s'}, {0x00E0,'a'}, {0x00E1,'a'},
	{0x00E2,'a'}, {0x00E3,'a'}, {0x00E4,'a'}, {0x00E5,'a'}, {0x00E7,'c'}, {0x00E8,'e'},
	{0x00E9,'e'}, {0x00EA,'e'}, {0x00EB,'e'}, {0x00EC,'i'}, {0x00ED,'i'}, {0x00EE,'i'},
	{0x00EF,'i'}, {0x00F1,'n'}, {0x00F2,'o'}, {0x00F3,'o'}, {0x00F4,'o'}, {0x00F5,'o'},
	{0x00F6,'o'}, {0x00F8,'o'}, {0x00F9,'u'}, {0x00FA,'u'}, {0x00FB,'u'}, {0x00FC,'u'},
	{0x00FD,'y'}, {0x00FF,'y'}, {0x0101,'a'}, {0x0103,'a'}, {0x0105,'a'}, {0x0107,'c'},
	{0x0109,'c'}, {0x010B,'c'}, {0x010D,'c'}, {0x010F,'d'}, {0x0111,'d'}, {0x0113,'e'},
	{0x0115,'e'}, {0x0117,'e'}, {0x0119,'e'}, {0x011B,'e'}, {0x011D,'g'}, {0x011F,'g'},
	{0x0121,'g'}, {0x0123,'g'}, {0x0125,'h'}, {0x0127,'h'}, {0x0129,'i'}, {0x012B,'i'},
	{0x012D,'i'}, {0x012F,'i'}, {0x0130,'I'}, {0x0131,'i'}, {0x0135,'j'}, {0x0137,'k'},
	{0x013A,'l'}, {0x013C,'l'}, {0x013E,'l'}, {0x0140,'l'}, {0x0142,'l'}, {0x0144,'n'},
	{0x0146,'n'}, {0x0148,'n'}, {0x014D,'o'}, {0x014F,'o'}, {0x0151,'o'}, {0x0155,'r'},
	{0x0157,'r'}, {0x0159,'r'}, {0x015B,'s'}, {0x015D,'s'}, {0x015F,'s'}, {0x0161,'s'},
	{0x0163,'t'}, {0x0165,'t'}, {0x0167,'t'}, {0x0169,'u'}, {0x016B,'u'}, {0x016D,'u'},
	{0x016F,'u'}, {0x0171,'u'}, {0x0173,'u'}, {0x0175,'w'}, {0x0177,'y'}, {0x0178,'Y'},
	{0x017A,'z'}, {0x017C,'z'}, {0x017E,'z'}, {0x017F,'s'}, {0x0180,'b'}, {0x0181,'B'},
	{0x0183,'b'}, {0x0186,'O'}, {0x0188,'c'}, {0x0189,'D'}, {0x018A,'D'}, {0x018C,'d'},
	{0x018E,'E'}, {0x0190,'E'}, {0x0192,'f'}, {0x0193,'G'}, {0x0197,'I'}, {0x0199,'k'},
	{0x019A,'l'}, {0x019C,'M'}, {0x019D,'N'}, {0x019E,'n'}, {0x019F,'O'}, {0x01A1,'o'},
	{0x01A5,'p'}, {0x01AB,'t'}, {0x01AD,'t'}, {0x01AE,'T'}, {0x01B0,'u'}, {0x01B2,'V'},
	{0x01B4,'y'}, {0x01B6,'z'}, {0x01CE,'a'}, {0x01D0,'i'}, {0x01D2,'o'}, {0x01D4,'u'},
	{0x01DD,'e'}, {0x01E5,'g'}, {0x01E7,'g'}, {0x01E9,'k'}, {0x01EB,'o'}, {0x01F0,'j'},
	{0x01F5,'g'}, {0x01F9,'n'}, {0x0201,'a'}, {0x0203,'a'}, {0x0205,'e'}, {0x0207,'e'},
	{0x0209,'i'}, {0x020B,'i'}, {0x020D,'o'}, {0x020F,'o'}, {0x0211,'r'}, {0x0213,'r'},
	{0x0215,'u'}, {0x0217,'u'}, {0x0219,'s'}, {0x021B,'t'}, {0x021F,'h'}, {0x0220,'N'},
	{0x0221,'d'}, {0x0225,'z'}, {0x0227,'a'}, {0x0229,'e'}, {0x022F,'o'}, {0x0233,'y'},
	{0x0234,'l'}, {0x0235,'n'}, {0x0236,'t'}, {0x0237,'j'}, {0x023A,'A'}, {0x023B,'C'},
	{0x023C,'c'}, {0x023D,'L'}, {0x023E,'T'}, {0x023F,'s'}, {0x0240,'z'}, {0x0243,'B'},
	{0x0244,'U'}, {0x0245,'V'}, {0x0246,'E'}, {0x0247,'e'}, {0x0248,'J'}, {0x0249,'j'},
	{0x024A,'Q'}, {0x024B,'q'}, {0x024C,'R'}, {0x024D,'r'}, {0x024E,'Y'}, {0x024F,'y'},
	{0x0250,'a'}, {0x0251,'a'}, {0x0253,'b'}, {0x0254,'o'}, {0x0255,'c'}, {0x0256,'d'},
	{0x0257,'d'}, {0x0258,'e'}, {0x025B,'e'}, {0x025C,'e'}, {0x025D,'e'}, {0x025E,'e'},
	{0x025F,'j'}, {0x0260,'g'}, {0x0261,'g'}, {0x0262,'G'}, {0x0265,'h'}, {0x0266,'h'},
	{0x0268,'i'}, {0x026A,'I'}, {0x026B,'l'}, {0x026C,'l'}, {0x026D,'l'}, {0x026F,'m'},
	{0x0270,'m'}, {0x0271,'m'}, {0x0272,'n'}, {0x0273,'n'}, {0x0274,'N'}, {0x0275,'o'},
	{0x0279,'r'}, {0x027A,'r'}, {0x027B,'r'}, {0x027C,'r'}, {0x027D,'r'}, {0x027E,'r'},
	{0x027F,'r'}, {0x0280,'R'}, {0x0281,'R'}, {0x0282,'s'}, {0x0287,'t'}, {0x0288,'t'},
	{0x0289,'u'}, {0x028B,'v'}, {0x028C,'v'}, {0x028D,'w'}, {0x028E,'y'}, {0x028F,'Y'},
	{0x0290,'z'}, {0x0291,'z'}, {0x0297,'c'}, {0x0299,'B'}, {0x029A,'e'}, {0x029B,'G'},
	{0x029C,'H'}, {0x029D,'j'}, {0x029E,'k'}, {0x029F,'L'}, {0x02A0,'q'}, {0x02AE,'h'},
	{0x0363,'a'}, {0x0364,'e'}, {0x0365,'i'}, {0x0366,'o'}, {0x0367,'u'}, {0x0368,'c'},
	{0x0369,'d'}, {0x036A,'h'}, {0x036B,'m'}, {0x036C,'r'}, {0x036D,'t'}, {0x036E,'v'},
	{0x036F,'x'}, {0x1D00,'A'}, {0x1D03,'B'}, {0x1D04,'C'}, {0x1D05,'D'}, {0x1D07,'E'},
	{0x1D08,'e'}, {0x1D09,'i'}, {0x1D0A,'J'}, {0x1D0B,'K'}, {0x1D0C,'L'}, {0x1D0D,'M'},
	{0x1D0E,'N'}, {0x1D0F,'O'}, {0x1D10,'O'}, {0x1D11,'o'}, {0x1D12,'o'}, {0x1D13,'o'},
	{0x1D16,'o'}, {0x1D17,'o'}, {0x1D18,'P'}, {0x1D19,'R'}, {0x1D1A,'R'}, {0x1D1B,'T'},
	{0x1D1C,'U'}, {0x1D1D,'u'}, {0x1D1E,'u'}, {0x1D1F,'m'}, {0x1D20,'V'}, {0x1D21,'W'},
	{0x1D22,'Z'}, {0x1D62,'i'}, {0x1D63,'r'}, {0x1D64,'u'}, {0x1D65,'v'}, {0x1E01,'a'},
	{0x1E03,'b'}, {0x1E05,'b'}, {0x1E07,'b'}, {0x1E0B,'d'}, {0x1E0D,'d'}, {0x1E0F,'d'},
	{0x1E11,'d'}, {0x1E13,'d'}, {0x1E19,'e'}, {0x1E1B,'e'}, {0x1E1F,'f'}, {0x1E21,'g'},
	{0x1E23,'h'}, {0x1E25,'h'}, {0x1E27,'h'}, {0x1E29,'h'}, {0x1E2B,'h'}, {0x1E2D,'i'},
	{0x1E31,'k'}, {0x1E33,'k'}, {0x1E35,'k'}, {0x1E37,'l'}, {0x1E3B,'l'}, {0x1E3D,'l'},
	{0x1E3F,'m'}, {0x1E41,'m'}, {0x1E43,'m'}, {0x1E45,'n'}, {0x1E47,'n'}, {0x1E49,'n'},
	{0x1E4B,'n'}, {0x1E55,'p'}, {0x1E57,'p'}, {0x1E59,'r'}, {0x1E5B,'r'}, {0x1E5F,'r'},
	{0x1E61,'s'}, {0x1E63,'s'}, {0x1E6B,'t'}, {0x1E6D,'t'}, {0x1E6F,'t'}, {0x1E71,'t'},
	{0x1E73,'u'}, {0x1E75,'u'}, {0x1E77,'u'}, {0x1E7D,'v'}, {0x1E7F,'v'}, {0x1E81,'w'},
	{0x1E83,'w'}, {0x1E85,'w'}, {0x1E87,'w'}, {0x1E89,'w'}, {0x1E8B,'x'}, {0x1E8D,'x'},
	{0x1E8F,'y'}, {0x1E91,'z'}, {0x1E93,'z'}, {0x1E95,'z'}, {0x1E96,'h'}, {0x1E97,'t'},
	{0x1E98,'w'}, {0x1E99,'y'}, {0x1E9A,'a'}, {0x1E9B,'s'}, {0x1EA1,'a'}, {0x1EA3,'a'},
	{0x1EB9,'e'}, {0x1EBB,'e'}, {0x1EBD,'e'}, {0x1EC9,'i'}, {0x1ECB,'i'}, {0x1ECD,'o'},
	{0x1ECF,'o'}, {0x1EE5,'u'}, {0x1EE7,'u'}, {0x1EF3,'y'}, {0x1EF5,'y'}, {0x1EF7,'y'},
	{0x1EF9,'y'}, {0x2071,'i'}, {0x2095,'h'}, {0x2096,'k'}, {0x2097,'l'}, {0x2098,'m'},
	{0x2099,'n'}, {0x209A,'p'}, {0x209B,'s'}, {0x209C,'t'}, {0x2184,'c'}, {0xFF01,'!'},
	{0xFF02,'"'}, {0xFF03,'#'}, {0xFF04,'$'}, {0xFF05,'%'}, {0xFF06,'&'}, {0xFF07,'\\'},
	{0xFF08,'('}, {0xFF09,')'}, {0xFF0A,'*'}, {0xFF0B,'+'}, {0xFF0C,','}, {0xFF0D,'-'},
	{0xFF0E,'.'}, {0xFF0F,'/'}, {0xFF10,'0'}, {0xFF11,'1'}, {0xFF12,'2'}, {0xFF13,'3'},
	{0xFF14,'4'}, {0xFF15,'5'}, {0xFF16,'6'}, {0xFF17,'7'}, {0xFF18,'8'}, {0xFF19,'9'},
	{0xFF1A,':'}, {0xFF1B,';'}, {0xFF1C,'<'}, {0xFF1D,'='}, {0xFF1E,'>'}, {0xFF1F,'?'},
	{0xFF20,'@'}, {0xFF21,'A'}, {0xFF22,'B'}, {0xFF23,'C'}, {0xFF24,'D'}, {0xFF25,'E'},
	{0xFF26,'F'}, {0xFF27,'G'}, {0xFF28,'H'}, {0xFF29,'I'}, {0xFF2A,'J'}, {0xFF2B,'K'},
	{0xFF2C,'L'}, {0xFF2D,'M'}, {0xFF2E,'N'}, {0xFF2F,'O'}, {0xFF30,'P'}, {0xFF31,'Q'},
	{0xFF32,'R'}, {0xFF33,'S'}, {0xFF34,'T'}, {0xFF35,'U'}, {0xFF36,'V'}, {0xFF37,'W'},
	{0xFF38,'X'}, {0xFF39,'Y'}, {0xFF3A,'Z'}, {0xFF3B,'['}, {0xFF3D,']'}, {0xFF3E,'^'},
	{0xFF3F,'_'}, {0xFF40,'`'}, {0xFF41,'a'}, {0xFF42,'b'}, {0xFF43,'c'}, {0xFF44,'d'},
	{0xFF45,'e'}, {0xFF46,'f'}, {0xFF47,'g'}, {0xFF48,'h'}, {0xFF49,'i'}, {0xFF4A,'j'},
	{0xFF4B,'k'}, {0xFF4C,'l'}, {0xFF4D,'m'}, {0xFF4E,'n'}, {0xFF4F,'o'}, {0xFF50,'p'},
	{0xFF51,'q'}, {0xFF52,'r'}, {0xFF53,'s'}, {0xFF54,'t'}, {0xFF55,'u'}, {0xFF56,'v'},
	{0xFF57,'w'}, {0xFF58,'x'}, {0xFF59,'y'}, {0xFF5A,'z'}, {0xFF5B,'{'}, {0xFF5C,'|'},
	{0xFF5D,'}'}, {0xFF5E,'~'}, {0xFF61,'.'},
};

/* ======== main.c ======== */
/* main.c - fzf in C: reads the candidates, runs either the non-interactive filter (-f) or the interface. */
#include <fcntl.h>
#include <signal.h>


static void vec_push(ItemVec *v, Item *it)
{
	if (v->n == v->cap) { v->cap = v->cap ? v->cap * 2 : 1024; v->v = xrealloc(v->v, v->cap * sizeof(Item *)); }
	it->index = (uint32_t)v->n;
	v->v[v->n++] = it;
}

/* reads every line of fd into items; returns false when there is nothing to read from (a terminal) */
static void read_all(const Opts *o, FILE *f, ItemVec *out)
{
	Buf line;
	buf_init(&line);
	int c;
	char sep = o->read0 ? 0 : '\n';
	char chunk[65536];
	size_t got;
	while ((got = fread(chunk, 1, sizeof chunk, f)) > 0) {
		for (size_t i = 0; i < got; i++) {
			c = (unsigned char)chunk[i];
			if (c == sep) { vec_push(out, item_new(o, line.p ? line.p : "", line.len)); buf_reset(&line); }
			else buf_addc(&line, (char)c);
		}
	}
	if (line.len) vec_push(out, item_new(o, line.p, line.len));
	buf_free(&line);
}

static void print_item(const Opts *o, const Item *it, FILE *out)
{
	if (o->ansi && it->nansi >= 0) {
		Buf b;
		buf_init(&b);
		ansi_extract(it->orig, it->olen, &b, NULL, NULL, NULL);
		fwrite(b.p ? b.p : "", 1, b.len, out);
		buf_free(&b);
	} else fwrite(it->orig, 1, it->olen, out);
	fputc(o->print0 ? 0 : '\n', out);
}

static int filter_mode(Opts *o, ItemVec *items)
{
	Pattern *p = pattern_new(o, o->filter);
	size_t nh;
	Hit *hits = search_items(o, p, items->v, items->n, &nh);
	if (o->print_query) { fputs(o->filter, stdout); fputc(o->print0 ? 0 : '\n', stdout); }
	for (size_t i = 0; i < nh; i++) print_item(o, items->v[hits[i].idx], stdout);
	free(hits);
	pattern_free(p);
	return nh ? 0 : 1;
}

int ui_main(Opts *o);                                             /* ui.c */

int main(int argc, char **argv)
{
	locale_init();
	signal(SIGPIPE, SIG_IGN);
	Opts *o = opts_new();
	opts_parse(o, argc, argv);
	opts_finish(o);
	ItemVec items = {0};
	if (o->has_filter) {
		if (isatty(0)) die("no input to filter (pipe candidates to fzf -f)");
		read_all(o, stdin, &items);
		return filter_mode(o, &items);
	}
	return ui_main(o);
}

/* ======== util.c ======== */
/* util.c - allocation, growable buffers, UTF-8 and terminal cell widths. */
#include <locale.h>

void die(const char *fmt, ...)
{
	va_list ap;
	fputs("fzf: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(2);
}
void *xmalloc(size_t n) { void *p = malloc(n ? n : 1); if (!p) die("out of memory"); return p; }
void *xrealloc(void *p, size_t n) { p = realloc(p, n ? n : 1); if (!p) die("out of memory"); return p; }
char *xstrdup(const char *s) { return xstrndup(s, strlen(s)); }
char *xstrndup(const char *s, size_t n) { char *p = xmalloc(n + 1); memcpy(p, s, n); p[n] = 0; return p; }

void buf_init(Buf *b) { b->p = NULL; b->len = b->cap = 0; }
void buf_free(Buf *b) { free(b->p); buf_init(b); }
void buf_reset(Buf *b) { b->len = 0; if (b->p) b->p[0] = 0; }
static void buf_grow(Buf *b, size_t extra)
{
	if (b->len + extra + 1 > b->cap) {
		size_t nc = b->cap ? b->cap * 2 : 256;
		while (nc < b->len + extra + 1) nc *= 2;
		b->p = xrealloc(b->p, nc);
		b->cap = nc;
	}
}
void buf_addn(Buf *b, const char *s, size_t n) { buf_grow(b, n); memcpy(b->p + b->len, s, n); b->len += n; b->p[b->len] = 0; }
void buf_adds(Buf *b, const char *s) { buf_addn(b, s, strlen(s)); }
void buf_addc(Buf *b, char c) { buf_addn(b, &c, 1); }
void buf_printf(Buf *b, const char *fmt, ...)
{
	va_list ap, ap2;
	va_start(ap, fmt);
	va_copy(ap2, ap);
	int n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (n > 0) { buf_grow(b, (size_t)n); vsnprintf(b->p + b->len, (size_t)n + 1, fmt, ap2); b->len += (size_t)n; }
	va_end(ap2);
}
char *buf_take(Buf *b)
{
	char *p = b->p ? b->p : xstrdup("");
	b->p = NULL; b->len = b->cap = 0;
	return p;
}

/* ---- UTF-8 ---- */
int u8dec(const char *s, size_t n, rune *out)
{
	const unsigned char *u = (const unsigned char *)s;
	if (n == 0) { *out = 0; return 0; }
	if (u[0] < 0x80) { *out = u[0]; return 1; }
	int len = (u[0] >= 0xF0 && u[0] < 0xF8) ? 4 : (u[0] >= 0xE0) ? 3 : (u[0] >= 0xC2 && u[0] < 0xE0) ? 2 : 0;
	if (u[0] >= 0xF8 || len == 0 || (size_t)len > n) { *out = 0xFFFD; return 1; }
	rune r = len == 2 ? (u[0] & 0x1F) : len == 3 ? (u[0] & 0x0F) : (u[0] & 0x07);
	for (int i = 1; i < len; i++) {
		if ((u[i] & 0xC0) != 0x80) { *out = 0xFFFD; return 1; }
		r = (r << 6) | (u[i] & 0x3F);
	}
	*out = r;
	return len;
}
size_t u8enc(rune r, char *o)
{
	if (r < 0x80) { o[0] = (char)r; return 1; }
	if (r < 0x800) { o[0] = (char)(0xC0 | (r >> 6)); o[1] = (char)(0x80 | (r & 0x3F)); return 2; }
	if (r < 0x10000) { o[0] = (char)(0xE0 | (r >> 12)); o[1] = (char)(0x80 | ((r >> 6) & 0x3F)); o[2] = (char)(0x80 | (r & 0x3F)); return 3; }
	o[0] = (char)(0xF0 | (r >> 18)); o[1] = (char)(0x80 | ((r >> 12) & 0x3F)); o[2] = (char)(0x80 | ((r >> 6) & 0x3F)); o[3] = (char)(0x80 | (r & 0x3F));
	return 4;
}
size_t utf8_to_runes(const char *s, size_t n, rune *out)
{
	size_t i = 0, k = 0;
	while (i < n) { rune r; i += (size_t)u8dec(s + i, n - i, &r); out[k++] = r; }
	return k;
}
size_t utf8_len(const char *s, size_t n)
{
	size_t i = 0, k = 0;
	while (i < n) { rune r; i += (size_t)u8dec(s + i, n - i, &r); k++; }
	return k;
}

/* Cell width: combining marks and controls 0, East Asian wide / emoji 2, everything else 1. */
int rune_width(rune r)
{
	if (r < 0x20 || (r >= 0x7F && r < 0xA0)) return 0;
	if (r < 0x300) return 1;
	static const struct { rune lo, hi; } zero[] = {
		{0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x05BF, 0x05BF}, {0x05C1, 0x05C2}, {0x05C4, 0x05C5}, {0x0610, 0x061A},
		{0x064B, 0x065F}, {0x0670, 0x0670}, {0x06D6, 0x06DC}, {0x06DF, 0x06E4}, {0x06E7, 0x06E8}, {0x06EA, 0x06ED}, {0x0E31, 0x0E31},
		{0x0E34, 0x0E3A}, {0x0E47, 0x0E4E}, {0x1AB0, 0x1AFF}, {0x1DC0, 0x1DFF}, {0x200B, 0x200F}, {0x202A, 0x202E}, {0x2060, 0x2064},
		{0x20D0, 0x20FF}, {0xFE00, 0xFE0F}, {0xFE20, 0xFE2F}, {0xFEFF, 0xFEFF}, {0xE0100, 0xE01EF}};
	for (size_t i = 0; i < sizeof zero / sizeof zero[0]; i++)
		if (r >= zero[i].lo && r <= zero[i].hi) return 0;
	static const struct { rune lo, hi; } wide[] = {
		{0x1100, 0x115F}, {0x231A, 0x231B}, {0x2329, 0x232A}, {0x23E9, 0x23EC}, {0x23F0, 0x23F0}, {0x23F3, 0x23F3}, {0x25FD, 0x25FE},
		{0x2614, 0x2615}, {0x2648, 0x2653}, {0x267F, 0x267F}, {0x2693, 0x2693}, {0x26A1, 0x26A1}, {0x26AA, 0x26AB}, {0x26BD, 0x26BE},
		{0x26C4, 0x26C5}, {0x26CE, 0x26CE}, {0x26D4, 0x26D4}, {0x26EA, 0x26EA}, {0x26F2, 0x26F3}, {0x26F5, 0x26F5}, {0x26FA, 0x26FA},
		{0x26FD, 0x26FD}, {0x2705, 0x2705}, {0x270A, 0x270B}, {0x2728, 0x2728}, {0x274C, 0x274C}, {0x274E, 0x274E}, {0x2753, 0x2755},
		{0x2757, 0x2757}, {0x2795, 0x2797}, {0x27B0, 0x27B0}, {0x27BF, 0x27BF}, {0x2B1B, 0x2B1C}, {0x2B50, 0x2B50}, {0x2B55, 0x2B55},
		{0x2E80, 0x303E}, {0x3041, 0x33FF}, {0x3400, 0x4DBF}, {0x4E00, 0x9FFF}, {0xA000, 0xA4CF}, {0xA960, 0xA97F}, {0xAC00, 0xD7A3},
		{0xF900, 0xFAFF}, {0xFE10, 0xFE19}, {0xFE30, 0xFE6F}, {0xFF00, 0xFF60}, {0xFFE0, 0xFFE6}, {0x1F300, 0x1F64F}, {0x1F680, 0x1F6FF},
		{0x1F900, 0x1F9FF}, {0x1FA70, 0x1FAFF}, {0x20000, 0x2FFFD}, {0x30000, 0x3FFFD}};
	for (size_t i = 0; i < sizeof wide / sizeof wide[0]; i++)
		if (r >= wide[i].lo && r <= wide[i].hi) return 2;
	return 1;
}
int str_width(const char *s, size_t n)
{
	int w = 0;
	size_t i = 0;
	while (i < n) { rune r; i += (size_t)u8dec(s + i, n - i, &r); w += rune_width(r); }
	return w;
}

void locale_init(void)
{
	if (!setlocale(LC_CTYPE, "")) setlocale(LC_CTYPE, "C.UTF-8");
	else if (MB_CUR_MAX == 1) setlocale(LC_CTYPE, "C.UTF-8");   /* the C locale cannot classify non-ASCII: use a UTF-8 one */
}
rune rune_lower(rune r) { return r < 0x80 ? ((r >= 'A' && r <= 'Z') ? r + 32 : r) : (rune)towlower((wint_t)r); }
rune rune_upper(rune r) { return r < 0x80 ? ((r >= 'a' && r <= 'z') ? r - 32 : r) : (rune)towupper((wint_t)r); }
bool rune_is_space(rune r)
{
	return r == ' ' || (r >= 9 && r <= 13) || r == 0x85 || r == 0xA0 || (r >= 0x2000 && r <= 0x200A) || r == 0x1680 || r == 0x2028 ||
	       r == 0x2029 || r == 0x202F || r == 0x205F || r == 0x3000;
}

/* ======== algo.c ======== */
/* algo.c - the matching algorithms. A port of the scoring scheme of fzf's FuzzyMatchV2 (a Smith-Waterman style
 * alignment with word-boundary / camelCase / consecutive bonuses and gap penalties), the greedy V1 fallback and the
 * exact / prefix / suffix / equal matchers, written from the documented algorithm so that ranking is the same. */

enum { C_WHITE, C_NONWORD, C_DELIM, C_LOWER, C_UPPER, C_LETTER, C_NUMBER };

#define SCORE_MATCH 16
#define SCORE_GAP_START (-3)
#define SCORE_GAP_EXT (-1)
#define BONUS_BOUNDARY (SCORE_MATCH / 2)
#define BONUS_NONWORD (SCORE_MATCH / 2)
#define BONUS_CAMEL123 (BONUS_BOUNDARY + SCORE_GAP_EXT)
#define BONUS_CONSECUTIVE (-(SCORE_GAP_START + SCORE_GAP_EXT))
#define BONUS_FIRST_MULT 2

static int16_t bonus_white = BONUS_BOUNDARY + 2, bonus_delim = BONUS_BOUNDARY + 1;
static int initial_class = C_WHITE;
static uint8_t ascii_class[128];
static int16_t bonus_matrix[7][7];
static const MRes NOMATCH = {-1, -1, 0};

static int class_of_ascii_init(int c, const char *delims)
{
	if (c >= 'a' && c <= 'z') return C_LOWER;
	if (c >= 'A' && c <= 'Z') return C_UPPER;
	if (c >= '0' && c <= '9') return C_NUMBER;
	if (strchr(" \t\n\v\f\r", c) && c) return C_WHITE;
	if (c && strchr(delims, c)) return C_DELIM;
	return C_NONWORD;
}

static int16_t bonus_for(int prev, int cls)
{
	if (cls >= C_NONWORD) {
		if (prev == C_WHITE) return bonus_white;
		if (prev == C_DELIM) return bonus_delim;
		if (prev == C_NONWORD) return BONUS_BOUNDARY;
	}
	if ((prev == C_LOWER && cls == C_UPPER) || (prev != C_NUMBER && cls == C_NUMBER)) return BONUS_CAMEL123;
	if (cls == C_NONWORD || cls == C_DELIM) return BONUS_NONWORD;
	if (cls == C_WHITE) return bonus_white;
	return 0;
}

bool algo_init(const char *scheme)
{
	const char *delims = "/,:;|";
	initial_class = C_WHITE;
	if (!strcmp(scheme, "default")) { bonus_white = BONUS_BOUNDARY + 2; bonus_delim = BONUS_BOUNDARY + 1; }
	else if (!strcmp(scheme, "path")) { bonus_white = BONUS_BOUNDARY; bonus_delim = BONUS_BOUNDARY + 1; delims = "/"; initial_class = C_DELIM; }
	else if (!strcmp(scheme, "history")) { bonus_white = BONUS_BOUNDARY; bonus_delim = BONUS_BOUNDARY; }
	else return false;
	for (int i = 0; i < 128; i++) ascii_class[i] = (uint8_t)class_of_ascii_init(i, delims);
	for (int i = 0; i <= C_NUMBER; i++)
		for (int j = 0; j <= C_NUMBER; j++) bonus_matrix[i][j] = bonus_for(i, j);
	return true;
}

static int class_of(rune r)
{
	if (r < 128) return ascii_class[r];
	if (iswlower((wint_t)r)) return C_LOWER;
	if (iswupper((wint_t)r)) return C_UPPER;
	if (iswdigit((wint_t)r)) return C_NUMBER;
	if (iswalpha((wint_t)r)) return C_LETTER;
	if (rune_is_space(r)) return C_WHITE;
	return C_NONWORD;
}

/* ---- accent folding: lowercase Latin letters (with accents and strokes) to ASCII, applied after case folding ---- */
rune normalize_rune(rune r)
{
	if (r < 0xC0 || r > 0x1EFF) return r;
	size_t lo = 0, hi = sizeof norm_tab / sizeof norm_tab[0];
	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		if (norm_tab[mid].from == r) return norm_tab[mid].to;
		if (norm_tab[mid].from < r) lo = mid + 1; else hi = mid;
	}
	return r;
}
void normalize_runes(rune *r, int n) { for (int i = 0; i < n; i++) r[i] = normalize_rune(r[i]); }

static inline rune fold(rune c, bool cs, bool norm)
{
	if (!cs) { if (c >= 'A' && c <= 'Z') c += 32; else if (c > 127) c = rune_lower(c); }
	if (norm && c > 127) c = normalize_rune(c);
	return c;
}

/* ---- scratch memory (one per thread) ---- */
struct Scratch { int16_t *i16; size_t c16; int32_t *i32; size_t c32; rune *rb; size_t crb; int *ib[3]; size_t cib[3]; };
Scratch *scratch_new(void) { return calloc(1, sizeof(Scratch)); }
void scratch_free(Scratch *s) { if (s) { free(s->i16); free(s->i32); free(s->rb); for (int i = 0; i < 3; i++) free(s->ib[i]); free(s); } }
int *scratch_ibuf(Scratch *s, int w, size_t n) { if (n > s->cib[w]) { s->cib[w] = n * 2 + 16; s->ib[w] = xrealloc(s->ib[w], s->cib[w] * sizeof(int)); } return s->ib[w]; }
rune *scratch_rbuf(Scratch *s, size_t n) { if (n > s->crb) { s->crb = n * 2 + 16; s->rb = xrealloc(s->rb, s->crb * sizeof(rune)); } return s->rb; }
static int16_t *a16(Scratch *s, size_t n, size_t off)
{
	if (off + n > s->c16) { s->c16 = (off + n) * 2; s->i16 = xrealloc(s->i16, s->c16 * sizeof(int16_t)); }
	return s->i16 + off;
}
static int32_t *a32(Scratch *s, size_t n, size_t off)
{
	if (off + n > s->c32) { s->c32 = (off + n) * 2; s->i32 = xrealloc(s->i32, s->c32 * sizeof(int32_t)); }
	return s->i32 + off;
}

/* window of the text that can hold a match: from one before the first occurrence of pat[0] to the last occurrence of the
 * last pattern character. Returns false when a pattern character is missing (no match at all). */
static bool fuzzy_index(Runes t, const rune *pat, int m, bool cs, bool norm, int *lo, int *hi)
{
	int first = 0, idx = 0, last = 0;
	rune pc = 0;
	for (int p = 0; p < m; p++) {
		pc = pat[p];
		while (idx < t.n && fold(t.r[idx], cs, norm) != pc) idx++;
		if (idx >= t.n) return false;
		if (p == 0 && idx > 0) first = idx - 1;
		last = idx++;
	}
	int end = last + 1;
	for (int i = t.n - 1; i > last; i--)
		if (fold(t.r[i], cs, norm) == pc) { end = i + 1; break; }
	*lo = first; *hi = end;
	return true;
}

static int16_t bonus_at(Runes t, int i)
{
	if (i == 0) return bonus_white;
	return bonus_matrix[class_of(t.r[i - 1])][class_of(t.r[i])];
}

static int calc_score(bool cs, bool norm, Runes t, const rune *pat, int m, int sidx, int eidx, int *pos, int *npos)
{
	int pidx = 0, score = 0, consecutive = 0, first_bonus = 0;
	bool in_gap = false;
	int prev = initial_class;
	int np = 0;
	if (sidx > 0) prev = class_of(t.r[sidx - 1]);
	for (int i = sidx; i < eidx; i++) {
		rune c = t.r[i];
		int cls = class_of(c);
		c = fold(c, cs, norm);
		if (pidx < m && c == pat[pidx]) {
			if (pos) pos[np++] = i;
			score += SCORE_MATCH;
			int bonus = bonus_matrix[prev][cls];
			if (consecutive == 0) first_bonus = bonus;
			else {
				if (bonus >= BONUS_BOUNDARY && bonus > first_bonus) first_bonus = bonus;
				if (first_bonus > bonus) bonus = first_bonus;
				if (BONUS_CONSECUTIVE > bonus) bonus = BONUS_CONSECUTIVE;
			}
			score += pidx == 0 ? bonus * BONUS_FIRST_MULT : bonus;
			in_gap = false;
			consecutive++;
			pidx++;
		} else {
			score += in_gap ? SCORE_GAP_EXT : SCORE_GAP_START;
			in_gap = true;
			consecutive = 0;
			first_bonus = 0;
		}
		prev = cls;
	}
	if (npos) *npos = np;
	return score;
}

MRes fuzzy_v1(bool cs, bool norm, bool fwd, Runes t, const rune *pat, int m, int *pos, int *npos, Scratch *sc)
{
	(void)sc;
	if (npos) *npos = 0;
	if (m == 0) return (MRes){0, 0, 0};
	int lo, hi;
	if (!fuzzy_index(t, pat, m, cs, norm, &lo, &hi)) return NOMATCH;
	int pidx = 0, sidx = -1, eidx = -1, n = t.n;
	for (int i = 0; i < n; i++) {
		rune c = fold(t.r[fwd ? i : n - i - 1], cs, norm);
		rune pc = pat[fwd ? pidx : m - pidx - 1];
		if (c == pc) {
			if (sidx < 0) sidx = i;
			if (++pidx == m) { eidx = i + 1; break; }
		}
	}
	if (sidx >= 0 && eidx >= 0) {
		pidx--;
		for (int i = eidx - 1; i >= sidx; i--) {
			rune c = fold(t.r[fwd ? i : n - i - 1], cs, norm);
			rune pc = pat[fwd ? pidx : m - pidx - 1];
			if (c == pc && --pidx < 0) { sidx = i; break; }
		}
		if (!fwd) { int s2 = n - eidx, e2 = n - sidx; sidx = s2; eidx = e2; }
		int score = calc_score(cs, norm, t, pat, m, sidx, eidx, pos, npos);
		return (MRes){sidx, eidx, score};
	}
	return NOMATCH;
}

MRes fuzzy_v2(bool cs, bool norm, bool fwd, Runes t, const rune *pat, int m, int *pos, int *npos, Scratch *sc)
{
	if (npos) *npos = 0;
	if (m == 0) return (MRes){0, 0, 0};
	int N = t.n;
	if (m > N) return NOMATCH;
	if ((int64_t)N * m > (1 << 21) || m > 1000) return fuzzy_v1(cs, norm, fwd, t, pat, m, pos, npos, sc);

	int minIdx, maxIdx;
	if (!fuzzy_index(t, pat, m, cs, norm, &minIdx, &maxIdx)) return NOMATCH;
	N = maxIdx - minIdx;

	size_t o16 = 0, o32 = 0;
	int16_t *H0 = a16(sc, (size_t)N, o16); o16 += (size_t)N;
	int16_t *C0 = a16(sc, (size_t)N, o16); o16 += (size_t)N;
	int16_t *B = a16(sc, (size_t)N, o16); o16 += (size_t)N;
	int32_t *F = a32(sc, (size_t)m, o32); o32 += (size_t)m;
	int32_t *T = a32(sc, (size_t)N, o32); o32 += (size_t)N;
	/* a16/a32 may have moved the block while growing: re-derive every pointer from the base */
	H0 = sc->i16; C0 = sc->i16 + N; B = sc->i16 + 2 * N; F = sc->i32; T = sc->i32 + m;
	for (int i = 0; i < N; i++) T[i] = (int32_t)t.r[minIdx + i];

	int maxScore = 0, maxScorePos = 0, pidx = 0, lastIdx = 0;
	rune pchar0 = pat[0], pchar = pat[0];
	int prevH0 = 0, prevClass = initial_class;
	bool inGap = false;
	for (int off = 0; off < N; off++) {
		rune c = (rune)T[off];
		int cls = class_of(c);
		c = fold(c, cs, norm);
		T[off] = (int32_t)c;
		int bonus = bonus_matrix[prevClass][cls];
		B[off] = (int16_t)bonus;
		prevClass = cls;
		if (c == pchar) {
			if (pidx < m) { F[pidx] = off; pidx++; pchar = pat[pidx < m - 1 ? pidx : m - 1]; }
			lastIdx = off;
		}
		if (c == pchar0) {
			int score = SCORE_MATCH + bonus * BONUS_FIRST_MULT;
			H0[off] = (int16_t)score;
			C0[off] = 1;
			if (m == 1 && ((fwd && score > maxScore) || (!fwd && score >= maxScore))) {
				maxScore = score; maxScorePos = off;
				if (fwd && bonus >= BONUS_BOUNDARY) break;
			}
			inGap = false;
		} else {
			int v = prevH0 + (inGap ? SCORE_GAP_EXT : SCORE_GAP_START);
			H0[off] = (int16_t)(v > 0 ? v : 0);
			C0[off] = 0;
			inGap = true;
		}
		prevH0 = H0[off];
	}
	if (pidx != m) return NOMATCH;
	if (m == 1) {
		if (pos) { pos[0] = minIdx + maxScorePos; if (npos) *npos = 1; }
		return (MRes){minIdx + maxScorePos, minIdx + maxScorePos + 1, maxScore};
	}

	int f0 = F[0], width = lastIdx - f0 + 1;
	int16_t *H = a16(sc, (size_t)width * (size_t)m, o16); o16 += (size_t)width * (size_t)m;
	int16_t *C = a16(sc, (size_t)width * (size_t)m, o16);
	H0 = sc->i16; C0 = sc->i16 + N; B = sc->i16 + 2 * N; F = sc->i32; T = sc->i32 + m;
	H = sc->i16 + 3 * N; C = H + (size_t)width * (size_t)m;
	memcpy(H, H0 + f0, (size_t)width * sizeof(int16_t));
	memcpy(C, C0 + f0, (size_t)width * sizeof(int16_t));
	memset(H + width, 0, (size_t)width * (size_t)(m - 1) * sizeof(int16_t));
	memset(C + width, 0, (size_t)width * (size_t)(m - 1) * sizeof(int16_t));

	for (int pi = 1; pi < m; pi++) {
		int f = F[pi];
		rune pc = pat[pi];
		int row = pi * width;
		bool gap = false;
		H[row + f - f0 - 1] = 0;                              /* Hleft[0] = 0 */
		for (int col = f; col <= lastIdx; col++) {
			int hleft = H[row + col - f0 - 1], hdiag = H[row - width + col - f0 - 1], cdiag = C[row - width + col - f0 - 1];
			int s1 = 0, s2 = hleft + (gap ? SCORE_GAP_EXT : SCORE_GAP_START), consecutive = 0;
			if (pc == (rune)T[col]) {
				s1 = hdiag + SCORE_MATCH;
				int b = B[col];
				consecutive = cdiag + 1;
				if (consecutive > 1) {
					int fb = B[col - consecutive + 1];
					if (b >= BONUS_BOUNDARY && b > fb) consecutive = 1;
					else { if (BONUS_CONSECUTIVE > b) b = BONUS_CONSECUTIVE; if (fb > b) b = fb; }
				}
				if (s1 + b < s2) { s1 += B[col]; consecutive = 0; }
				else s1 += b;
			}
			C[row + col - f0] = (int16_t)consecutive;
			gap = s1 < s2;
			int score = s1 > s2 ? s1 : s2;
			if (score < 0) score = 0;
			if (pi == m - 1 && ((fwd && score > maxScore) || (!fwd && score >= maxScore))) { maxScore = score; maxScorePos = col; }
			H[row + col - f0] = (int16_t)score;
		}
	}

	int j = f0;
	if (pos) {
		int i = m - 1, np = 0;
		j = maxScorePos;
		bool preferMatch = true;
		for (;;) {
			int I = i * width, j0 = j - f0, s = H[I + j0], s1 = 0, s2 = 0;
			if (i > 0 && j >= F[i]) s1 = H[I - width + j0 - 1];
			if (j > F[i]) s2 = H[I + j0 - 1];
			int row = i;
			if (s > s1 && (s > s2 || (s == s2 && preferMatch))) {
				pos[np++] = j + minIdx;
				if (i == 0) break;
				i--;
			}
			preferMatch = C[I + j0] > 1 || (row + 1 < m && j < lastIdx && j + 1 >= F[row + 1] && C[I + width + j0 + 1] > 0);
			j--;
		}
		if (npos) *npos = np;
	}
	return (MRes){minIdx + j, minIdx + maxScorePos + 1, maxScore};
}

static MRes exact_impl(bool cs, bool norm, bool fwd, bool boundary, Runes t, const rune *pat, int m, int *pos, int *npos)
{
	if (npos) *npos = 0;
	if (m == 0) return (MRes){0, 0, 0};
	int n = t.n;
	if (n < m) return NOMATCH;
	int lo, hi;
	if (!fuzzy_index(t, pat, m, cs, norm, &lo, &hi)) return NOMATCH;

	int pidx = 0, bestPos = -1, bonus = 0, bbonus = 0, bestBonus = -1;
	for (int index = 0; index < n; index++) {
		int idx_ = fwd ? index : n - index - 1;
		rune c = fold(t.r[idx_], cs, norm);
		int pidx_ = fwd ? pidx : m - pidx - 1;
		bool ok = pat[pidx_] == c;
		if (ok) {
			if (pidx_ == 0) bonus = bonus_at(t, idx_);
			if (boundary) {
				if (fwd && pidx_ == 0) bbonus = bonus;
				else if (!fwd && pidx_ == m - 1) bbonus = idx_ < n - 1 ? bonus_at(t, idx_ + 1) : bonus_white;
				ok = bbonus >= BONUS_BOUNDARY;
				if (ok && pidx_ == 0) ok = idx_ == 0 || class_of(t.r[idx_ - 1]) <= C_DELIM;
				if (ok && pidx_ == m - 1) ok = idx_ == n - 1 || class_of(t.r[idx_ + 1]) <= C_DELIM;
			}
		}
		if (ok) {
			pidx++;
			if (pidx == m) {
				if (bonus > bestBonus) { bestPos = index; bestBonus = bonus; }
				if (bonus >= BONUS_BOUNDARY) break;
				index -= pidx - 1;
				pidx = 0; bonus = 0;
			}
		} else {
			index -= pidx;
			pidx = 0; bonus = 0;
		}
	}
	if (bestPos < 0) return NOMATCH;
	int sidx, eidx;
	if (fwd) { sidx = bestPos - m + 1; eidx = bestPos + 1; }
	else { sidx = n - (bestPos + 1); eidx = n - (bestPos - m + 1); }
	int score;
	if (boundary) {
		score = bonus;
		int deduct = bonus - BONUS_BOUNDARY + 1;
		if (sidx > 0 && t.r[sidx - 1] == '_') { score -= deduct + 1; deduct = 1; }
		if (eidx < n && t.r[eidx] == '_') score -= deduct;
		score += SCORE_MATCH * m + bonus_white * (m + 1);
	} else score = calc_score(cs, norm, t, pat, m, sidx, eidx, NULL, NULL);
	(void)pos;
	return (MRes){sidx, eidx, score};
}
MRes exact_naive(bool cs, bool norm, bool fwd, Runes t, const rune *pat, int m, int *pos, int *npos, Scratch *sc) { (void)sc; return exact_impl(cs, norm, fwd, false, t, pat, m, pos, npos); }
MRes exact_boundary(bool cs, bool norm, bool fwd, Runes t, const rune *pat, int m, int *pos, int *npos, Scratch *sc) { (void)sc; return exact_impl(cs, norm, fwd, true, t, pat, m, pos, npos); }

int leading_spaces(Runes t) { int i = 0; while (i < t.n && rune_is_space(t.r[i])) i++; return i; }
int trailing_spaces(Runes t) { int i = 0; while (i < t.n && rune_is_space(t.r[t.n - 1 - i])) i++; return i; }

MRes prefix_match(bool cs, bool norm, bool fwd, Runes t, const rune *pat, int m, int *pos, int *npos, Scratch *sc)
{
	(void)fwd; (void)pos; (void)sc;
	if (npos) *npos = 0;
	if (m == 0) return (MRes){0, 0, 0};
	int trimmed = rune_is_space(pat[0]) ? 0 : leading_spaces(t);
	if (t.n - trimmed < m) return NOMATCH;
	for (int i = 0; i < m; i++)
		if (fold(t.r[trimmed + i], cs, norm) != pat[i]) return NOMATCH;
	return (MRes){trimmed, trimmed + m, calc_score(cs, norm, t, pat, m, trimmed, trimmed + m, NULL, NULL)};
}
MRes suffix_match(bool cs, bool norm, bool fwd, Runes t, const rune *pat, int m, int *pos, int *npos, Scratch *sc)
{
	(void)fwd; (void)pos; (void)sc;
	if (npos) *npos = 0;
	int trimmed = t.n;
	if (m == 0 || !rune_is_space(pat[m - 1])) trimmed -= trailing_spaces(t);
	if (m == 0) return (MRes){trimmed, trimmed, 0};
	int diff = trimmed - m;
	if (diff < 0) return NOMATCH;
	for (int i = 0; i < m; i++)
		if (fold(t.r[i + diff], cs, norm) != pat[i]) return NOMATCH;
	return (MRes){trimmed - m, trimmed, calc_score(cs, norm, t, pat, m, trimmed - m, trimmed, NULL, NULL)};
}
MRes equal_match(bool cs, bool norm, bool fwd, Runes t, const rune *pat, int m, int *pos, int *npos, Scratch *sc)
{
	(void)fwd; (void)pos; (void)sc;
	if (npos) *npos = 0;
	if (m == 0) return NOMATCH;
	int lead = rune_is_space(pat[0]) ? 0 : leading_spaces(t);
	int trail = rune_is_space(pat[m - 1]) ? 0 : trailing_spaces(t);
	if (t.n - lead - trail != m) return NOMATCH;
	for (int i = 0; i < m; i++)
		if (fold(t.r[lead + i], cs, norm) != pat[i]) return NOMATCH;
	return (MRes){lead, lead + m, (SCORE_MATCH + bonus_white) * m + (BONUS_FIRST_MULT - 1) * bonus_white};
}

/* ======== pattern.c ======== */
/* pattern.c - items, field selection (--nth / --with-nth / --delimiter), the extended-search query language,
 * ranking (score, tiebreaks) and the multi-threaded search driver. Semantics follow fzf's documented behaviour. */
#include <pthread.h>

/* ------------------------------------------------------------------ ranges and delimiters */
bool range_parse(const char *s, Range *r)
{
	size_t n = strlen(s);
	char *e;
	long a, b;
	if (!strcmp(s, "..")) { *r = (Range){0, 0}; return true; }
	if (n > 2 && s[0] == '.' && s[1] == '.') {
		b = strtol(s + 2, &e, 10);
		if (*e || b == 0) return false;
		*r = (Range){0, b == -1 ? 0 : (int)b};
		return true;
	}
	if (n > 2 && s[n - 2] == '.' && s[n - 1] == '.') {
		char *t = xstrndup(s, n - 2);
		a = strtol(t, &e, 10);
		bool bad = *e || a == 0;
		free(t);
		if (bad) return false;
		*r = (Range){(int)a, 0};
		if (a == 1) r->begin = 0;
		return true;
	}
	const char *dd = strstr(s, "..");
	if (dd) {
		char *t = xstrndup(s, (size_t)(dd - s));
		a = strtol(t, &e, 10);
		bool bad = *e || a == 0;
		free(t);
		b = strtol(dd + 2, &e, 10);
		if (bad || *e || b == 0 || dd[2] == 0 || (a < 0 && b > 0)) return false;
		*r = (Range){(int)a, (int)b};
		if (r->begin == 1 && r->end != 1) r->begin = 0;
		if (r->end == -1) r->end = 0;
		return true;
	}
	a = strtol(s, &e, 10);
	if (*e || a == 0 || n == 0) return false;
	*r = (Range){(int)a, (int)a};
	if (a == 1) { r->begin = 1; r->end = 1; }
	if (a == -1) { r->begin = -1; r->end = 0; r->end = -1; }
	return true;
}

int ranges_parse(const char *spec, Range **out)
{
	int n = 0, cap = 4;
	Range *v = xmalloc((size_t)cap * sizeof(Range));
	char *copy = xstrdup(spec), *save = NULL;
	for (char *t = strtok_r(copy, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
		if (n == cap) { cap *= 2; v = xrealloc(v, (size_t)cap * sizeof(Range)); }
		if (!range_parse(t, &v[n])) { free(copy); free(v); return -1; }
		n++;
	}
	free(copy);
	*out = v;
	return n;
}

void delim_set(Delim *d, const char *s0)
{
	Buf b;
	buf_init(&b);
	for (const char *p = s0; *p; p++) {                       /* \t as a tab */
		if (p[0] == '\\' && p[1] == 't') { buf_addc(&b, '\t'); p++; }
		else buf_addc(&b, *p);
	}
	char *s = buf_take(&b);
	memset(d, 0, sizeof *d);
	bool special = strpbrk(s, "\\.+*?()|[]{}^$") != NULL;
	if (utf8_len(s, strlen(s)) == 1 || !special) { d->str = s; d->len = strlen(s); return; }
	if (regcomp(&d->re, s, REG_EXTENDED) == 0) { d->has_re = true; free(s); return; }
	d->str = s; d->len = strlen(s);
}

/* ------------------------------------------------------------------ tokens (rune ranges of an item) */
typedef struct { int start, end; } Tok;

static int byte_to_rune(const char *s, size_t byte_off)
{
	return (int)utf8_len(s, byte_off);
}

/* text as bytes s[0..n) and runes r[0..nr): tokens partition the text, each token keeps its trailing delimiter */
static int tokenize(const char *s, size_t n, const rune *r, int nr, const Delim *d, Tok **out)
{
	int cap = 8, nt = 0;
	Tok *t = xmalloc((size_t)cap * sizeof(Tok));
#define PUSH(a, b) do { if (nt == cap) { cap *= 2; t = xrealloc(t, (size_t)cap * sizeof(Tok)); } t[nt].start = (a); t[nt].end = (b); nt++; } while (0)
	if (!d->str && !d->has_re) {                                    /* awk: runs of blanks separate fields, kept as a suffix */
		int state = 0, begin = 0, end = 0;
		for (int i = 0; i < nr; i++) {
			bool white = r[i] == 9 || r[i] == 32 || r[i] == 10;
			if (state == 0) { if (!white) { state = 1; begin = i; end = i + 1; } }
			else if (state == 1) { end = i + 1; if (white) state = 2; }
			else if (white) end = i + 1;
			else { PUSH(begin, end); state = 1; begin = i; end = i + 1; }
		}
		if (begin < end) PUSH(begin, end);
	} else if (d->str) {
		size_t pos = 0, tokstart = 0;
		while (pos + d->len <= n && d->len) {
			if (!memcmp(s + pos, d->str, d->len)) { PUSH(byte_to_rune(s, tokstart), byte_to_rune(s, pos + d->len)); pos += d->len; tokstart = pos; }
			else pos++;
		}
		PUSH(byte_to_rune(s, tokstart), nr);
	} else {
		char *tmp = xstrndup(s, n);
		regmatch_t m;
		size_t off = 0, begin = 0;
		while (off <= n && regexec(&d->re, tmp + off, 1, &m, off ? REG_NOTBOL : 0) == 0) {
			size_t me = off + (size_t)m.rm_eo;
			if (m.rm_eo == m.rm_so) { off++; continue; }
			PUSH(byte_to_rune(s, begin), byte_to_rune(s, me));
			begin = off = me;
		}
		if (begin < n) PUSH(byte_to_rune(s, begin), nr);
		free(tmp);
	}
#undef PUSH
	*out = t;
	return nt;
}

/* Transform: one merged token per range */
static int transform(const Tok *tk, int ntk, const Range *rg, int nrg, Tok *out)
{
	for (int i = 0; i < nrg; i++) {
		Range r = rg[i];
		int b, e;
		if (r.begin == r.end && r.begin != 0) {
			int k = r.begin < 0 ? r.begin + ntk + 1 : r.begin;
			if (k >= 1 && k <= ntk) { out[i] = tk[k - 1]; continue; }
			out[i] = (Tok){0, 0};
			continue;
		}
		if (r.begin == 0 && r.end == 0) {
			out[i] = ntk ? (Tok){tk[0].start, tk[ntk - 1].end} : (Tok){0, 0};
			continue;
		}
		if (r.begin == 0) { b = 1; e = r.end < 0 ? r.end + ntk + 1 : r.end; }
		else if (r.end == 0) { b = r.begin < 0 ? r.begin + ntk + 1 : r.begin; e = ntk; }
		else { b = r.begin < 0 ? r.begin + ntk + 1 : r.begin; e = r.end < 0 ? r.end + ntk + 1 : r.end; }
		int first = -1, last = -1;
		for (int k = b; k <= e; k++)
			if (k >= 1 && k <= ntk) { if (first < 0) first = k - 1; last = k - 1; }
		out[i] = first < 0 ? (Tok){0, 0} : (Tok){tk[first].start, tk[last].end};
	}
	return nrg;
}

/* ------------------------------------------------------------------ items */
Item *item_new(const Opts *o, const char *line, size_t len)
{
	Item *it = calloc(1, sizeof *it);
	if (!it) die("out of memory");
	it->orig = xstrndup(line, len);
	it->olen = (uint32_t)len;
	Buf plain;
	buf_init(&plain);
	const char *src = line;
	size_t srclen = len;
	Buf joined;
	buf_init(&joined);
	if (o->with_nth && o->nwith > 0) {                             /* the shown text is made of the chosen fields */
		rune *rb = xmalloc((len + 1) * sizeof(rune));
		int nr = (int)utf8_to_runes(line, len, rb);
		Tok *tk;
		int ntk = tokenize(line, len, rb, nr, &o->delim, &tk);
		Tok *tr = xmalloc((size_t)o->nwith * sizeof(Tok));
		transform(tk, ntk, o->with_nth, o->nwith, tr);
		for (int i = 0; i < o->nwith; i++)
			for (int k = tr[i].start; k < tr[i].end; k++) { char e[4]; buf_addn(&joined, e, u8enc(rb[k], e)); }
		free(tr); free(tk); free(rb);
		src = joined.p ? joined.p : ""; srclen = joined.len;
	}
	uint32_t cap = 0;
	if (o->ansi) ansi_extract(src, srclen, &plain, &it->ansi, &it->nansi, &cap);
	else buf_addn(&plain, src, srclen);
	if (o->with_nth && o->nwith > 0) {                             /* trailing blanks are not part of a transformed line */
		while (plain.len && (plain.p[plain.len - 1] == ' ' || plain.p[plain.len - 1] == '\t')) plain.len--;
		if (plain.p) plain.p[plain.len] = 0;
	}
	it->tlen = (uint32_t)plain.len;
	it->text = buf_take(&plain);
	buf_free(&joined);
	it->nrunes = (uint32_t)utf8_len(it->text, it->tlen);
	/* trimmed length in runes */
	rune *rb = xmalloc((it->tlen + 1) * sizeof(rune));
	int n = (int)utf8_to_runes(it->text, it->tlen, rb);
	int a = 0, b = n;
	while (a < b && rune_is_space(rb[a])) a++;
	while (b > a && rune_is_space(rb[b - 1])) b--;
	it->trim = (uint16_t)(b - a > 65535 ? 65535 : b - a);
	free(rb);
	return it;
}
void item_free(Item *it)
{
	if (!it) return;
	free(it->orig); free(it->text); free(it->ansi); free(it->id); free(it);
}

/* ------------------------------------------------------------------ query -> pattern */
enum { T_FUZZY, T_EXACT, T_BOUNDARY, T_PREFIX, T_SUFFIX, T_EQUAL };
typedef struct { int typ; bool inv, cs, norm; rune *t; int n; } Term;
typedef struct { Term *v; int n; } TermSet;

struct Pattern {
	bool extended, fuzzy, cs, norm, fwd, sortable, v1;
	rune *text; int ntext;
	TermSet *sets; int nsets;
	const Opts *o;
	Crit crit[5]; int ncrit;
	Tok *nth_dummy;
};

static bool has_upper(const rune *r, int n) { for (int i = 0; i < n; i++) if (rune_lower(r[i]) != r[i]) return true; return false; }

static void set_push(TermSet *s, Term t) { s->v = xrealloc(s->v, (size_t)(s->n + 1) * sizeof(Term)); s->v[s->n++] = t; }

static rune *dup_runes(const char *s, size_t n, int *out)
{
	rune *r = xmalloc((n + 1) * sizeof(rune));
	*out = (int)utf8_to_runes(s, n, r);
	return r;
}

/* splits on runs of blanks, "\ " keeps a blank inside a term */
static char **split_terms(const char *q, int *nt)
{
	int cap = 8;
	char **v = xmalloc((size_t)cap * sizeof(char *));
	*nt = 0;
	Buf cur;
	buf_init(&cur);
	bool have = false;
	for (const char *p = q; ; p++) {
		if (*p == '\\' && p[1] == ' ') { buf_addc(&cur, '\t'); have = true; p++; continue; }
		if (*p == ' ' || *p == 0) {
			if (have) {
				if (*nt == cap) { cap *= 2; v = xrealloc(v, (size_t)cap * sizeof(char *)); }
				for (size_t i = 0; i < cur.len; i++) if (cur.p[i] == '\t') cur.p[i] = ' ';
				v[(*nt)++] = buf_take(&cur);
				have = false;
			}
			if (!*p) break;
		} else { buf_addc(&cur, *p); have = true; }
	}
	buf_free(&cur);
	return v;
}

Pattern *pattern_new(const Opts *o, const char *query)
{
	Pattern *p = calloc(1, sizeof *p);
	if (!p) die("out of memory");
	p->o = o;
	p->extended = o->extended;
	p->fuzzy = o->fuzzy;
	p->fwd = o->forward;
	p->v1 = o->algo_v1;
	p->norm = o->normalize;
	memcpy(p->crit, o->crit, sizeof p->crit);
	p->ncrit = o->ncrit;
	char *q = xstrdup(query);
	if (!p->extended) {
		char *lower = q;
		int n;
		rune *rn = dup_runes(q, strlen(q), &n);
		bool up = has_upper(rn, n);
		p->cs = o->case_mode == CASE_RESPECT || (o->case_mode == CASE_SMART && up);
		if (!p->cs) for (int i = 0; i < n; i++) rn[i] = rune_lower(rn[i]);
		if (p->norm) { rune *cp = xmalloc((size_t)(n + 1) * sizeof(rune)); memcpy(cp, rn, (size_t)n * sizeof(rune)); normalize_runes(cp, n); if (memcmp(cp, rn, (size_t)n * sizeof(rune))) p->norm = false; free(cp); }
		if (p->norm) normalize_runes(rn, n);
		p->text = rn; p->ntext = n;
		p->sortable = true;
		(void)lower;
		free(q);
		return p;
	}
	/* trim the query: leading blanks and trailing ones (unless escaped) do not count */
	char *s = q;
	while (*s == ' ') s++;
	size_t ln = strlen(s);
	while (ln > 0 && s[ln - 1] == ' ' && !(ln > 1 && s[ln - 2] == '\\')) ln--;
	s[ln] = 0;
	int nt;
	char **tok = split_terms(s, &nt);
	TermSet set = {NULL, 0};
	bool switch_set = false, after_bar = false;
	p->sortable = false;
	for (int i = 0; i < nt; i++) {
		char *t = tok[i];
		int typ = p->fuzzy ? T_FUZZY : T_EXACT;
		bool inv = false;
		int n0;
		rune *r0 = dup_runes(t, strlen(t), &n0);
		bool up = has_upper(r0, n0);
		free(r0);
		bool cs = o->case_mode == CASE_RESPECT || (o->case_mode == CASE_SMART && up);
		if (!cs) { for (char *c = t; *c; c++) if ((unsigned char)*c < 128) *c = (char)tolower((unsigned char)*c); }
		char *text = t;
		if (!cs) {   /* non-ASCII lowercase through runes */
			int n1; rune *r1 = dup_runes(t, strlen(t), &n1);
			for (int k = 0; k < n1; k++) r1[k] = rune_lower(r1[k]);
			Buf b; buf_init(&b);
			for (int k = 0; k < n1; k++) { char e[4]; buf_addn(&b, e, u8enc(r1[k], e)); }
			free(r1); free(t); t = tok[i] = buf_take(&b); text = t;
		}
		if (set.n > 0 && !after_bar && !strcmp(text, "|")) { switch_set = false; after_bar = true; continue; }
		after_bar = false;
		if (text[0] == '!') { inv = true; typ = T_EXACT; text++; }
		size_t tl = strlen(text);
		char *work = xstrndup(text, tl);
		char *w = work;
		size_t wl = tl;
		if (strcmp(w, "$") && wl > 0 && w[wl - 1] == '$') { typ = T_SUFFIX; w[--wl] = 0; }
		if (wl > 2 && w[0] == '\'' && w[wl - 1] == '\'') { typ = T_BOUNDARY; w[wl - 1] = 0; w++; wl -= 2; }
		else if (w[0] == '\'') { typ = (p->fuzzy && !inv) ? T_EXACT : T_FUZZY; w++; wl--; }
		else if (w[0] == '^') { typ = typ == T_SUFFIX ? T_EQUAL : T_PREFIX; w++; wl--; }
		if (wl > 0) {
			if (switch_set) { p->sets = xrealloc(p->sets, (size_t)(p->nsets + 1) * sizeof(TermSet)); p->sets[p->nsets++] = set; set = (TermSet){NULL, 0}; }
			Term tm;
			tm.typ = typ; tm.inv = inv; tm.cs = cs;
			tm.t = dup_runes(w, wl, &tm.n);
			tm.norm = false;
			if (o->normalize) {
				rune *cp = xmalloc((size_t)(tm.n + 1) * sizeof(rune)); memcpy(cp, tm.t, (size_t)tm.n * sizeof(rune)); normalize_runes(cp, tm.n);
				tm.norm = memcmp(cp, tm.t, (size_t)tm.n * sizeof(rune)) == 0;   /* a term with accents asks for the accents */
				free(cp);
				if (tm.norm) normalize_runes(tm.t, tm.n);
			}
			set_push(&set, tm);
			switch_set = true;
			if (!inv) p->sortable = true;
		}
		free(work);
	}
	if (set.n > 0) { p->sets = xrealloc(p->sets, (size_t)(p->nsets + 1) * sizeof(TermSet)); p->sets[p->nsets++] = set; }
	for (int i = 0; i < nt; i++) free(tok[i]);
	free(tok);
	free(q);
	return p;
}

void pattern_free(Pattern *p)
{
	if (!p) return;
	for (int i = 0; i < p->nsets; i++) { for (int j = 0; j < p->sets[i].n; j++) free(p->sets[i].v[j].t); free(p->sets[i].v); }
	free(p->sets); free(p->text); free(p);
}
bool pattern_empty(const Pattern *p) { return p->extended ? p->nsets == 0 : p->ntext == 0; }
bool pattern_sortable(const Pattern *p) { return p->sortable; }

/* ------------------------------------------------------------------ matching one item */
static AlgoFn fn_of(const Pattern *p, int typ)
{
	switch (typ) {
	case T_FUZZY: return p->v1 ? fuzzy_v1 : fuzzy_v2;
	case T_EXACT: return exact_naive;
	case T_BOUNDARY: return exact_boundary;
	case T_PREFIX: return prefix_match;
	case T_SUFFIX: return suffix_match;
	default: return equal_match;
	}
}

static uint16_t clamp16(int v) { return (uint16_t)(v < 0 ? 0 : v > 65535 ? 65535 : v); }

/* the fields to match on: the whole text, or the --nth ranges of it (last field without its trailing delimiter) */
static int input_tokens(const Pattern *p, Item *it, Runes t, Tok *out, int maxout)
{
	const Opts *o = p->o;
	if (o->nnth == 0 || o->nth == NULL) { out[0] = (Tok){0, t.n}; return 1; }
	Tok *tk;
	int ntk = tokenize(it->text, it->tlen, t.r, t.n, &o->delim, &tk);
	int n = o->nnth < maxout ? o->nnth : maxout;
	transform(tk, ntk, o->nth, n, out);
	free(tk);
	if (n > 0 && (o->delim.str || o->delim.has_re)) {                  /* strip the last delimiter to allow suffix matches */
		Tok *last = &out[n - 1];
		if (o->delim.str) {
			size_t dl = utf8_len(o->delim.str, o->delim.len);
			if (last->end - last->start >= (int)dl) {
				bool same = true;
				rune *dr = xmalloc((o->delim.len + 1) * sizeof(rune));
				utf8_to_runes(o->delim.str, o->delim.len, dr);
				for (size_t k = 0; k < dl; k++) if (t.r[last->end - (int)dl + (int)k] != dr[k]) same = false;
				free(dr);
				if (same) last->end -= (int)dl;
			}
		}
	}
	return n;
}

static bool iter_tokens(AlgoFn fn, bool cs, bool norm, bool fwd, Runes t, const Tok *tk, int ntk, const rune *pat, int m,
                        bool want_pos, Scratch *sc, int *bs, int *be, int *score, int *pos, int *npos)
{
	for (int i = 0; i < ntk; i++) {
		Runes sub = {t.r + tk[i].start, tk[i].end - tk[i].start};
		int np = 0;
		MRes r = fn(cs, norm, fwd, sub, pat, m, want_pos ? pos : NULL, &np, sc);
		if (r.start >= 0) {
			*bs = r.start + tk[i].start; *be = r.end + tk[i].start; *score = r.score;
			if (want_pos) {
				if (np == 0) { for (int k = *bs; k < *be; k++) pos[np++] = k; }
				else for (int k = 0; k < np; k++) pos[k] += tk[i].start;
			}
			*npos = np;
			return true;
		}
	}
	return false;
}

bool pattern_match(const Pattern *p, Item *it, Scratch *sc, bool want_pos, MatchOut *out)
{
	rune *rb = scratch_rbuf(sc, it->tlen + 1);
	int nr = (int)utf8_to_runes(it->text, it->tlen, rb);
	Runes t = {rb, nr};
	Tok tk[64];
	int ntk = input_tokens(p, it, t, tk, 64);
	int score = 0, minb = 65535, mine = 65535, maxe = 0;
	bool valid = false;
	int npos_total = 0;
	int *posbuf = want_pos ? scratch_ibuf(sc, 0, (size_t)nr + 8) : NULL;
	int *allpos = want_pos ? scratch_ibuf(sc, 1, ((size_t)nr + 8) * 2 + 16) : NULL;
	bool ok = true;
	if (!p->extended) {
		int bs, be, sc1, np = 0;
		AlgoFn fn = p->fuzzy ? fn_of(p, T_FUZZY) : exact_naive;
		if (!iter_tokens(fn, p->cs, p->norm, p->fwd, t, tk, ntk, p->text, p->ntext, want_pos, sc, &bs, &be, &sc1, posbuf, &np)) ok = false;
		else {
			score = sc1;
			if (bs < be) { minb = bs; mine = be; maxe = be; valid = true; }
			if (want_pos) for (int k = 0; k < np; k++) allpos[npos_total++] = posbuf[k];
		}
	} else {
		int matched_sets = 0;
		int *offs = xmalloc((size_t)(p->nsets + 1) * 2 * sizeof(int));
		for (int si = 0; si < p->nsets && ok; si++) {
			bool matched = false;
			int cur_score = 0, os = 0, oe = 0;
			const TermSet *ts = &p->sets[si];
			for (int ti = 0; ti < ts->n; ti++) {
				const Term *tm = &ts->v[ti];
				int bs, be, sc1, np = 0;
				bool hit = iter_tokens(fn_of(p, tm->typ), tm->cs, tm->norm, p->fwd, t, tk, ntk, tm->t, tm->n, want_pos, sc, &bs, &be, &sc1, posbuf, &np);
				if (hit) {
					if (tm->inv) continue;
					os = bs; oe = be; cur_score = sc1; matched = true;
					if (want_pos) for (int k = 0; k < np; k++) allpos[npos_total++] = posbuf[k];
					break;
				} else if (tm->inv) { os = oe = 0; cur_score = 0; matched = true; continue; }
			}
			if (matched) { offs[2 * matched_sets] = os; offs[2 * matched_sets + 1] = oe; matched_sets++; score += cur_score; }
			else ok = false;
		}
		if (ok && matched_sets == p->nsets) {
			for (int i = 0; i < matched_sets; i++) {
				int b = offs[2 * i], e = offs[2 * i + 1];
				if (b < e) { if (b < minb) minb = b; if (e < mine) mine = e; if (e > maxe) maxe = e; valid = true; }
			}
		} else ok = false;
		free(offs);
	}
	if (!ok) return false;

	/* rank: criterion i -> pts[3 - i], smaller is better */
	memset(out->pts, 0, sizeof out->pts);
	for (int idx = 0; idx < p->ncrit; idx++) {
		uint16_t val = 65535;
		switch (p->crit[idx]) {
		case CR_SCORE: val = (uint16_t)(65535 - clamp16(score)); break;
		case CR_LENGTH: val = it->trim; break;
		case CR_CHUNK:
			if (valid) {
				int b = minb, e = maxe;
				for (; b >= 1; b--) if (rune_is_space(rb[b - 1])) break;
				for (; e < nr; e++) if (rune_is_space(rb[e])) break;
				val = clamp16(e - b);
			}
			break;
		case CR_PATHNAME:
			if (valid) {
				int last = -1;
				for (int i = nr - 1; i >= 0; i--) if (rb[i] == '/' || rb[i] == '\\') { last = i; break; }
				if (last <= minb) val = clamp16(minb - last);
			}
			break;
		case CR_BEGIN: case CR_END:
			if (valid) {
				int wp = 0;
				for (int i = 0; i < nr; i++) { wp = i; if (i == minb || !rune_is_space(rb[i])) break; }
				if (p->crit[idx] == CR_BEGIN) val = clamp16(mine - wp);
				else val = clamp16(65535 - 65535 * (maxe - wp) / ((int)it->trim + 1));
			}
			break;
		}
		out->pts[3 - idx] = val;
	}
	out->minb = valid ? minb : -1; out->mine = valid ? mine : -1; out->maxe = maxe;
	if (want_pos) { out->pos = allpos; out->npos = npos_total; } else { out->pos = NULL; out->npos = 0; }   /* points into the scratch: copy it if it must outlive the next call */
	return true;
}

/* ------------------------------------------------------------------ search driver */
typedef struct { const Pattern *p; Item **items; size_t lo, hi; Hit *hits; size_t nhits; bool need_pos; } Job;

static uint64_t key_of(const MatchOut *m)
{
	return (uint64_t)m->pts[0] | ((uint64_t)m->pts[1] << 16) | ((uint64_t)m->pts[2] << 32) | ((uint64_t)m->pts[3] << 48);
}

static void *worker(void *arg)
{
	Job *j = arg;
	Scratch *sc = scratch_new();
	j->hits = xmalloc((j->hi - j->lo + 1) * sizeof(Hit));
	for (size_t i = j->lo; i < j->hi; i++) {
		MatchOut m;
		if (pattern_match(j->p, j->items[i], sc, j->need_pos, &m)) j->hits[j->nhits++] = (Hit){(uint32_t)i, key_of(&m)};
	}
	scratch_free(sc);
	return NULL;
}

static int cmp_hits(const void *a, const void *b)
{
	const Hit *x = a, *y = b;
	if (x->key != y->key) return x->key < y->key ? -1 : 1;
	return x->idx < y->idx ? -1 : x->idx > y->idx;
}
static int cmp_hits_tac(const void *a, const void *b)
{
	const Hit *x = a, *y = b;
	if (x->key != y->key) return x->key < y->key ? -1 : 1;
	return x->idx > y->idx ? -1 : x->idx < y->idx;
}

Hit *search_items(const Opts *o, Pattern *p, Item **items, size_t n, size_t *nhits)
{
	bool need_pos = false;              /* the chunk and pathname tiebreaks need the exact match positions */
	for (int i = 1; i < o->ncrit; i++) if (o->crit[i] == CR_CHUNK || o->crit[i] == CR_PATHNAME) need_pos = true;
	long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
	int nt = ncpu < 1 ? 1 : ncpu > 16 ? 16 : (int)ncpu;
	if (n < 2000) nt = 1;
	Job *jobs = xmalloc((size_t)nt * sizeof(Job));
	pthread_t *th = xmalloc((size_t)nt * sizeof(pthread_t));
	size_t per = (n + (size_t)nt - 1) / (size_t)nt;
	for (int i = 0; i < nt; i++) {
		jobs[i] = (Job){p, items, (size_t)i * per, (size_t)(i + 1) * per < n ? (size_t)(i + 1) * per : n, NULL, 0, need_pos};
		if (jobs[i].lo > n) jobs[i].lo = jobs[i].hi = n;
	}
	if (nt == 1) worker(&jobs[0]);
	else {
		for (int i = 0; i < nt; i++) pthread_create(&th[i], NULL, worker, &jobs[i]);
		for (int i = 0; i < nt; i++) pthread_join(th[i], NULL);
	}
	size_t total = 0;
	for (int i = 0; i < nt; i++) total += jobs[i].nhits;
	Hit *all = xmalloc((total + 1) * sizeof(Hit));
	size_t k = 0;
	for (int i = 0; i < nt; i++) { memcpy(all + k, jobs[i].hits, jobs[i].nhits * sizeof(Hit)); k += jobs[i].nhits; free(jobs[i].hits); }
	free(jobs); free(th);
	bool sorted = o->sort && !pattern_empty(p) && pattern_sortable(p);
	if (sorted) qsort(all, total, sizeof(Hit), o->tac ? cmp_hits_tac : cmp_hits);
	else if (o->tac) for (size_t i = 0; i < total / 2; i++) { Hit t = all[i]; all[i] = all[total - 1 - i]; all[total - 1 - i] = t; }
	*nhits = total;
	return all;
}

/* ======== opts.c ======== */
/* opts.c - command line and FZF_DEFAULT_OPTS. One table describes every option: its long name, an optional short
 * form, whether it takes a value, and the setter. --no-NAME / +X undo a boolean option. */

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

/* ======== ansi.c ======== */
/* ansi.c - takes the colour escapes out of a line (--ansi) and remembers which byte range had which colour. */

typedef struct { int fg, bg, attr; } St;
#define DEF (-1)
/* attribute bits, same numbering as term.c uses */
#define A_BOLD 1
#define A_DIM 2
#define A_ITAL 4
#define A_UL 8
#define A_BLINK 16
#define A_REV 32
#define A_STRIKE 64

static void sgr(St *s, const int *v, int n)
{
	if (n == 0) { s->fg = s->bg = DEF; s->attr = 0; return; }
	for (int i = 0; i < n; i++) {
		int c = v[i];
		if (c == 0) { s->fg = s->bg = DEF; s->attr = 0; }
		else if (c == 1) s->attr |= A_BOLD;
		else if (c == 2) s->attr |= A_DIM;
		else if (c == 3) s->attr |= A_ITAL;
		else if (c == 4) s->attr |= A_UL;
		else if (c == 5) s->attr |= A_BLINK;
		else if (c == 7) s->attr |= A_REV;
		else if (c == 9) s->attr |= A_STRIKE;
		else if (c == 22) s->attr &= ~(A_BOLD | A_DIM);
		else if (c == 23) s->attr &= ~A_ITAL;
		else if (c == 24) s->attr &= ~A_UL;
		else if (c == 25) s->attr &= ~A_BLINK;
		else if (c == 27) s->attr &= ~A_REV;
		else if (c == 29) s->attr &= ~A_STRIKE;
		else if (c >= 30 && c <= 37) s->fg = c - 30;
		else if (c >= 90 && c <= 97) s->fg = c - 90 + 8;
		else if (c >= 40 && c <= 47) s->bg = c - 40;
		else if (c >= 100 && c <= 107) s->bg = c - 100 + 8;
		else if (c == 39) s->fg = DEF;
		else if (c == 49) s->bg = DEF;
		else if (c == 38 || c == 48) {
			int *tgt = c == 38 ? &s->fg : &s->bg;
			if (i + 2 < n && v[i + 1] == 5) { *tgt = v[i + 2] & 255; i += 2; }
			else if (i + 4 < n && v[i + 1] == 2) { *tgt = 0x1000000 | ((v[i + 2] & 255) << 16) | ((v[i + 3] & 255) << 8) | (v[i + 4] & 255); i += 4; }
		}
	}
}

/* Copies line to out without escape sequences. When spans is not NULL, coloured ranges (byte offsets into the
 * stripped text) are appended to it. Returns true if the line held any escape. */
bool ansi_extract(const char *s, size_t n, Buf *out, AnsiSpan **spans, uint32_t *nspans, uint32_t *cap)
{
	St st = {DEF, DEF, 0}, open = {DEF, DEF, 0};
	size_t i = 0, span_start = 0;
	bool found = false, in_span = false;
	while (i < n) {
		if (s[i] == 033 && i + 1 < n) {
			found = true;
			if (s[i + 1] == '[') {
				size_t j = i + 2;
				int v[32], nv = 0, cur = 0;
				bool have = false;
				while (j < n && ((s[j] >= '0' && s[j] <= '9') || s[j] == ';' || s[j] == ':')) {
					if (s[j] == ';' || s[j] == ':') { if (nv < 32) v[nv++] = have ? cur : 0; cur = 0; have = false; }
					else { cur = cur * 10 + (s[j] - '0'); have = true; }
					j++;
				}
				if (have || nv) { if (nv < 32) v[nv++] = have ? cur : 0; }
				while (j < n && s[j] >= 0x20 && s[j] <= 0x2F) j++;
				if (j < n && s[j] == 'm') {
					St old = st;
					sgr(&st, v, nv);
					if (spans && (old.fg != st.fg || old.bg != st.bg || old.attr != st.attr)) {
						if (in_span && out->len > span_start) {
							if (*nspans >= *cap) { *cap = *cap ? *cap * 2 : 4; *spans = xrealloc(*spans, *cap * sizeof(AnsiSpan)); }
							(*spans)[(*nspans)++] = (AnsiSpan){(uint32_t)span_start, (uint32_t)(out->len - span_start), (uint32_t)open.fg, (uint32_t)open.bg, (uint16_t)open.attr};
						}
						open = st; span_start = out->len;
						in_span = st.fg != DEF || st.bg != DEF || st.attr;
					}
				}
				i = j < n ? j + 1 : n;
			} else if (s[i + 1] == ']') {             /* OSC ... BEL or ST */
				size_t j = i + 2;
				while (j < n && s[j] != 7 && !(s[j] == 033 && j + 1 < n && s[j + 1] == '\\')) j++;
				i = j < n ? (s[j] == 7 ? j + 1 : j + 2) : n;
			} else i += 2;
			continue;
		}
		buf_addc(out, s[i++]);
	}
	if (spans && in_span && out->len > span_start) {
		if (*nspans >= *cap) { *cap = *cap ? *cap * 2 : 4; *spans = xrealloc(*spans, *cap * sizeof(AnsiSpan)); }
		(*spans)[(*nspans)++] = (AnsiSpan){(uint32_t)span_start, (uint32_t)(out->len - span_start), (uint32_t)open.fg, (uint32_t)open.bg, (uint16_t)open.attr};
	}
	return found;
}

/* ======== term.c ======== */
/* term.c - raw terminal, key and mouse decoding (xterm SGR mouse, bracketed paste, CSI/SS3 keys, alt-prefix). */
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>

static int tty = -1;
static struct termios saved;
static bool is_raw, use_alt, use_mouse, mouse_on;
static volatile sig_atomic_t winch;
static unsigned char in[65536];
static size_t in_len, in_pos;
static Buf outb;
static bool kitty_wanted, kitty_pushed, kitty_ok;
static void kitty_pop(void);

static void on_winch(int s) { (void)s; winch = 1; }
bool term_resized(void) { bool r = winch; winch = 0; return r; }
int term_fd(void) { return tty; }

void term_out(const char *s, size_t n) { buf_addn(&outb, s, n); }
void term_outs(const char *s) { buf_adds(&outb, s); }
void term_printf(const char *fmt, ...)
{
	va_list ap;
	char tmp[512];
	va_start(ap, fmt);
	int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
	va_end(ap);
	if (n >= (int)sizeof tmp) {
		char *big = xmalloc((size_t)n + 1);
		va_start(ap, fmt);
		vsnprintf(big, (size_t)n + 1, fmt, ap);
		va_end(ap);
		buf_addn(&outb, big, (size_t)n);
		free(big);
	} else if (n > 0) buf_addn(&outb, tmp, (size_t)n);
}
void term_flush(void)
{
	size_t off = 0;
	while (off < outb.len) {
		ssize_t w = write(tty, outb.p + off, outb.len - off);
		if (w < 0) { if (errno == EINTR || errno == EAGAIN) continue; break; }
		off += (size_t)w;
	}
	buf_reset(&outb);
}

bool term_open(bool alt, bool mouse)
{
	tty = open("/dev/tty", O_RDWR | O_CLOEXEC);
	if (tty < 0) return false;
	if (tcgetattr(tty, &saved) < 0) return false;
	struct termios t = saved;
	t.c_iflag &= ~(unsigned)(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
	t.c_oflag &= ~(unsigned)OPOST;
	t.c_lflag &= ~(unsigned)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
	t.c_cflag |= CS8;
	t.c_cc[VMIN] = 1;
	t.c_cc[VTIME] = 0;
	tcsetattr(tty, TCSANOW, &t);
	is_raw = true;
	use_alt = alt; use_mouse = mouse;
	struct sigaction sa = {0};
	sa.sa_handler = on_winch;
	sigaction(SIGWINCH, &sa, NULL);
	if (alt) term_outs("\033[?1049h");
	term_outs("\033[?7l");                          /* no autowrap: we place every cell */
	term_outs("\033[?2004h");                       /* bracketed paste */
	term_set_mouse(mouse);
	term_flush();
	if (kitty_wanted) term_kitty(true);
	return true;
}

void term_set_mouse(bool on)
{
	if (on == mouse_on || !use_mouse) return;
	mouse_on = on;
	term_outs(on ? "\033[?1000h\033[?1002h\033[?1006h" : "\033[?1006l\033[?1002l\033[?1000l");
}

void term_close(void)
{
	if (!is_raw) return;
	kitty_pop();
	term_set_mouse(false);
	term_outs("\033[?2004l\033[?7h\033[?25h\033[0m");
	if (use_alt) term_outs("\033[?1049l");
	term_flush();
	tcsetattr(tty, TCSANOW, &saved);
	is_raw = false;
}

void term_size(int *rows, int *cols)
{
	struct winsize ws;
	if (ioctl(tty, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) { *rows = ws.ws_row; *cols = ws.ws_col; }
	else { *rows = 24; *cols = 80; }
}

/* ---- decoding ---- */
static int fill(int timeout_ms)
{
	if (in_pos > 0 && in_pos == in_len) in_pos = in_len = 0;
	if (in_len == sizeof in) return 0;
	struct pollfd p = {tty, POLLIN, 0};
	if (poll(&p, 1, timeout_ms) <= 0) return 0;
	ssize_t n = read(tty, in + in_len, sizeof in - in_len);
	if (n <= 0) return 0;
	in_len += (size_t)n;
	return 1;
}
bool term_pending(void) { return in_pos < in_len; }

static int csi_mods(int m)   /* xterm modifier parameter (1 + bits) -> our flags */
{
	int r = 0;
	m = m > 0 ? m - 1 : 0;
	if (m & 1) r |= M_SHIFT;
	if (m & 2) r |= M_ALT;
	if (m & 4) r |= M_CTRL;
	return r;
}

/* ---- kitty keyboard protocol: modifier keys held, release events (only with --hints) ---- */
static int held;                        /* M_CTRL | M_ALT | M_SHIFT currently held */
static bool held_dirty;
bool term_kitty_active(void) { return kitty_pushed; }
int term_mods(void) { return held; }
bool term_mods_dirty(void) { bool d = held_dirty; held_dirty = false; return d; }
static void set_held(int m) { if (m != held) { held = m; held_dirty = true; } }

static void kitty_push(void)
{
	if (kitty_ok && !kitty_pushed) { term_outs("\033[>27u\033[?1004h"); term_flush(); kitty_pushed = true; }
}
static void kitty_pop(void)
{
	if (kitty_pushed) { term_outs("\033[?1004l\033[<u"); term_flush(); kitty_pushed = false; }
	held = 0;
}

/* asks the terminal (CSI ? u, then CSI c which every terminal answers) and switches the protocol on */
bool term_kitty(bool on)
{
	if (!on) { kitty_wanted = false; kitty_pop(); return false; }
	kitty_wanted = true;
	if (!kitty_ok) {
		term_outs("\033[?u\033[c");
		term_flush();
		unsigned char c;
		int state = 0;
		bool got_u = false;
		struct pollfd p = {tty, POLLIN, 0};
		char seq[32];
		size_t sl = 0;
		while (poll(&p, 1, 150) > 0 && read(tty, &c, 1) == 1) {
			if (c == 27) { state = 1; sl = 0; continue; }
			if (state == 1) { state = c == '[' ? 2 : 0; continue; }
			if (state == 2) {
				if (sl < sizeof seq - 1) seq[sl++] = (char)c;
				if (c >= 0x40 && c <= 0x7e) {
					if (c == 'u' && seq[0] == '?') got_u = true;
					if (c == 'c') break;
					state = 0; sl = 0;
				}
			}
		}
		kitty_ok = got_u;
	}
	if (kitty_ok) kitty_push();
	return kitty_ok;
}

static int kitty_base(int code)
{
	switch (code) {
	case 13: return K_ENTER;
	case 9: return K_TAB;
	case 27: return K_ESC;
	case 127: return K_BSPACE;
	case 57414: return K_ENTER;                       /* keypad enter */
	}
	return code;
}

/* CSI parameters: up to 16 numbers, sub[i] set when the number followed a ':' */
typedef struct { int v[16]; bool sub[16]; int n; } Par;

static int decode_csi(const unsigned char *s, size_t n, size_t *used, Ev *e)
{
	size_t i = 0;
	Par P = {{0}, {false}, 0};
	char lead = 0;
	if (i < n && (s[i] == '<' || s[i] == '?' || s[i] == '>')) lead = (char)s[i++];
	bool have = false;
	while (i < n && ((s[i] >= '0' && s[i] <= '9') || s[i] == ';' || s[i] == ':')) {
		if (s[i] == ';' || s[i] == ':') {
			if (!have && P.n == 0) P.n = 1;
			if (P.n < 16) { P.sub[P.n] = s[i] == ':'; P.v[P.n++] = 0; }
			have = false; i++; continue;
		}
		if (P.n == 0) P.n = 1;
		P.v[P.n - 1] = P.v[P.n - 1] * 10 + (s[i] - '0');
		have = true;
		i++;
	}
	if (i >= n) return -1;                            /* incomplete */
	char f = (char)s[i++];
	*used = i;
	int nv = P.n;
	int *v = P.v;
	/* the modifier parameter is the second ';' field; an event type follows it after ':' */
	int mfield = -1, event = 1;
	for (int k = 1, fld = 0; k < nv; k++) if (!P.sub[k]) { fld++; if (fld == 1) { mfield = k; break; } }
	if (mfield > 0 && mfield + 1 < nv && P.sub[mfield + 1]) event = v[mfield + 1];
	int mods = mfield > 0 ? csi_mods(v[mfield]) : 0;
	if (lead == '<' && (f == 'M' || f == 'm')) {      /* SGR mouse: button;x;y */
		int b = v[0], x = nv > 1 ? v[1] - 1 : 0, y = nv > 2 ? v[2] - 1 : 0;
		e->mouse = 1; e->x = x; e->y = y;
		int m = 0;
		if (b & 4) m |= M_SHIFT;
		if (b & 8) m |= M_ALT;
		if (b & 16) m |= M_CTRL;
		if (b & 32) { e->key = 0; return 2; }                  /* motion: ignored */
		if (b & 64) { e->key = ((b & 3) == 0 ? K_SCROLL_UP : K_SCROLL_DOWN) | m; return 1; }
		if (f == 'm') { e->key = 0; return 2; }                /* release: ignored (fzf acts on press) */
		int btn = b & 3;
		e->key = (btn == 0 ? K_CLICK : btn == 2 ? K_RCLICK : 0) | m;
		if (!e->key) return 2;
		return 1;
	}
	if (lead) return 2;                                        /* an answer to a query */
	if (f == 'I') { set_held(0); e->key = K_FOCUS_IN; return 1; }
	if (f == 'O') { set_held(0); e->key = K_FOCUS_OUT; return 1; }
	if (kitty_pushed && f != 'u') set_held(mods);
	if (event == 3 && f != 'u') return 2;                      /* a release */
	int base = 0;
	switch (f) {
	case 'A': base = K_UP; break;
	case 'B': base = K_DOWN; break;
	case 'C': base = K_RIGHT; break;
	case 'D': base = K_LEFT; break;
	case 'H': base = K_HOME; break;
	case 'F': base = K_END; break;
	case 'Z': base = K_BTAB; mods = 0; break;
	case 'P': base = K_F1; break;
	case 'Q': base = K_F2; break;
	case 'R': base = K_F3; break;
	case 'S': base = K_F4; break;
	case '~':
		switch (v[0]) {
		case 1: case 7: base = K_HOME; break;
		case 2: base = K_INS; break;
		case 3: base = K_DEL; break;
		case 4: case 8: base = K_END; break;
		case 5: base = K_PGUP; break;
		case 6: base = K_PGDN; break;
		case 11: base = K_F1; break; case 12: base = K_F2; break; case 13: base = K_F3; break; case 14: base = K_F4; break;
		case 15: base = K_F5; break; case 17: base = K_F6; break; case 18: base = K_F7; break; case 19: base = K_F8; break;
		case 20: base = K_F9; break; case 21: base = K_F10; break; case 23: base = K_F11; break; case 24: base = K_F12; break;
		case 200: e->key = K_PASTE; return 3;               /* caller reads until 201~ */
		default: return 2;
		}
		break;
	case 'u': {                                             /* CSI u: key[:shifted[:base]] ; mods[:event] ; text */
		int code = v[0];
		/* modifier keys change the held set only */
		if (code >= 57441 && code <= 57454) {
			int bit = (code == 57441 || code == 57447) ? M_SHIFT : (code == 57442 || code == 57448) ? M_CTRL : (code == 57443 || code == 57449) ? M_ALT : 0;
			if (bit) set_held(event == 3 ? (held & ~bit) : (held | bit));
			return 2;
		}
		if (code >= 57358 && code <= 57360) return 2;      /* caps/num/scroll lock */
		if (event == 3) { if (kitty_pushed) set_held(mods); return 2; }
		if (kitty_pushed) set_held(mods);
		/* text field: the third ';' field, code points separated by ':' */
		int fld = 0, text = 0;
		for (int k = 1; k < nv; k++) {
			if (!P.sub[k]) fld++;
			if (fld == 2 && text == 0 && v[k] >= 32) text = v[k];
		}
		if (text && !(mods & (M_CTRL | M_ALT))) { e->key = text; return 1; }
		base = kitty_base(code);
		if (mods & M_CTRL) {
			if (base >= 'A' && base <= 'Z') base += 32;
			if (base == '/') base = '_';
			if (!(mods & ~M_CTRL)) { if (base == 'i') { e->key = K_TAB; return 1; } if (base == 'm') { e->key = K_ENTER; return 1; } if (base == '[') { e->key = K_ESC; return 1; } }
		}
		if (base == K_TAB && (mods & M_SHIFT)) { e->key = K_BTAB | (mods & ~M_SHIFT); return 1; }
		break;
	}
	default: return 2;
	}
	e->key = base | mods;
	return 1;
}

int term_read(Ev *e)
{
	memset(e, 0, sizeof *e);
	if (in_pos >= in_len) { if (!fill(0)) return 0; }
	for (;;) {
		if (in_pos >= in_len) return 0;
		const unsigned char *s = in + in_pos;
		size_t n = in_len - in_pos;
		unsigned char c = s[0];
		if (c == 27) {
			if (n == 1) { if (!fill(25)) { in_pos++; e->key = K_ESC; return 1; } continue; }   /* a lone ESC after a short wait */
			if (s[1] == '[' && n >= 3 && s[2] == '[') {             /* Linux console F1-F5: ESC [ [ A..E */
				if (n < 4) { if (!fill(25)) { in_pos = in_len; return 0; } continue; }
				if (s[3] >= 'A' && s[3] <= 'E') { in_pos += 4; e->key = K_F1 + (s[3] - 'A'); return 1; }
			}
			if (s[1] == '[') {
				size_t used = 0;
				int r = decode_csi(s + 2, n - 2, &used, e);
				if (r < 0) { if (!fill(25)) { in_pos = in_len; return 0; } continue; }
				in_pos += 2 + used;
				if (r == 3) {                                       /* bracketed paste: up to ESC [ 201 ~ */
					Buf b; buf_init(&b);
					for (;;) {
						while (in_pos < in_len) {
							if (in_len - in_pos >= 6 && !memcmp(in + in_pos, "\033[201~", 6)) { in_pos += 6; goto pasted; }
							buf_addc(&b, (char)in[in_pos++]);
						}
						if (!fill(500)) break;
					}
				pasted:
					e->key = K_PASTE; e->paste = buf_take(&b);
					return 1;
				}
				if (r == 2) { memset(e, 0, sizeof *e); continue; }
				return 1;
			}
			if (s[1] == 'O' && n >= 3) {                            /* SS3 */
				in_pos += 3;
				switch (s[2]) {
				case 'P': e->key = K_F1; break; case 'Q': e->key = K_F2; break; case 'R': e->key = K_F3; break; case 'S': e->key = K_F4; break;
				case 'A': e->key = K_UP; break; case 'B': e->key = K_DOWN; break; case 'C': e->key = K_RIGHT; break; case 'D': e->key = K_LEFT; break;
				case 'H': e->key = K_HOME; break; case 'F': e->key = K_END; break;
				default: continue;
				}
				return 1;
			}
			if (s[1] == 'O' && n < 3) { if (!fill(25)) { in_pos++; e->key = K_ESC; return 1; } continue; }
			/* alt + key */
			in_pos++;
			Ev inner;
			int r = term_read(&inner);
			if (!r) { e->key = K_ESC; return 1; }
			e->key = inner.key | M_ALT; e->x = inner.x; e->y = inner.y; e->mouse = inner.mouse; e->paste = inner.paste;
			return 1;
		}
		/* plain byte(s) */
		if (c == 13) { in_pos++; e->key = K_ENTER; return 1; }
		if (c == 9) { in_pos++; e->key = K_TAB; return 1; }
		if (c == 127) { in_pos++; e->key = K_BSPACE; return 1; }
		if (c == 0) { in_pos++; e->key = M_CTRL | ' '; return 1; }
		if (c < 32) { in_pos++; e->key = M_CTRL | (c >= 28 ? ("\\]^_"[c - 28]) : ('a' + c - 1)); return 1; }
		rune r;
		int len = u8dec((const char *)s, n, &r);
		if (len > 1 && (size_t)len > n) { if (!fill(25)) { in_pos = in_len; return 0; } continue; }
		in_pos += (size_t)len;
		e->key = (int)r;
		return 1;
	}
}

int term_cursor_row(void)
{
	term_outs("\033[6n");
	term_flush();
	unsigned char buf[32];
	size_t n = 0;
	struct pollfd p = {tty, POLLIN, 0};
	while (n < sizeof buf - 1 && poll(&p, 1, 300) > 0) {
		ssize_t r = read(tty, buf + n, 1);
		if (r <= 0) break;
		n++;
		if (buf[n - 1] == 'R') break;
	}
	buf[n] = 0;
	int row, col;
	if (sscanf((char *)buf, "\033[%d;%dR", &row, &col) == 2) return row;
	return -1;
}

/* ======== theme.c ======== */
/* theme.c - the default (dark 256) theme of fzf and the --color parser. */

static Col mk(int fg, int bg, int attr) { return (Col){fg, bg, attr}; }

void theme_default(Theme *t)
{
	for (int i = 0; i < C_COUNT; i++) t->c[i] = mk(COL_UNDEF, COL_UNDEF, 0);
	t->colored = true;
	t->c[C_FG] = mk(COL_DEF, COL_UNDEF, 0);
	t->c[C_BG] = mk(COL_UNDEF, COL_DEF, 0);
	t->c[C_HL] = mk(108, COL_UNDEF, 0);
	t->c[C_FGP] = mk(254, 236, ATTR_BOLD);
	t->c[C_HLP] = mk(151, COL_UNDEF, 0);
	t->c[C_INFO] = mk(144, COL_UNDEF, 0);
	t->c[C_BORDER] = mk(59, COL_UNDEF, 0);
	t->c[C_PROMPT] = mk(110, COL_UNDEF, ATTR_BOLD);
	t->c[C_POINTER] = mk(161, COL_UNDEF, ATTR_BOLD);
	t->c[C_MARKER] = mk(168, COL_UNDEF, 0);
	t->c[C_SPINNER] = mk(148, COL_UNDEF, 0);
	t->c[C_HEADER] = mk(109, COL_UNDEF, 0);
	t->c[C_FOOTER] = mk(109, COL_UNDEF, 0);
	t->c[C_LABEL] = mk(145, COL_UNDEF, 0);
	t->c[C_QUERY] = mk(COL_DEF, COL_UNDEF, ATTR_BOLD);
}

static void theme_16(Theme *t)
{
	theme_default(t);
	t->c[C_HL] = mk(2, COL_UNDEF, 0); t->c[C_FGP] = mk(15, 8, 0);
	t->c[C_HLP] = mk(10, COL_UNDEF, 0); t->c[C_INFO] = mk(3, COL_UNDEF, 0); t->c[C_BORDER] = mk(COL_UNDEF, COL_UNDEF, 0);
	t->c[C_PROMPT] = mk(4, COL_UNDEF, 0); t->c[C_POINTER] = mk(1, COL_UNDEF, 0); t->c[C_MARKER] = mk(5, COL_UNDEF, 0);
	t->c[C_SPINNER] = mk(2, COL_UNDEF, 0); t->c[C_HEADER] = mk(6, COL_UNDEF, 0); t->c[C_FOOTER] = mk(6, COL_UNDEF, 0);
	t->c[C_LABEL] = mk(COL_UNDEF, COL_UNDEF, 0);
}
static void theme_light(Theme *t)
{
	theme_default(t);
	t->c[C_HL] = mk(65, COL_UNDEF, 0); t->c[C_FGP] = mk(236, 254, 0);
	t->c[C_HLP] = mk(65, COL_UNDEF, 0); t->c[C_INFO] = mk(101, COL_UNDEF, 0); t->c[C_BORDER] = mk(145, COL_UNDEF, 0);
	t->c[C_PROMPT] = mk(110, COL_UNDEF, 0); t->c[C_POINTER] = mk(161, COL_UNDEF, 0); t->c[C_MARKER] = mk(168, COL_UNDEF, 0);
	t->c[C_SPINNER] = mk(148, COL_UNDEF, 0); t->c[C_HEADER] = mk(31, COL_UNDEF, 0); t->c[C_FOOTER] = mk(31, COL_UNDEF, 0);
	t->c[C_LABEL] = mk(145, COL_UNDEF, 0);
}

/* the TempleOS-like scheme (blue ground, yellow text) used by the dmenu rofi mode */
static void theme_temple(Theme *t)
{
	theme_default(t);
	t->c[C_FG] = mk(11, 4, 0); t->c[C_FGP] = mk(0, 14, ATTR_BOLD); t->c[C_HL] = mk(15, COL_UNDEF, ATTR_BOLD);
	t->c[C_HLP] = mk(4, COL_UNDEF, ATTR_BOLD); t->c[C_INFO] = mk(14, COL_UNDEF, 0); t->c[C_BORDER] = mk(14, COL_UNDEF, 0);
	t->c[C_PROMPT] = mk(15, COL_UNDEF, ATTR_BOLD); t->c[C_POINTER] = mk(12, COL_UNDEF, ATTR_BOLD); t->c[C_MARKER] = mk(10, COL_UNDEF, 0);
	t->c[C_HEADER] = mk(14, COL_UNDEF, 0); t->c[C_FOOTER] = mk(14, COL_UNDEF, 0); t->c[C_LABEL] = mk(15, COL_UNDEF, 0);
	t->c[C_GUTTER].fg = 4; t->c[C_GUTTER].bg = 4; t->c[C_QUERY] = mk(15, COL_UNDEF, ATTR_BOLD);
}

static int color_of(const char *s)
{
	static const char *names[] = {"black", "red", "green", "yellow", "blue", "magenta", "cyan", "white"};
	if (!*s) return COL_UNDEF;
	if (!strcmp(s, "-1") || !strcmp(s, "default")) return COL_DEF;
	for (int i = 0; i < 8; i++) {
		if (!strcmp(s, names[i])) return i;
		char b[24]; snprintf(b, sizeof b, "bright-%s", names[i]);
		if (!strcmp(s, b)) return i + 8;
	}
	if (!strcmp(s, "grey") || !strcmp(s, "gray")) return 8;
	if (s[0] == '#' && strlen(s) == 7) {
		unsigned r, g, b;
		if (sscanf(s + 1, "%2x%2x%2x", &r, &g, &b) == 3) return 0x1000000 | (int)(r << 16 | g << 8 | b);
	}
	char *e;
	long v = strtol(s, &e, 10);
	if (!*e && v >= 0 && v < 256) return (int)v;
	return COL_UNDEF;
}
static int attr_of(const char *s)
{
	if (!strcmp(s, "regular") || !strcmp(s, "normal")) return 0;
	static const struct { const char *n; int a; } at[] = {{"bold", ATTR_BOLD}, {"dim", ATTR_DIM}, {"italic", ATTR_ITAL}, {"underline", ATTR_UL},
		{"blink", ATTR_BLINK}, {"reverse", ATTR_REV}, {"strikethrough", ATTR_STRIKE}};
	for (size_t i = 0; i < sizeof at / sizeof at[0]; i++) if (!strcmp(s, at[i].n)) return at[i].a;
	return -1;
}

static int slot_of(const char *n, bool *bg)
{
	static const struct { const char *name; int slot; bool bg; } m[] = {
		{"fg", C_FG, false}, {"bg", C_FG, true}, {"hl", C_HL, false}, {"fg+", C_FGP, false}, {"bg+", C_FGP, true}, {"hl+", C_HLP, false},
		{"info", C_INFO, false}, {"border", C_BORDER, false}, {"prompt", C_PROMPT, false}, {"pointer", C_POINTER, false},
		{"marker", C_MARKER, false}, {"spinner", C_SPINNER, false}, {"header", C_HEADER, false}, {"footer", C_FOOTER, false},
		{"gutter", C_GUTTER, true}, {"query", C_QUERY, false}, {"disabled", C_DISABLED, false}, {"preview-fg", C_PFG, false},
		{"preview-bg", C_PFG, true}, {"label", C_LABEL, false}, {"separator", C_SEP, false}, {"scrollbar", C_SCROLL, false},
		{"preview-scrollbar", C_PSCROLL, false}, {"preview-border", C_PBORDER, false}, {"preview-label", C_PLABEL, false},
		{"selected-fg", C_SELFG, false}, {"selected-bg", C_SELFG, true}, {"ghost", C_GHOST, false}, {"nth", C_NTH, false},
		{"list-fg", C_FG, false}, {"list-bg", C_FG, true}, {"input-fg", C_QUERY, false}, {"input-bg", C_QUERY, true},
		{"header-bg", C_HEADER, true}, {"footer-bg", C_FOOTER, true}, {"border-label", C_LABEL, false},
		{"list-border", C_BORDER, false}, {"input-border", C_BORDER, false}, {"header-border", C_BORDER, false},
		{"footer-border", C_BORDER, false}, {"list-label", C_LABEL, false}, {"input-label", C_LABEL, false},
		{"header-label", C_LABEL, false}, {"footer-label", C_LABEL, false}};
	for (size_t i = 0; i < sizeof m / sizeof m[0]; i++)
		if (!strcmp(n, m[i].name)) { *bg = m[i].bg; return m[i].slot; }
	return -1;
}

void theme_apply(Theme *t, const char *spec)
{
	char *c = xstrdup(spec), *save = NULL;
	for (char *tok = strtok_r(c, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
		while (*tok == ' ') tok++;
		if (!strcmp(tok, "dark")) { theme_default(t); continue; }
		if (!strcmp(tok, "light")) { theme_light(t); continue; }
		if (!strcmp(tok, "temple")) { theme_temple(t); continue; }
		if (!strcmp(tok, "16")) { theme_16(t); continue; }
		if (!strcmp(tok, "bw")) { t->colored = false; continue; }
		char *colon = strchr(tok, ':');
		if (!colon) { die("invalid color specification: %s", tok); }
		*colon = 0;
		bool bg;
		int slot = slot_of(tok, &bg);
		if (slot < 0) die("invalid color specification: %s", tok);
		char *rest = colon + 1, *sv2 = NULL;
		bool first = true;
		int attr = 0;
		bool have_attr = false;
		int colv = COL_UNDEF;
		for (char *p = strtok_r(rest, ":", &sv2); p; p = strtok_r(NULL, ":", &sv2)) {
			int a = first ? -1 : attr_of(p);
			if (first) { colv = color_of(p); if (colv == COL_UNDEF && *p) { a = attr_of(p); if (a >= 0) { attr |= a; have_attr = true; } } }
			else if (a >= 0) { attr |= a; have_attr = true; }
			first = false;
		}
		/* "bg" and "bg+" style names set the background of the slot; foreground ones the foreground */
		if (bg) { if (colv != COL_UNDEF) t->c[slot].bg = colv; }
		else { if (colv != COL_UNDEF) t->c[slot].fg = colv; if (have_attr) t->c[slot].attr = attr; }
	}
	free(c);
}

/* inherited slots: pointer/marker/gutter and friends take from the list colours */
static void inherit(Col *d, const Col *p)
{
	if (d->fg == COL_UNDEF) d->fg = p->fg;
	if (d->bg == COL_UNDEF) d->bg = p->bg;
}
void theme_resolve(Theme *t)
{
	Col *c = t->c;
	if (!t->colored) { for (int i = 0; i < C_COUNT; i++) c[i] = mk(COL_DEF, COL_DEF, 0); c[C_HL].attr = ATTR_BOLD | ATTR_UL; c[C_FGP].attr = ATTR_REV; c[C_POINTER].attr = ATTR_BOLD; return; }
	if (c[C_FG].fg == COL_UNDEF) c[C_FG].fg = COL_DEF;
	if (c[C_FG].bg == COL_UNDEF) c[C_FG].bg = COL_DEF;
	if (c[C_FGP].bg == COL_UNDEF) c[C_FGP].bg = 236;
	if (c[C_GUTTER].bg == COL_UNDEF) c[C_GUTTER].bg = c[C_FGP].bg;
	if (c[C_GUTTER].fg == COL_UNDEF) c[C_GUTTER].fg = c[C_FGP].bg;
	inherit(&c[C_POINTER], &c[C_FGP]);
	if (c[C_POINTER].bg == COL_UNDEF) c[C_POINTER].bg = c[C_FGP].bg;
	if (c[C_MARKER].bg == COL_UNDEF) c[C_MARKER].bg = c[C_FGP].bg;
	inherit(&c[C_SEP], &c[C_BORDER]);
	inherit(&c[C_SCROLL], &c[C_BORDER]);
	inherit(&c[C_PBORDER], &c[C_BORDER]);
	inherit(&c[C_PSCROLL], &c[C_PBORDER]);
	inherit(&c[C_PFG], &c[C_FG]);
	inherit(&c[C_PLABEL], &c[C_LABEL]);
	inherit(&c[C_DISABLED], &c[C_INFO]);
	inherit(&c[C_GHOST], &c[C_DISABLED]);
	if (c[C_QUERY].fg == COL_UNDEF) c[C_QUERY].fg = COL_DEF;
	for (int i = 0; i < C_COUNT; i++) { if (c[i].fg == COL_UNDEF) c[i].fg = COL_DEF; if (c[i].bg == COL_UNDEF) c[i].bg = COL_DEF; }
	/* the background of the bg+ slot is the current line, stored in C_BGP.bg; fg+ text uses C_FGP.fg */
}

static void emit_col(Buf *b, int v, bool bg)
{
	if (v == COL_DEF || v == COL_UNDEF) return;
	if (v & 0x1000000) buf_printf(b, ";%d;2;%d;%d;%d", bg ? 48 : 38, (v >> 16) & 255, (v >> 8) & 255, v & 255);
	else if (v < 8) buf_printf(b, ";%d", (bg ? 40 : 30) + v);
	else if (v < 16) buf_printf(b, ";%d", (bg ? 100 : 90) + v - 8);
	else buf_printf(b, ";%d;5;%d", bg ? 48 : 38, v);
}
void sgr_emit(Buf *b, Col c)
{
	buf_adds(b, "\033[0");
	if (c.attr & ATTR_BOLD) buf_adds(b, ";1");
	if (c.attr & ATTR_DIM) buf_adds(b, ";2");
	if (c.attr & ATTR_ITAL) buf_adds(b, ";3");
	if (c.attr & ATTR_UL) buf_adds(b, ";4");
	if (c.attr & ATTR_BLINK) buf_adds(b, ";5");
	if (c.attr & ATTR_REV) buf_adds(b, ";7");
	if (c.attr & ATTR_STRIKE) buf_adds(b, ";9");
	emit_col(b, c.fg, false);
	emit_col(b, c.bg, true);
	buf_addc(b, 'm');
}

/* One theme file for every tool: $FZF_THEME_FILE, else ~/.config/fzf/theme. It holds a --color value
 * (a preset name such as dark/light/16/temple/bw and/or slot:colour pairs, separated by commas or newlines). */
void theme_load_file(Theme *t)
{
	const char *p = getenv("FZF_THEME_FILE");
	char path[1024];
	if (!p || !*p) {
		const char *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
		if (x && *x) snprintf(path, sizeof path, "%s/fzf/theme", x);
		else if (h) snprintf(path, sizeof path, "%s/.config/fzf/theme", h);
		else return;
		p = path;
	}
	FILE *f = fopen(p, "r");
	if (!f) return;
	Buf b;
	buf_init(&b);
	char line[512];
	while (fgets(line, sizeof line, f)) {
		size_t n = strlen(line);
		while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = 0;
		char *s = line;
		while (*s == ' ' || *s == '\t') s++;
		if (!*s || *s == '#') continue;
		if (b.len) buf_addc(&b, ',');
		buf_adds(&b, s);
	}
	fclose(f);
	if (b.len) theme_apply(t, b.p);
	buf_free(&b);
}

/* ======== keys.c ======== */
/* keys.c - key names, the --bind grammar ("key:action+action(arg),key2:action"), the action table, default bindings. */

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

/* ======== draw.c ======== */
/* draw.c - the frame: a grid of cells filled by the layout code and written to the terminal in one go. */

/* ------------------------------------------------------------------ grid */
Col col_mk(int fg, int bg, int attr) { return (Col){fg, bg, attr}; }
Col col_mix(Col base, Col over)      /* over wins where it says something */
{
	Col r = base;
	if (over.fg != COL_DEF && over.fg != COL_UNDEF) r.fg = over.fg;
	if (over.bg != COL_DEF && over.bg != COL_UNDEF) r.bg = over.bg;
	r.attr |= over.attr;
	return r;
}

void grid_init(int w, int h)
{
	free(U.g.cell);
	U.g.w = w; U.g.h = h;
	U.g.cell = xmalloc((size_t)w * (size_t)h * sizeof(Cell));
}

static void grid_clear(void)
{
	Col c = col_mk(COL_DEF, U.th.c[C_FG].bg, 0);
	for (int i = 0; i < U.g.w * U.g.h; i++) { U.g.cell[i].ch = ' '; U.g.cell[i].c = c; }
}

static inline bool inr(Rect r, int x, int y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

/* --no-unicode: the drawing characters fzf uses, as ASCII */
static rune ascii_of(rune ch)
{
	switch (ch) {
	case 0x2500: case 0x2501: case 0x2550: return '-';
	case 0x2502: case 0x2503: case 0x2551: case 0x2590: return '|';
	case 0x250C: case 0x2510: case 0x2514: case 0x2518: case 0x256D: case 0x256E: case 0x256F: case 0x2570: case 0x250F: case 0x2513: case 0x2517: case 0x251B:
	case 0x2554: case 0x2557: case 0x255A: case 0x255D: return '+';
	case 0x258C: return '>';
	case 0x2191: return '^';
	case 0x2193: return 'v';
	case 0x2190: return '<';
	case 0x2192: return '>';
	case 0xD7: return 'x';
	}
	return ch > 127 ? '?' : ch;
}

static void gput(Rect clip, int x, int y, rune ch, Col c)
{
	if (U.o->ascii && ch > 127 && ch < 0x3000 && !(ch >= 0xA0 && ch < 0x2000)) ch = ascii_of(ch);   /* keep letters (Latin, Cyrillic...), replace symbols */
	int w = rune_width(ch);
	if (w == 0) return;
	if (!inr(clip, x, y) || x >= U.g.w || y >= U.g.h) return;
	if (w == 2 && !inr(clip, x + 1, y)) { ch = ' '; w = 1; }
	Cell *cl = &U.g.cell[y * U.g.w + x];
	cl->ch = ch; cl->c = c;
	if (w == 2) { cl[1].ch = 0; cl[1].c = c; }
}

/* draws s[0..n) at (x,y) in clip, at most maxw columns; returns the columns used */
static int gstr(Rect clip, int x, int y, const char *s, size_t n, Col c, int maxw)
{
	int used = 0;
	for (size_t i = 0; i < n;) {
		rune r;
		int l = u8dec(s + i, n - i, &r);
		i += (size_t)l;
		if (r < 32 || r == 127) r = ' ';
		int w = rune_width(r);
		if (used + w > maxw) break;
		gput(clip, x + used, y, r, c);
		used += w;
	}
	return used;
}
int gs(Rect clip, int x, int y, const char *s, Col c) { return gstr(clip, x, y, s, strlen(s), c, 1 << 20); }

void gfill(Rect r, Col c)
{
	for (int y = r.y; y < r.y + r.h; y++)
		for (int x = r.x; x < r.x + r.w; x++)
			if (x >= 0 && y >= 0 && x < U.g.w && y < U.g.h) { Cell *cl = &U.g.cell[y * U.g.w + x]; cl->ch = ' '; cl->c = c; }
}

/* ------------------------------------------------------------------ borders */
int border_edges(int s, int *t, int *r, int *b, int *l)
{
	*t = *r = *b = *l = 0;
	switch (s) {
	case BORDER_NONE: break;
	case BORDER_HORIZONTAL: *t = *b = 1; break;
	case BORDER_VERTICAL: *r = *l = 1; break;
	case BORDER_TOP: *t = 1; break;
	case BORDER_BOTTOM: *b = 1; break;
	case BORDER_LEFT: *l = 1; break;
	case BORDER_RIGHT: *r = 1; break;
	default: *t = *r = *b = *l = 1;
	}
	return *t || *r || *b || *l;
}
static Rect border_inner(Rect r, int shape)
{
	int t, rr, b, l;
	border_edges(shape, &t, &rr, &b, &l);
	r.x += l * 2; r.w -= l * 2 + rr * 2;                     /* one column of padding next to a vertical edge */
	r.y += t; r.h -= t + b;
	if (r.w < 0) r.w = 0;
	if (r.h < 0) r.h = 0;
	return r;
}
static void draw_border(Rect r, int shape, Col c, const char *label, int labelpos)
{
	if (shape == BORDER_NONE || r.w < 1 || r.h < 1) return;
	int t, rr, b, l;
	border_edges(shape, &t, &rr, &b, &l);
	const char *g[6] = {"╭", "╮", "╰", "╯", "─", "│"};
	if (shape == BORDER_SHARP) { g[0] = "┌"; g[1] = "┐"; g[2] = "└"; g[3] = "┘"; }
	else if (shape == BORDER_BOLD) { g[0] = "┏"; g[1] = "┓"; g[2] = "┗"; g[3] = "┛"; g[4] = "━"; g[5] = "┃"; }
	else if (shape == BORDER_DOUBLE) { g[0] = "╔"; g[1] = "╗"; g[2] = "╚"; g[3] = "╝"; g[4] = "═"; g[5] = "║"; }
	Rect all = {0, 0, U.g.w, U.g.h};
	int x1 = r.x + r.w - 1, y1 = r.y + r.h - 1;
	for (int x = r.x; x <= x1; x++) {
		if (t) gs(all, x, r.y, g[4], c);
		if (b) gs(all, x, y1, g[4], c);
	}
	for (int y = r.y; y <= y1; y++) {
		if (l) gs(all, r.x, y, g[5], c);
		if (rr) gs(all, x1, y, g[5], c);
	}
	if (t && l) gs(all, r.x, r.y, g[0], c);
	if (t && rr) gs(all, x1, r.y, g[1], c);
	if (b && l) gs(all, r.x, y1, g[2], c);
	if (b && rr) gs(all, x1, y1, g[3], c);
	if (label && *label && t) {
		int lw = str_width(label, strlen(label));
		int room = r.w - 4;
		if (room > 0) {
			if (lw > room) lw = room;
			int x = labelpos < 0 ? x1 - 1 - lw - 1 : r.x + 2;
			if (labelpos > 0) x = r.x + labelpos + 1;
			Col lc = col_mix(col_mk(COL_DEF, U.th.c[C_FG].bg, 0), U.th.c[C_LABEL]);
			gstr(all, x, r.y, label, strlen(label), lc, lw);
		}
	}
}

/* ------------------------------------------------------------------ text with colours */
typedef struct { rune ch; int w; Col c; int bytepos; } Glyph;
static Glyph *gl_buf; static size_t gl_cap;

/* decodes text into glyphs: tabs expanded, escapes (spans) applied, match positions highlighted */
static size_t glyphs_of(const char *text, size_t tlen, const AnsiSpan *sp, uint32_t nsp, const int *pos, int npos,
                        Col base, Col hl, int tabstop, int *total_w)
{
	size_t n = 0;
	int w = 0, ri = 0, pi = 0;
	uint32_t si = 0;
	/* pos is sorted descending or unsorted: mark with a small bitmap-ish scan instead */
	for (size_t i = 0; i < tlen;) {
		rune r;
		int l = u8dec(text + i, tlen - i, &r);
		Col c = base;
		if (nsp) {
			while (si < nsp && sp[si].off + sp[si].len <= i) si++;
			if (si < nsp && sp[si].off <= i) {
				const AnsiSpan *s = &sp[si];
				int f = (int)s->fg, b = (int)s->bg;
				if (f != -1) c.fg = f;
				if (b != -1) c.bg = b;
				c.attr |= s->attr;
			}
		}
		bool m = false;
		for (pi = 0; pi < npos; pi++) if (pos[pi] == ri) { m = true; break; }
		if (m) c = col_mix(c, hl);
		int cells = 1;
		if (r == '\t') { cells = tabstop - (w % tabstop); r = ' '; }
		else if (r < 32 || r == 127) r = ' ';
		else cells = rune_width(r);
		if (cells == 0) { i += (size_t)l; ri++; continue; }
		if (r == ' ' && cells > 1) {                       /* a tab */
			for (int k = 0; k < cells; k++) {
				if (n == gl_cap) { gl_cap = gl_cap ? gl_cap * 2 : 1024; gl_buf = xrealloc(gl_buf, gl_cap * sizeof(Glyph)); }
				gl_buf[n++] = (Glyph){' ', 1, c, (int)i};
			}
		} else {
			if (n == gl_cap) { gl_cap = gl_cap ? gl_cap * 2 : 1024; gl_buf = xrealloc(gl_buf, gl_cap * sizeof(Glyph)); }
			gl_buf[n++] = (Glyph){r, cells, c, (int)i};
		}
		w += cells;
		i += (size_t)l; ri++;
	}
	*total_w = w;
	return n;
}

/* draws a line of glyphs into avail columns, scrolled to make column `want_end` visible, with ellipsis marks */
static void draw_glyphs(Rect clip, int x, int y, int avail, size_t n, int total, int want_end, bool right, Col fill)
{
	if (avail <= 0) return;
	const char *ell = U.o->ellipsis;
	int ew = str_width(ell, strlen(ell));
	size_t from = 0, to = n;
	bool cut_l = false, cut_r = false;
	if (total > avail) {
		int off = 0;
		if (right) off = total - avail;
		else if (want_end > avail - ew) { off = want_end - avail + 10; if (off > total - avail) off = total - avail; if (off < 0) off = 0; }
		int acc = 0;
		while (from < n && acc < off) acc += gl_buf[from++].w;
		cut_l = from > 0;
		int used = 0, lead = cut_l ? ew : 0;
		size_t k = from;
		int rem = avail - lead;
		while (k < n && used + gl_buf[k].w <= rem) used += gl_buf[k++].w;
		to = k;
		cut_r = to < n;
		if (cut_r) {                                       /* make room for the right ellipsis */
			while (to > from && used > rem - ew) used -= gl_buf[--to].w;
		}
	}
	int cx = x;
	if (cut_l) { cx += gstr(clip, cx, y, ell, strlen(ell), fill, ew); }
	for (size_t k = from; k < to; k++) { gput(clip, cx, y, gl_buf[k].ch, gl_buf[k].c); cx += gl_buf[k].w; }
	if (cut_r) gstr(clip, cx, y, ell, strlen(ell), fill, x + avail - cx);
}

/* an escape-carrying string (header, footer, preview line) drawn as-is */
static void draw_ansi(Rect clip, int x, int y, int avail, const char *raw, size_t n, Col base)
{
	Buf b;
	buf_init(&b);
	AnsiSpan *sp = NULL;
	uint32_t ns = 0, cap = 0;
	ansi_extract(raw, n, &b, &sp, &ns, &cap);
	int total;
	size_t gn = glyphs_of(b.p ? b.p : "", b.len, sp, ns, NULL, 0, base, base, U.o->tabstop, &total);
	(void)gn;
	draw_glyphs(clip, x, y, avail, gn, total, 0, false, base);
	free(sp);
	buf_free(&b);
}

/* ------------------------------------------------------------------ list */
static int ptr_w, mrk_w;

static void draw_item_row(Rect win, int y, size_t hi, bool cur, int text_x, int avail_w)
{
	Item *it = U.items.v[U.hits[hi].idx];
	bool sel = (it->flags & ITEM_SEL) != 0;
	Theme *T = &U.th;
	Col plain = col_mk(T->c[C_FG].fg, T->c[C_FG].bg, T->c[C_FG].attr);
	Col base = plain, hlc = col_mk(T->c[C_HL].fg, COL_DEF, T->c[C_HL].attr);
	if (cur) {
		base = col_mk(T->c[C_FGP].fg, T->c[C_FGP].bg, T->c[C_FGP].attr);
		hlc = col_mk(T->c[C_HLP].fg, COL_DEF, T->c[C_HLP].attr);
	} else if (sel && (T->c[C_SELFG].fg != COL_DEF || T->c[C_SELFG].bg != COL_DEF)) {
		base = col_mix(base, T->c[C_SELFG]);
	}
	Col ptr = col_mix(base, T->c[C_POINTER]); ptr.bg = cur ? T->c[C_FGP].bg : plain.bg;
	Col mrk = col_mix(base, T->c[C_MARKER]); mrk.bg = cur ? T->c[C_FGP].bg : plain.bg;
	Col gut = col_mk(T->c[C_GUTTER].fg, plain.bg, 0);
	int x = win.x;
	if (cur) { ptr.attr |= 0; gs(win, x, y, U.o->pointer, ptr); }
	else if (!strcmp(U.o->pointer, "▌")) gs(win, x, y, U.o->pointer, gut);
	x += ptr_w;
	if (sel) gs(win, x, y, U.o->marker, mrk);
	else if (cur) gfill((Rect){x, y, mrk_w, 1}, col_mk(COL_DEF, base.bg, 0));
	x += mrk_w;
	if (cur && U.o->highlight_line) gfill((Rect){x, y, win.x + win.w - x, 1}, base);
	if (cur) gfill((Rect){win.x + 0, y, 0, 0}, base);
	MatchOut mo;
	int *pos = NULL, npos = 0, maxe = 0;
	if (!pattern_empty(U.pat) && pattern_match(U.pat, it, U.sc, true, &mo)) { pos = mo.pos; npos = mo.npos; maxe = mo.maxe; }
	int total;
	size_t n = glyphs_of(it->text, it->tlen, U.o->ansi ? it->ansi : NULL, U.o->ansi ? it->nansi : 0, pos, npos, base, hlc,
	                     U.o->tabstop, &total);
	int endcol = 0;
	if (maxe > 0) { int a = 0; for (size_t k = 0; k < n && (int)k < maxe; k++) a += gl_buf[k].w; endcol = a + 1; }
	if (cur) gfill((Rect){text_x, y, total < avail_w ? total : avail_w, 1}, base);
	draw_glyphs(win, text_x, y, avail_w, n, total, endcol, U.o->keep_right, plain);
}

static void draw_scrollbar(Rect r, int shown, size_t total, size_t top)
{
	if (total <= (size_t)shown || shown < 1) return;
	int th = (int)((size_t)shown * (size_t)shown / total);
	if (th < 1) th = 1;
	int tp = (int)((size_t)(shown - th) * top / (total - (size_t)shown));
	Col c = col_mk(U.th.c[C_SCROLL].fg, U.th.c[C_FG].bg, 0);
	for (int i = 0; i < th; i++) gs(r, r.x + r.w - 1, r.y + tp + i, "│", c);
}

/* --wrap: an item takes as many rows as its text needs; continuation lines start with "↳ " */
static int wrap_split(size_t n, int avail, int *starts, int maxlines)
{
	int lines = 1, w = 0, lim = avail;
	if (starts) starts[0] = 0;
	for (size_t k = 0; k < n; k++) {
		if (w + gl_buf[k].w > lim && w > 0) {
			if (starts && lines < maxlines) starts[lines] = (int)k;
			lines++; w = 0; lim = avail - 2 > 0 ? avail - 2 : 1;
		}
		w += gl_buf[k].w;
	}
	return lines;
}

static int item_lines(size_t hi, int avail)
{
	Item *it = U.items.v[U.hits[hi].idx];
	int total;
	Col c = col_mk(COL_DEF, COL_DEF, 0);
	size_t n = glyphs_of(it->text, it->tlen, NULL, 0, NULL, 0, c, c, U.o->tabstop, &total);
	return total <= avail ? 1 : wrap_split(n, avail, NULL, 0);
}

static void draw_list_wrap(Rect r, bool bottom_up)
{
	int avail = r.w - ptr_w - mrk_w - 1;                 /* the last column stays free, like fzf */
	if (avail < 4) avail = 4;
	free(U.rowmap);
	U.rowmap = xmalloc((size_t)r.h * sizeof(size_t));
	for (int i = 0; i < r.h; i++) U.rowmap[i] = (size_t)-1;
	if (!U.nhits) return;
	/* scroll so that the whole current item is visible */
	if (U.cur < U.top) U.top = U.cur;
	for (;;) {
		int rows = 0;
		for (size_t i = U.top; i <= U.cur; i++) rows += item_lines(i, avail);
		if (rows <= r.h || U.top >= U.cur) break;
		U.top++;
	}
	Theme *T = &U.th;
	Col plain = col_mk(T->c[C_FG].fg, T->c[C_FG].bg, T->c[C_FG].attr);
	int used = 0;
	for (size_t hi = U.top; hi < U.nhits && used < r.h; hi++) {
		Item *it = U.items.v[U.hits[hi].idx];
		bool cur = hi == U.cur, sel = (it->flags & ITEM_SEL) != 0;
		Col base = plain, hlc = col_mk(T->c[C_HL].fg, COL_DEF, T->c[C_HL].attr);
		if (cur) { base = col_mk(T->c[C_FGP].fg, T->c[C_FGP].bg, T->c[C_FGP].attr); hlc = col_mk(T->c[C_HLP].fg, COL_DEF, T->c[C_HLP].attr); }
		Col ptr = col_mix(base, T->c[C_POINTER]); ptr.bg = cur ? T->c[C_FGP].bg : plain.bg;
		Col mrk = col_mix(base, T->c[C_MARKER]); mrk.bg = cur ? T->c[C_FGP].bg : plain.bg;
		Col gut = col_mk(T->c[C_GUTTER].fg, plain.bg, 0);
		MatchOut mo;
		int *pos = NULL, npos = 0;
		if (!pattern_empty(U.pat) && pattern_match(U.pat, it, U.sc, true, &mo)) { pos = mo.pos; npos = mo.npos; }
		int total;
		size_t n = glyphs_of(it->text, it->tlen, U.o->ansi ? it->ansi : NULL, U.o->ansi ? it->nansi : 0, pos, npos, base, hlc, U.o->tabstop, &total);
		int starts[64];
		int lines = total <= avail ? 1 : wrap_split(n, avail, starts, 64);
		if (total <= avail) starts[0] = 0;
		int cap = lines < 64 ? lines : 64;
		for (int ln = 0; ln < cap; ln++) {
			int row = used + ln;                              /* rows from the list's start edge */
			int y = bottom_up ? r.y + r.h - used - lines + ln : r.y + row;
			if (row >= r.h || y < r.y) continue;
			U.rowmap[y - r.y] = hi;
			int x = r.x;
			if (cur) gs(r, x, y, U.o->pointer, ptr);
			else if (!strcmp(U.o->pointer, "▌")) gs(r, x, y, U.o->pointer, gut);
			x += ptr_w;
			if (sel) gs(r, x, y, U.o->marker, mrk);
			else if (cur) gfill((Rect){x, y, mrk_w, 1}, col_mk(COL_DEF, base.bg, 0));
			x += mrk_w;
			size_t from = (size_t)starts[ln], to = ln + 1 < lines && ln + 1 < cap ? (size_t)starts[ln + 1] : n;
			int lw = 0;
			for (size_t k = from; k < to; k++) lw += gl_buf[k].w;
			int tx = x;
			if (ln > 0) { Col wc = base; if (cur) gfill((Rect){tx, y, 2, 1}, base); gs(r, tx, y, "↳ ", wc); tx += 2; }
			if (cur) gfill((Rect){tx, y, lw, 1}, base);
			for (size_t k = from; k < to; k++) { gput(r, tx, y, gl_buf[k].ch, gl_buf[k].c); tx += gl_buf[k].w; }
		}
		used += lines;
	}
	if (U.nhits < 20000) {                                 /* scrollbar, measured in rows */
		size_t total_rows = 0, before = 0;
		for (size_t i = 0; i < U.nhits; i++) { int l = item_lines(i, avail); total_rows += (size_t)l; if (i < U.top) before += (size_t)l; }
		if (total_rows > (size_t)r.h) {
			int th = (int)((size_t)r.h * (size_t)r.h / total_rows); if (th < 1) th = 1;
			int tp = (int)((size_t)(r.h - th) * before / (total_rows - (size_t)r.h));
			Col c = col_mk(U.th.c[C_SCROLL].fg, U.th.c[C_FG].bg, 0);
			for (int i = 0; i < th; i++) {
				int row = bottom_up ? r.h - 1 - (tp + i) : tp + i;
				gs((Rect){0, 0, U.g.w, U.g.h}, U.list_padbar ? r.x + r.w : r.x + r.w - 1, r.y + row, "│", c);
			}
		}
	}
}

static void draw_list(Rect r, bool bottom_up)
{
	U.list_rect = r;
	if (r.h <= 0 || r.w <= 0) return;
	ptr_w = str_width(U.o->pointer, strlen(U.o->pointer));
	mrk_w = str_width(U.o->marker, strlen(U.o->marker));
	if (U.o->wrap) { draw_list_wrap(r, bottom_up); return; }
	{                                                     /* keep the cursor inside the window, --scroll-off rows from the edges */
		size_t rows = (size_t)r.h, so = (size_t)U.o->scroll_off;
		if (so > (rows - 1) / 2) so = (rows - 1) / 2;
		if (U.cur < U.top + so) U.top = U.cur >= so ? U.cur - so : 0;
		if (U.cur + so + 1 > U.top + rows) U.top = U.cur + so + 1 - rows;
		if (U.nhits <= rows) U.top = 0;
		else if (U.top > U.nhits - rows) U.top = U.nhits - rows;
	}
	bool bar = U.nhits > (size_t)r.h;
	int avail = r.w - ptr_w - mrk_w - (bar && !U.list_padbar ? 1 : 0);
	for (int i = 0; i < r.h; i++) {
		size_t hi = U.top + (size_t)i;
		if (hi >= U.nhits) break;
		int y = bottom_up ? r.y + r.h - 1 - i : r.y + i;
		draw_item_row(r, y, hi, hi == U.cur, r.x + ptr_w + mrk_w, avail);
	}
	if (bar) {
		Rect br = r; br.w -= 0;
		int th = (int)((size_t)r.h * (size_t)r.h / U.nhits); if (th < 1) th = 1;
		int tp = (int)((size_t)(r.h - th) * U.top / (U.nhits - (size_t)r.h));
		Col c = col_mk(U.th.c[C_SCROLL].fg, U.th.c[C_FG].bg, 0);
		for (int i = 0; i < th; i++) {
			int row = bottom_up ? r.h - 1 - (tp + i) : tp + i;
			gs((Rect){0, 0, U.g.w, U.g.h}, U.list_padbar ? r.x + r.w : r.x + r.w - 1, r.y + row, "│", c);
		}
	}
	(void)draw_scrollbar;
}

/* ------------------------------------------------------------------ preview */
static void pv_line(size_t i, const char **s, size_t *n)
{
	*s = U.pv_out.p + U.pv_lines[i];
	size_t e = (i + 1 < U.pv_nl) ? U.pv_lines[i + 1] : U.pv_out.len;
	if (e > U.pv_lines[i] && U.pv_out.p[e - 1] == '\n') e--;
	if (e > U.pv_lines[i] && U.pv_out.p[e - 1] == '\r') e--;
	*n = e - U.pv_lines[i];
}

static void draw_preview(Rect outer)
{
	U.pv_rect = outer;
	Col pfg = col_mix(col_mk(COL_DEF, U.th.c[C_FG].bg, 0), U.th.c[C_PFG]);
	if (U.th.c[C_PFG].bg != COL_DEF) pfg.bg = U.th.c[C_PFG].bg;
	Col bc = col_mix(col_mk(COL_DEF, pfg.bg, 0), U.th.c[C_PBORDER]);
	Rect in = border_inner(outer, U.pw_active.border);
	gfill(outer, pfg);
	draw_border(outer, U.pw_active.border, bc, U.preview_label, U.o->border_label_pos);
	if (in.w <= 0 || in.h <= 0) { U.pv_rect = in; return; }
	U.pv_rect = in;
	int hl = U.pw_active.header_lines;
	if ((size_t)hl > U.pv_nl) hl = (int)U.pv_nl;
	int y = in.y, room = in.h;
	bool wrap = U.pv_wrap;
	size_t total = U.pv_nl;
	/* fixed header lines first */
	for (int i = 0; i < hl && room > 0; i++, y++, room--) {
		const char *s; size_t n;
		pv_line((size_t)i, &s, &n);
		draw_ansi(in, in.x, y, in.w, s, n, pfg);
	}
	size_t line = (size_t)hl + (size_t)U.pv_scroll;
	int maxs = (int)total - hl - room;
	if (maxs < 0) maxs = 0;
	if (U.pv_scroll > maxs && !wrap) U.pv_scroll = maxs;
	line = (size_t)hl + (size_t)U.pv_scroll;
	for (; line < total && room > 0; line++) {
		const char *s; size_t n;
		pv_line(line, &s, &n);
		if (!wrap) {
			Buf b; buf_init(&b);
			AnsiSpan *sp = NULL; uint32_t ns = 0, cap = 0;
			ansi_extract(s, n, &b, &sp, &ns, &cap);
			int tw;
			size_t gn = glyphs_of(b.p ? b.p : "", b.len, sp, ns, NULL, 0, pfg, pfg, U.o->tabstop, &tw);
			int off = U.pv_hscroll;
			size_t from = 0; int acc = 0;
			while (from < gn && acc < off) acc += gl_buf[from++].w;
			int cx = in.x;
			for (size_t k = from; k < gn && cx - in.x + gl_buf[k].w <= in.w; k++) { gput(in, cx, y, gl_buf[k].ch, gl_buf[k].c); cx += gl_buf[k].w; }
			free(sp); buf_free(&b);
			y++; room--;
		} else {
			Buf b; buf_init(&b);
			AnsiSpan *sp = NULL; uint32_t ns = 0, cap = 0;
			ansi_extract(s, n, &b, &sp, &ns, &cap);
			int tw;
			size_t gn = glyphs_of(b.p ? b.p : "", b.len, sp, ns, NULL, 0, pfg, pfg, U.o->tabstop, &tw);
			size_t k = 0;
			do {
				int cx = in.x;
				while (k < gn && cx - in.x + gl_buf[k].w <= in.w) { gput(in, cx, y, gl_buf[k].ch, gl_buf[k].c); cx += gl_buf[k].w; k++; }
				y++; room--;
			} while (k < gn && room > 0);
			free(sp); buf_free(&b);
		}
	}
	if (total > (size_t)in.h) {
		int t, rr, bb, l;
		border_edges(U.pw_active.border, &t, &rr, &bb, &l);
		int bx = rr ? in.x + in.w : in.x + in.w - 1;               /* the padding column next to the border */
		int th = (int)((size_t)in.h * (size_t)in.h / total);
		if (th < 1) th = 1;
		int maxs = (int)total - in.h;
		int tp = maxs > 0 ? (int)((size_t)(in.h - th) * (size_t)(hl + U.pv_scroll) / (size_t)maxs) : 0;
		Col sc = col_mk(U.th.c[C_PSCROLL].fg, pfg.bg, 0);
		for (int i = 0; i < th; i++) gs((Rect){0, 0, U.g.w, U.g.h}, bx, in.y + tp + i, "│", sc);
		if (U.pw_active.info) {
			char buf[48];
			snprintf(buf, sizeof buf, "%zu/%zu", (size_t)(hl + U.pv_scroll + 1), total);
			Col ic = col_mk(U.th.c[C_INFO].fg, pfg.bg, U.th.c[C_INFO].attr | ATTR_REV);
			int end = rr ? in.x + in.w : in.x + in.w - 1;
			gs((Rect){0, 0, U.g.w, U.g.h}, end - (int)strlen(buf), in.y, buf, ic);
		}
	}
}

/* ------------------------------------------------------------------ frame */
static PW *pw_pick(int total_cols)
{
	PW *p = &U.pw;
	if (p->alt && total_cols < p->threshold) return p->alt;
	return p;
}

static int size_of(int total, int size, bool pct)
{
	int v = pct ? total * size / 100 : size;
	if (v < 1) v = 1;
	if (v > total) v = total;
	return v;
}

static int text_lines(const char *s)
{
	if (!s || !*s) return 0;
	int n = 1;
	for (; *s; s++) if (*s == '\n') n++;
	return n;
}

static void draw_query_line(Rect r, int *cursor_x, int *cursor_y, bool with_info, const char *info)
{
	U.prompt_rect = r;
	Col pc = col_mk(U.th.c[C_PROMPT].fg, U.th.c[C_FG].bg, U.th.c[C_PROMPT].attr);
	Col qc = col_mk(U.th.c[C_QUERY].fg, U.th.c[C_FG].bg, U.th.c[C_QUERY].attr);
	Col fill = col_mk(COL_DEF, U.th.c[C_FG].bg, 0);
	gfill(r, fill);
	if (U.hide_input || U.o->no_input) return;
	int pw = 0;
	for (const char *p = U.prompt; *p;) {          /* blanks of the prompt are not bold */
		rune ch; int l = u8dec(p, strlen(p), &ch);
		Col c1 = pc;
		if (ch == ' ') c1.attr &= ~ATTR_BOLD;
		pw += gstr(r, r.x + pw, r.y, p, (size_t)l, c1, 1 << 20);
		p += l;
	}
	/* query: scroll horizontally when it does not fit */
	int qw = 0;
	for (int i = 0; i < U.qn; i++) qw += rune_width(U.q[i]) ? rune_width(U.q[i]) : 0;
	int room = r.w - pw - 1;
	int skip = 0, cw = 0;
	for (int i = 0; i < U.qc; i++) cw += rune_width(U.q[i]);
	if (cw > room - 1) skip = cw - (room - 1);
	int x = r.x + pw, acc = 0, curx = r.x + pw;
	for (int i = 0; i < U.qn; i++) {
		int w = rune_width(U.q[i]);
		if (acc >= skip && x - r.x + w <= r.w) { gput(r, x, r.y, U.q[i], qc); x += w; }
		acc += w;
		if (i + 1 == U.qc) curx = x;
	}
	if (U.qc == 0) curx = r.x + pw;
	*cursor_x = curx; *cursor_y = r.y;
	if (U.qn == 0 && U.ghost && *U.ghost) {
		Col gc = col_mk(U.th.c[C_GHOST].fg, U.th.c[C_FG].bg, U.th.c[C_GHOST].attr);
		gstr(r, r.x + pw, r.y, U.ghost, strlen(U.ghost), gc, room);
	}
	(void)qw;
	if (with_info && info) {                       /* inline styles: the info follows the query on the same line */
		int ix = r.x + pw + (U.qn ? (x - r.x - pw) : 0) + 1;
		const char *prefix = U.o->info == INFO_INLINE ? " < " : "";
		Col ic = col_mk(U.th.c[C_INFO].fg, U.th.c[C_FG].bg, U.th.c[C_INFO].attr);
		Col sc = col_mk(U.th.c[C_SEP].fg, U.th.c[C_FG].bg, 0);
		if (U.o->info == INFO_INLINE_RIGHT) ix = r.x + r.w - (int)strlen(info) - 2;
		if (ix < r.x + pw + 1) ix = r.x + pw + 1;
		int used = gs(r, ix, r.y, prefix, U.feed_done ? pc : col_mk(U.th.c[C_SPINNER].fg, U.th.c[C_FG].bg, 0));
		ix += used;
		used = gstr(r, ix, r.y, info, strlen(info), ic, r.x + r.w - ix - 1);
		ix += used + 1;
		if (U.o->info == INFO_INLINE && U.info_rule) for (int k = ix; k < r.x + r.w - (U.list_padbar ? 0 : 1); k++) gs(r, k, r.y, "─", sc);
	}
}

static void build_info(char *out, size_t cap)
{
	size_t total = U.items.n;
	size_t found = U.nhits;
	if (total < found) total = found;
	int n = snprintf(out, cap, "%zu/%zu", found, total);
	if (U.o->multi) {
		if (U.o->multi_max) n += snprintf(out + n, cap - (size_t)n, " (%zu/%d)", U.nsel, U.o->multi_max);
		else n += snprintf(out + n, cap - (size_t)n, " (%zu)", U.nsel);
	}
	if (!U.sort && U.o->sort == false) { /* --no-sort from the start is not shown */ }
	if (U.track_on) n += snprintf(out + n, cap - (size_t)n, " +T");
	(void)n;
}

static void draw_info_line(Rect r)
{
	U.info_rect = r;
	Col fill = col_mk(COL_DEF, U.th.c[C_FG].bg, 0);
	gfill(r, fill);
	char info[96];
	build_info(info, sizeof info);
	Col ic = col_mk(U.th.c[C_INFO].fg, fill.bg, U.th.c[C_INFO].attr);
	Col sc = col_mk(U.th.c[C_SEP].fg, fill.bg, 0);
	Col spin = col_mk(U.th.c[C_SPINNER].fg, fill.bg, U.th.c[C_SPINNER].attr);
	int x = r.x;
	if (U.o->info == INFO_HIDDEN) { for (int k = 0; U.info_rule && k < r.w; k++) gs(r, r.x + k, r.y, "─", sc); return; }
	bool loading = !U.feed_done;
	if (U.o->info == INFO_RIGHT) {
		int len = (int)strlen(info);
		int start = r.x + r.w - len - 2;
		for (int k = r.x; U.info_rule && k < start; k++) gs(r, k, r.y, "─", sc);
		gs(r, start + 1, r.y, info, ic);
		return;
	}
	static const char *sp[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
	static const char *sa[] = {"|", "/", "-", "\\"};
	gs(r, x, r.y, loading ? (U.o->ascii ? sa[(int)(U.now * 10) % 4] : sp[(int)(U.now * 10) % 10]) : " ", spin);
	x += 2;
	x += gs(r, x, r.y, info, ic) + 1;
	for (int k = x; U.info_rule && k < r.x + r.w - (U.list_padbar ? 0 : 1); k++) gs(r, k, r.y, "─", sc);
}

static void draw_hdr_lines(Rect r, bool bottom_up)
{
	U.hdr_rect = r;
	Col hc = col_mk(U.th.c[C_HEADER].fg, U.th.c[C_HEADER].bg == COL_DEF ? U.th.c[C_FG].bg : U.th.c[C_HEADER].bg, U.th.c[C_HEADER].attr);
	gfill(r, hc);
	int ind = U.o->header_border != BORDER_NONE ? 0 : ptr_w + mrk_w, y = r.y;
	bool rev = U.o->layout != LAYOUT_REVERSE;              /* default and reverse-list read the --header-lines bottom-up */
	for (int pass = 0; pass < 2; pass++) {
		bool items = rev ? pass == 0 : pass == 1;
		if (!items) {
			if (!U.header) continue;
			const char *p = U.header;
			while (*p && y < r.y + r.h) {
				const char *e = strchr(p, '\n');
				size_t n = e ? (size_t)(e - p) : strlen(p);
				draw_ansi(r, r.x + ind, y++, r.w - ind, p, n, hc);
				if (!e) break;
				p = e + 1;
			}
			continue;
		}
		for (size_t k = 0; k < U.hdr.n && y < r.y + r.h; k++) {
			Item *it = U.hdr.v[rev ? U.hdr.n - 1 - k : k];
			int total;
			size_t n = glyphs_of(it->text, it->tlen, U.o->ansi ? it->ansi : NULL, U.o->ansi ? it->nansi : 0, NULL, 0, hc, hc, U.o->tabstop, &total);
			draw_glyphs(r, r.x + ind, y++, r.w - ind, n, total, 0, false, hc);
		}
	}
	(void)bottom_up;
}

static void draw_footer(Rect r)
{
	Col fc = col_mk(U.th.c[C_FOOTER].fg, U.th.c[C_FOOTER].bg == COL_DEF ? U.th.c[C_FG].bg : U.th.c[C_FOOTER].bg, U.th.c[C_FOOTER].attr);
	gfill(r, fc);
	int ind = U.o->footer_border > 0 ? 0 : ptr_w + mrk_w, y = r.y;
	const char *p = U.footer;
	while (p && *p && y < r.y + r.h) {
		const char *e = strchr(p, '\n');
		size_t n = e ? (size_t)(e - p) : strlen(p);
		draw_ansi(r, r.x + ind, y++, r.w - ind, p, n, fc);
		if (!e) break;
		p = e + 1;
	}
}

static void draw_main(Rect m, int *cx, int *cy)
{
	Opts *o = U.o;
	bool prompt_top = o->layout == LAYOUT_REVERSE;
	bool bottom_up = o->layout == LAYOUT_DEFAULT;
	ptr_w = str_width(o->pointer, strlen(o->pointer));
	mrk_w = str_width(o->marker, strlen(o->marker));
	bool input_on = !o->no_input && !U.hide_input;
	bool inline_info = o->info == INFO_INLINE || o->info == INFO_INLINE_RIGHT;
	int input_h = input_on ? 1 : 0;
	int info_h = (o->info == INFO_HIDDEN && !input_on) ? 0 : (inline_info ? 0 : 1);
	if (o->info == INFO_HIDDEN) info_h = 1;
	if (!input_on) info_h = 0;                          /* the info line belongs to the input */
	int hdr_h = text_lines(U.header) + (int)U.hdr.n;
	int ftr_h = text_lines(U.footer);
	int fshape = o->footer_border > 0 ? o->footer_border : BORDER_NONE;
	int sep_f = (ftr_h && o->footer_border < 0) ? 1 : 0;            /* automatic: a rule under the footer */
	int in_h = input_h + info_h;
	int et, er, eb, el;
	#define EXTRA(shape) (border_edges(shape, &et, &er, &eb, &el) ? et + eb : 0)
	/* the sections in reading order, top to bottom: id, inner height, own border */
	enum { S_F, S_SEP, S_L, S_H, S_I };
	struct { int id, h, shape, ex; Rect r; bool in; } sec[5];
	int ns = 0;
	#define ADD(ID, H, SHAPE) do { sec[ns].id = ID; sec[ns].h = H; sec[ns].shape = SHAPE; sec[ns].ex = EXTRA(SHAPE); sec[ns].in = false; ns++; } while (0)
	if (!prompt_top) {
		if (ftr_h) ADD(S_F, ftr_h, fshape);
		if (sep_f) ADD(S_SEP, 1, BORDER_NONE);
		ADD(S_L, 0, BORDER_NONE);
		if (hdr_h && !o->header_first) ADD(S_H, hdr_h, o->header_border);
		if (in_h) ADD(S_I, in_h, o->input_border);
		if (hdr_h && o->header_first) ADD(S_H, hdr_h, o->header_border);
	} else {
		if (hdr_h && o->header_first) ADD(S_H, hdr_h, o->header_border);
		if (in_h) ADD(S_I, in_h, o->input_border);
		if (hdr_h && !o->header_first) ADD(S_H, hdr_h, o->header_border);
		ADD(S_L, 0, BORDER_NONE);
		if (sep_f) ADD(S_SEP, 1, BORDER_NONE);
		if (ftr_h) ADD(S_F, ftr_h, fshape);
	}
	/* a list border also encloses the neighbouring sections that have no border of their own */
	int li = 0;
	for (int i = 0; i < ns; i++) if (sec[i].id == S_L) li = i;
	int g0 = li, g1 = li;
	if (o->list_border != BORDER_NONE) {
		while (g0 > 0 && sec[g0 - 1].id != S_SEP && sec[g0 - 1].shape == BORDER_NONE) g0--;
		while (g1 + 1 < ns && sec[g1 + 1].id != S_SEP && sec[g1 + 1].shape == BORDER_NONE) g1++;
	}
	int lt, lr_, lb, ll;
	int lgex = border_edges(o->list_border, &lt, &lr_, &lb, &ll) ? lt + lb : 0;
	int used = lgex;
	for (int i = 0; i < ns; i++) used += sec[i].h + sec[i].ex;
	int list_h = m.h - used;
	if (list_h < 0) {                                  /* squeeze the header first */
		for (int i = 0; i < ns && list_h < 0; i++) if (sec[i].id == S_H) {
			int cut = -list_h < sec[i].h ? -list_h : sec[i].h;
			sec[i].h -= cut; list_h += cut;
			if (!sec[i].h) { list_h += sec[i].ex; sec[i].ex = 0; }
		}
		if (list_h < 0) list_h = 0;
	}
	sec[li].h = list_h;
	Rect gin = border_inner((Rect){m.x, m.y, m.w, m.h}, o->list_border);   /* horizontal inset of the enclosed sections */
	int y = m.y, gy0 = m.y;
	for (int i = 0; i < ns; i++) {
		bool inside = o->list_border != BORDER_NONE && i >= g0 && i <= g1;
		if (inside && i == g0) { gy0 = y; y += lt; }
		int x = inside ? gin.x : m.x, w = inside ? gin.w : m.w;
		sec[i].r = (Rect){x, y, w, sec[i].h + sec[i].ex};
		y += sec[i].h + sec[i].ex;
		if (inside && i == g1) {
			Col gbc = col_mk(U.th.c[C_BORDER].fg, U.th.c[C_FG].bg, 0);
			y += lb;
			draw_border((Rect){m.x, gy0, m.w, y - gy0}, o->list_border, gbc, NULL, 0);
		}
	}
	Col sbc = col_mk(U.th.c[C_BORDER].fg, U.th.c[C_FG].bg, 0);
	Rect fr = {0}, sepr = {0}, hr = {0}, lr = {0}, ir = {0}, pr = {0};
	for (int i = 0; i < ns; i++) {
		Rect o_ = sec[i].r, inner = border_inner(o_, sec[i].shape);
		if (sec[i].id != S_L) draw_border(o_, sec[i].shape, sbc, NULL, 0);
		switch (sec[i].id) {
		case S_F: fr = inner; break;
		case S_SEP: sepr = o_; break;
		case S_H: hr = inner; break;
		case S_L: lr = o_; break;
		case S_I:
			if (!prompt_top) { ir = (Rect){inner.x, inner.y, inner.w, info_h}; pr = (Rect){inner.x, inner.y + info_h, inner.w, input_h}; }
			else { pr = (Rect){inner.x, inner.y, inner.w, input_h}; ir = (Rect){inner.x, inner.y + input_h, inner.w, info_h}; }
			break;
		}
	}
	U.list_padbar = o->list_border != BORDER_NONE && (ll || lr_);
	U.info_rule = o->input_border == BORDER_NONE && (!hdr_h || o->header_border == BORDER_NONE);
	draw_list(lr, bottom_up);
	if (hr.h > 0) draw_hdr_lines(hr, bottom_up); else U.hdr_rect = (Rect){0, 0, 0, 0};
	if (ftr_h) {
		draw_footer(fr);
		Col sc = col_mk(U.th.c[C_BORDER].fg, U.th.c[C_FG].bg, 0);
		for (int k = 0; sep_f && k < sepr.w; k++) gs(sepr, sepr.x + k, sepr.y, "─", sc);
	}
	char info[96];
	build_info(info, sizeof info);
	*cx = *cy = -1;
	if (input_on) draw_query_line(pr, cx, cy, inline_info, info);
	else U.prompt_rect = (Rect){0, 0, 0, 0};
	if (info_h) draw_info_line(ir);
	else U.info_rect = (Rect){0, 0, 0, 0};
}

void draw_all(void)
{
	Opts *o = U.o;
	grid_clear();
	Rect whole = {0, 0, U.g.w, U.g.h};
	Rect a = whole;
	int mg[4];
	for (int i = 0; i < 4; i++) {
		int v = o->margin[i];
		int base = (i == 0 || i == 2) ? a.h : a.w;
		mg[i] = v < 0 ? base * (-v - 1) / 100 : v;
	}
	a.x += mg[3]; a.w -= mg[1] + mg[3]; a.y += mg[0]; a.h -= mg[0] + mg[2];
	if (a.w < 1 || a.h < 1) return;
	Col bc = col_mk(U.th.c[C_BORDER].fg, U.th.c[C_FG].bg, 0);
	draw_border(a, o->border, bc, U.border_label, o->border_label_pos);
	Rect in = border_inner(a, o->border);
	int pd[4];
	for (int i = 0; i < 4; i++) {
		int v = o->padding[i];
		int base = (i == 0 || i == 2) ? in.h : in.w;
		pd[i] = v < 0 ? base * (-v - 1) / 100 : v;
	}
	in.x += pd[3]; in.w -= pd[1] + pd[3]; in.y += pd[0]; in.h -= pd[0] + pd[2];
	if (in.w < 1 || in.h < 1) return;
	int hr = hints_rows();
	if (hr && in.h > hr + 5) { in.h -= hr; hints_draw((Rect){in.x, in.y + in.h, in.w, hr}); }
	else U.hint_rows = 0;
	U.area = in;
	Rect m = in;
	bool have_pv = U.pv_cmd != NULL && !U.pv_hidden;
	U.pv_rect = (Rect){0, 0, 0, 0};
	Rect pvr = {0};
	if (have_pv) {
		U.pw_active = *pw_pick(in.w);
		PW *p = &U.pw_active;
		if (p->pos == PW_RIGHT || p->pos == PW_LEFT) {
			int w = size_of(in.w, p->size, p->pct);
			if (w > in.w - 3) w = in.w - 3 > 0 ? in.w - 3 : in.w;
			pvr = (Rect){p->pos == PW_RIGHT ? in.x + in.w - w : in.x, in.y, w, in.h};
			m.w = in.w - w;
			if (p->pos == PW_LEFT) m.x = in.x + w;
		} else {
			int h = size_of(in.h, p->size, p->pct);
			if (h > in.h - 3) h = in.h - 3 > 0 ? in.h - 3 : in.h;
			pvr = (Rect){in.x, p->pos == PW_DOWN ? in.y + in.h - h : in.y, in.w, h};
			m.h = in.h - h;
			if (p->pos == PW_UP) m.y = in.y + h;
		}
	}
	int cx, cy;
	draw_main(m, &cx, &cy);
	if (have_pv) draw_preview(pvr);
	U.pw.hidden = U.pv_hidden;
	/* the cursor */
	term_outs("");
	static int lastx = -1, lasty = -1;
	lastx = cx; lasty = cy;
	U.mouse_last_x = cx; U.mouse_last_y = cy;
}

/* ------------------------------------------------------------------ output */
static Col last_c;
void present(void)
{
	Buf b;
	buf_init(&b);
	buf_adds(&b, "\033[?2026h\033[?25l");
	bool first = true;
	for (int y = 0; y < U.g.h; y++) {
		buf_printf(&b, "\033[%d;1H", U.top_row + y + 1);
		first = true;
		for (int x = 0; x < U.g.w; x++) {
			Cell *c = &U.g.cell[y * U.g.w + x];
			if (c->ch == 0) continue;
			if (first || c->c.fg != last_c.fg || c->c.bg != last_c.bg || c->c.attr != last_c.attr) { sgr_emit(&b, c->c); last_c = c->c; first = false; }
			char u[4];
			size_t n = u8enc(c->ch, u);
			buf_addn(&b, u, n);
		}
	}
	buf_adds(&b, "\033[0m");
	if (U.mouse_last_x >= 0 && !U.hide_input && !U.o->no_input)
		buf_printf(&b, "\033[%d;%dH\033[?25h", U.top_row + U.mouse_last_y + 1, U.mouse_last_x + 1);
	buf_adds(&b, "\033[?2026l");
	term_out(b.p, b.len);
	term_flush();
	buf_free(&b);
}

const char *pw_default_spec = "right,50%,border-rounded";

/* ======== ui.c ======== */
/* ui.c - the interactive fzf: start-up, the event loop, search results, cursor and selection. */
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
static void ui_print_item_(const Item *it)
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
	if (U.nsel) { for (size_t i = 0; i < U.nsel; i++) ui_print_item_(U.sel[i]); }
	else { Item *c = ui_current(); if (c) ui_print_item_(c); }
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

/* ======== act.c ======== */
/* act.c - what an action does: cursor and selection moves, query editing, preview control, running commands. */
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
static void act_set_str_(char **d, const char *v) { free(*d); *d = xstrdup(v); }
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
	case A_CHANGE_PROMPT: act_set_str_(&U.prompt, arg); break;
	case A_TRANSFORM_PROMPT: { char *s = run_transform(arg); act_set_str_(&U.prompt, s); free(s); break; }
	case A_CHANGE_HEADER: act_set_str_(&U.header, arg); break;
	case A_TRANSFORM_HEADER: { char *s = run_transform(arg); act_set_str_(&U.header, s); free(s); break; }
	case A_CHANGE_FOOTER: act_set_str_(&U.footer, arg); break;
	case A_TRANSFORM_FOOTER: { char *s = run_transform(arg); act_set_str_(&U.footer, s); free(s); break; }
	case A_CHANGE_BORDER_LABEL: act_set_str_(&U.border_label, arg); break;
	case A_TRANSFORM_BORDER_LABEL: { char *s = run_transform(arg); act_set_str_(&U.border_label, s); free(s); break; }
	case A_CHANGE_PREVIEW_LABEL: act_set_str_(&U.preview_label, arg); break;
	case A_TRANSFORM_PREVIEW_LABEL: { char *s = run_transform(arg); act_set_str_(&U.preview_label, s); free(s); break; }
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
	case A_CHANGE_PREVIEW: act_set_str_(&U.pv_cmd, arg); pv_schedule(); break;
	case A_PREVIEW: act_set_str_(&U.pv_cmd, arg); if (U.pv_hidden) U.pv_hidden = false; pv_schedule(); break;
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

/* ======== proc.c ======== */
/* proc.c - everything that talks to other processes: the input feed and reload, the preview command, execute/become,
 * placeholders in command templates ({}, {+}, {q}, {n}, {1..2}), the history file. */
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>

extern void ui_items_clear(void);
extern void ui_after_feed(void);

/* ------------------------------------------------------------------ processes */
static const char *shell_of(void)
{
	if (U.o->with_shell && *U.o->with_shell) return U.o->with_shell;
	const char *s = getenv("SHELL");
	return (s && *s) ? s : "sh";
}

char *env_setup(void)
{
	char b[64];
	int rows, cols;
	term_size(&rows, &cols);
	snprintf(b, sizeof b, "%d", U.g.h ? U.g.h : rows); setenv("FZF_LINES", b, 1);
	snprintf(b, sizeof b, "%d", U.g.w ? U.g.w : cols); setenv("FZF_COLUMNS", b, 1);
	snprintf(b, sizeof b, "%d", U.pv_rect.h); setenv("FZF_PREVIEW_LINES", b, 1);
	snprintf(b, sizeof b, "%d", U.pv_rect.w); setenv("FZF_PREVIEW_COLUMNS", b, 1);
	char *q = ui_query_str();
	setenv("FZF_QUERY", q, 1);
	free(q);
	setenv("FZF_PROMPT", U.prompt, 1);
	snprintf(b, sizeof b, "%zu", U.nhits); setenv("FZF_MATCH_COUNT", b, 1);
	snprintf(b, sizeof b, "%zu", U.items.n); setenv("FZF_TOTAL_COUNT", b, 1);
	snprintf(b, sizeof b, "%zu", U.nsel); setenv("FZF_SELECT_COUNT", b, 1);
	snprintf(b, sizeof b, "%zu", U.nhits ? U.cur + 1 : 0); setenv("FZF_POS", b, 1);
	Item *it = ui_current();
	if (it) { snprintf(b, sizeof b, "%u", it->index); setenv("FZF_CURRENT_INDEX", b, 1); } else unsetenv("FZF_CURRENT_INDEX");
	setenv("FZF_INPUT_STATE", U.hide_input ? "hidden" : "enabled", 1);
	setenv("FZF_KEY", U.last_key ? U.last_key : "", 1);
	setenv("FZF_ACTION", U.last_key ? U.last_key : "", 1);
	setenv("FZF_PORT", U.listen_port ? U.listen_port : "", 1);
	setenv("FZF_LAYOUT", U.o->layout == LAYOUT_REVERSE ? "reverse" : U.o->layout == LAYOUT_REVERSE_LIST ? "reverse-list" : "default", 1);
	return NULL;
}

/* runs cmd through the shell with its stdout+stderr on a pipe we read; stdin is /dev/null */
bool proc_start(Proc *p, const char *cmd, int stdin_fd)
{
	int fds[2];
	if (pipe(fds) < 0) return false;
	env_setup();
	pid_t pid = fork();
	if (pid < 0) { close(fds[0]); close(fds[1]); return false; }
	if (pid == 0) {
		setpgid(0, 0);
		int in = stdin_fd >= 0 ? stdin_fd : open("/dev/null", O_RDONLY);
		dup2(in, 0);
		dup2(fds[1], 1);
		dup2(fds[1], 2);
		close(fds[0]); close(fds[1]);
		signal(SIGPIPE, SIG_DFL);
		execlp(shell_of(), shell_of(), "-c", cmd, (char *)NULL);
		_exit(127);
	}
	setpgid(pid, pid);
	close(fds[1]);
	fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
	fcntl(fds[0], F_SETFD, FD_CLOEXEC);
	p->fd = fds[0]; p->pid = pid; p->live = true;
	return true;
}

void proc_kill(Proc *p)
{
	if (!p->live) return;
	kill(-p->pid, SIGKILL);
	kill(p->pid, SIGKILL);
	waitpid(p->pid, NULL, 0);
	if (p->fd >= 0) close(p->fd);
	p->live = false; p->fd = -1;
}

void proc_reap(Proc *p)
{
	if (!p->live) return;
	waitpid(p->pid, NULL, WNOHANG);
	if (p->fd >= 0) close(p->fd);
	p->live = false; p->fd = -1;
	waitpid(p->pid, NULL, 0);
}

int proc_run_capture(const char *cmd, Buf *out)
{
	Proc p = {-1, 0, false};
	if (!proc_start(&p, cmd, -1)) return -1;
	char tmp[8192];
	for (;;) {
		struct pollfd pf = {p.fd, POLLIN, 0};
		poll(&pf, 1, 5000);
		ssize_t n = read(p.fd, tmp, sizeof tmp);
		if (n > 0) buf_addn(out, tmp, (size_t)n);
		else if (n == 0 || (errno != EAGAIN && errno != EINTR)) break;
	}
	int st = 0;
	close(p.fd);
	waitpid(p.pid, &st, 0);
	return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

void proc_execute(const char *cmd, bool silent)
{
	env_setup();
	if (silent) {
		pid_t pid = fork();
		if (pid == 0) {
			setsid();
			int dn = open("/dev/null", O_RDWR);
			dup2(dn, 0); dup2(dn, 1); dup2(dn, 2);
			execlp(shell_of(), shell_of(), "-c", cmd, (char *)NULL);
			_exit(127);
		}
		if (pid > 0) waitpid(pid, NULL, 0);
		return;
	}
	term_close();
	pid_t pid = fork();
	if (pid == 0) {
		int t = open("/dev/tty", O_RDWR);
		if (t >= 0) { dup2(t, 0); dup2(t, 1); dup2(t, 2); }
		execlp(shell_of(), shell_of(), "-c", cmd, (char *)NULL);
		_exit(127);
	}
	if (pid > 0) waitpid(pid, NULL, 0);
	term_open(U.fullscreen, U.o->mouse);
	ui_resize();
}

void proc_become(const char *cmd)
{
	env_setup();
	term_close();
	int t = open("/dev/tty", O_RDWR);
	if (t >= 0) { dup2(t, 0); dup2(t, 2); }
	execlp(shell_of(), shell_of(), "-c", cmd, (char *)NULL);
	fprintf(stderr, "fzf: become: cannot run %s\n", cmd);
	_exit(127);
}

/* ------------------------------------------------------------------ placeholders */
char *proc_quote(const char *s)
{
	Buf b;
	buf_init(&b);
	buf_addc(&b, '\'');
	for (; *s; s++) {
		if (*s == '\'') buf_adds(&b, "'\\''");
		else buf_addc(&b, *s);
	}
	buf_addc(&b, '\'');
	return buf_take(&b);
}

static char *item_line(const Item *it)
{
	if (U.o->ansi) {
		Buf b;
		buf_init(&b);
		ansi_extract(it->orig, it->olen, &b, NULL, NULL, NULL);
		return buf_take(&b);
	}
	return xstrndup(it->orig, it->olen);
}

static char *trim_blanks(char *s)
{
	size_t n = strlen(s), a = 0;
	while (a < n && (s[a] == ' ' || s[a] == '\t')) a++;
	while (n > a && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
	char *r = xstrndup(s + a, n - a);
	free(s);
	return r;
}

/* the fields of the item's original line selected by spec ("1", "2..", "-1", "1,3") */
static char *item_fields(const Item *it, const char *spec, bool keep_space)
{
	Range *rg = NULL;
	int n = ranges_parse(spec, &rg);
	if (n <= 0) { free(rg); return xstrdup(""); }
	Opts tmp = *U.o;
	tmp.with_nth = rg; tmp.nwith = n; tmp.with_tmpl = NULL; tmp.ansi = U.o->ansi;
	Item *t = item_new(&tmp, it->orig, it->olen);
	char *r = xstrndup(t->text, t->tlen);
	item_free(t);
	free(rg);
	return keep_space ? r : trim_blanks(r);
}

static void append_items(Buf *out, Item **v, size_t n, const char *fields, bool idx, bool raw, bool keep_space, bool tmpfile)
{
	Buf all;
	buf_init(&all);
	for (size_t i = 0; i < n; i++) {
		char *s;
		char nb[24];
		if (idx) { snprintf(nb, sizeof nb, "%u", v[i]->index); s = xstrdup(nb); }
		else if (fields && *fields) s = item_fields(v[i], fields, keep_space);
		else s = item_line(v[i]);
		if (i) buf_addc(&all, tmpfile ? '\n' : ' ');
		if (tmpfile) buf_adds(&all, s);
		else if (raw) buf_adds(&all, s);
		else { char *q = proc_quote(s); buf_adds(&all, q); free(q); }
		free(s);
	}
	if (tmpfile) {
		char path[] = "/tmp/fzf-preview-XXXXXX";
		int fd = mkstemp(path);
		if (fd >= 0) { if (write(fd, all.p ? all.p : "", all.len) < 0) {} close(fd); }
		char *q = proc_quote(path);
		buf_adds(out, q);
		free(q);
	} else buf_addn(out, all.p ? all.p : "", all.len);
	buf_free(&all);
}

char *expand_template(const char *t, bool for_preview)
{
	(void)for_preview;
	Buf out;
	buf_init(&out);
	Item *cur = ui_current();
	for (const char *p = t; *p; p++) {
		if (*p == '\\' && p[1] == '{') { buf_addc(&out, '{'); p++; continue; }
		if (*p != '{') { buf_addc(&out, *p); continue; }
		const char *e = strchr(p, '}');
		if (!e) { buf_addc(&out, *p); continue; }
		/* nested braces inside the field spec are not allowed, so the first '}' ends the placeholder */
		char *body = xstrndup(p + 1, (size_t)(e - p - 1));
		const char *s = body;
		bool plus = false, raw = false, keep = false, file = false, idx = false;
		for (; *s == '+' || *s == 'r' || *s == 's' || *s == 'f' || *s == 'n'; s++) {
			if (*s == '+') plus = true;
			else if (*s == 'r') raw = true;
			else if (*s == 's') keep = true;
			else if (*s == 'f') file = true;
			else if (*s == 'n') { idx = true; if (s[1]) { /* "{n}" vs "{nfoo}" */ } else { s++; break; } }
		}
		if (!strcmp(body, "q") || !strcmp(body, "fzf:query")) {
			char *q = ui_query_str();
			char *qq = proc_quote(q);
			buf_adds(&out, qq);
			free(q); free(qq);
		} else if (!strcmp(body, "fzf:action") || !strcmp(body, "fzf:prompt")) {
			char *qq = proc_quote(body[4] == 'a' ? (U.last_key ? U.last_key : "") : U.prompt);
			buf_adds(&out, qq); free(qq);
		} else if (!*s || (s[0] == 'n' && !s[1]) || (idx && !*s)) {
			Item *one[1] = {cur};
			if (plus) {
				if (U.nsel) append_items(&out, U.sel, U.nsel, "", idx, raw, keep, file);
				else if (cur) append_items(&out, one, 1, "", idx, raw, keep, file);
			} else if (cur) append_items(&out, one, 1, "", idx, raw, keep, file);
			else if (!idx) buf_adds(&out, "''");
		} else if (isdigit((unsigned char)*s) || *s == '-' || *s == '.') {
			Item *one[1] = {cur};
			if (plus) {
				if (U.nsel) append_items(&out, U.sel, U.nsel, s, false, raw, keep, file);
				else if (cur) append_items(&out, one, 1, s, false, raw, keep, file);
			} else if (cur) append_items(&out, one, 1, s, false, raw, keep, file);
			else buf_adds(&out, "''");
		} else {
			buf_addn(&out, p, (size_t)(e - p + 1));                    /* unknown: left as it was */
		}
		free(body);
		p = e;
	}
	return buf_take(&out);
}

/* ------------------------------------------------------------------ input feed and reload */
typedef struct { ItemVec *items; ItemVec *hdr; int left; } Sink;
static Sink feed_sink, reload_sink;
static ItemVec reload_items, reload_hdr;
static bool reload_swapped;

static void take_line(Sink *k, const char *s, size_t n)
{
	if (k->left > 0) { itemvec_push(k->hdr, item_new(U.o, s, n)); k->left--; return; }
	itemvec_push(k->items, item_new(U.o, s, n));
}

/* reads what is available from pr, cuts lines; *eof is set at the end of the stream; returns true if it got data */
static bool consume(Proc *pr, Buf *part, Sink *k, bool *eof)
{
	char tmp[65536];
	char sep = U.o->read0 ? 0 : '\n';
	bool any = false;
	for (int rounds = 0; rounds < 8; rounds++) {
		ssize_t n = read(pr->fd, tmp, sizeof tmp);
		if (n < 0) { if (errno == EINTR) continue; if (errno == EAGAIN) return any; *eof = true; break; }
		if (n == 0) { *eof = true; break; }
		any = true;
		size_t start = 0;
		for (size_t i = 0; i < (size_t)n; i++) {
			if (tmp[i] != sep) continue;
			buf_addn(part, tmp + start, i - start);
			take_line(k, part->p ? part->p : "", part->len);
			buf_reset(part);
			start = i + 1;
		}
		buf_addn(part, tmp + start, (size_t)n - start);
	}
	if (*eof && part->len) { take_line(k, part->p, part->len); buf_reset(part); }
	return any;
}

void feed_setup(void)
{
	U.feed.fd = -1; U.feed.live = false;
	buf_init(&U.feed_part);
	feed_sink = (Sink){&U.items, &U.hdr, U.hdr_left};
	if (!isatty(0)) {
		U.feed.fd = 0; U.feed.pid = 0; U.feed.live = true;
		fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);
		return;
	}
	const char *cmd = getenv("FZF_DEFAULT_COMMAND");
	if (!cmd || !*cmd)
		cmd = "command find -L . -mindepth 1 \\( -path '*/.*' -o -fstype 'sysfs' -o -fstype 'devfs' -o -fstype 'devtmpfs' -o -fstype 'proc' \\) -prune "
		      "-o -type f -print -o -type d -print -o -type l -print 2>/dev/null | cut -b3-";
	if (!proc_start(&U.feed, cmd, -1)) { U.feed_done = true; return; }
}

void feed_read(void)
{
	if (!U.feed.live) return;
	bool eof = false;
	bool any = consume(&U.feed, &U.feed_part, &feed_sink, &eof);
	if (any || eof) U.need_search = true;
	if (eof) {
		if (U.feed.pid) proc_reap(&U.feed); else U.feed.live = false;
		U.feed_done = true;
		ui_after_feed();
	}
}

void reload_start(const char *cmd, bool sync)
{
	proc_kill(&U.reload);
	buf_init(&U.reload_part);
	buf_reset(&U.reload_part);
	reload_items = (ItemVec){0}; reload_hdr = (ItemVec){0};
	reload_swapped = false;
	U.reload_sync = sync;
	if (!proc_start(&U.reload, cmd, -1)) return;
	reload_sink = (Sink){&reload_items, &reload_hdr, U.o->header_lines};
	U.feed_done = false;
}

static void reload_swap(void)
{
	ui_items_clear();
	U.items = reload_items; U.hdr = reload_hdr;
	reload_items = (ItemVec){0}; reload_hdr = (ItemVec){0};
	reload_sink.items = &U.items; reload_sink.hdr = &U.hdr;
	reload_swapped = true;
	U.need_search = true;
}

void reload_read(void)
{
	if (!U.reload.live) return;
	bool eof = false;
	bool any = consume(&U.reload, &U.reload_part, &reload_sink, &eof);
	if (any && !U.reload_sync && !reload_swapped) reload_swap();
	else if (any && reload_swapped) U.need_search = true;
	if (eof) {
		proc_reap(&U.reload);
		if (!reload_swapped) reload_swap();
		U.feed_done = true;
		U.need_search = true;
		ui_after_feed();
	}
}

/* ------------------------------------------------------------------ preview */
void pv_kill(void) { proc_kill(&U.pv); }

void pv_schedule(void)
{
	if (!U.pv_cmd || U.pv_hidden) return;
	U.pv_pending = true;
	U.pv_start_at = now_sec() + 0.04;
}

static int scroll_expr(void)
{
	if (!U.pw_active.scroll) return 0;
	char *e = expand_template(U.pw_active.scroll, false);
	int base = -1, height = U.pv_rect.h - U.pw_active.header_lines;
	if (height < 0) height = 0;
	/* strip quotes the expansion added around a number */
	Buf b;
	buf_init(&b);
	for (char *c = e; *c; c++) if (*c != '\'') buf_addc(&b, *c);
	char *s = buf_take(&b);
	free(e);
	for (char *c = s; *c;) {
		if (*c == '+' || *c == '-' || *c == '/') {
			bool neg = false;
			char *st = c;
			if (*c == '-' && c[1] == '/') { c++; }
			if (*c == '/') {
				int d = atoi(c + 1);
				if (d) base -= height / d;
				break;
			}
			if (*st == '-') neg = true;
			long v = strtol(c + 1, &c, 10);
			base += neg ? -(int)v : (int)v;
		} else c++;
	}
	free(s);
	return base < 0 ? 0 : base;
}

void pv_start(void)
{
	U.pv_pending = false;
	proc_kill(&U.pv);
	Item *it = ui_current();
	buf_reset(&U.pv_out);
	U.pv_nl = 0;
	U.pv_scroll = 0; U.pv_hscroll = 0;
	U.pv_done = false;
	if (!U.pv_cmd || U.pv_hidden || !it) { U.pv_done = true; return; }
	U.pv_item = it->index;
	char *cmd = expand_template(U.pv_cmd, true);
	if (!proc_start(&U.pv, cmd, -1)) U.pv_done = true;
	free(cmd);
	U.pv_scroll = scroll_expr();
}

static void pv_index(size_t from)
{
	if (U.pv_nl == 0) {
		if (U.pv_cap == 0) { U.pv_cap = 256; U.pv_lines = xmalloc(U.pv_cap * sizeof(size_t)); }
		U.pv_lines[U.pv_nl++] = 0;
	}
	for (size_t i = from; i < U.pv_out.len; i++) {
		if (U.pv_out.p[i] != '\n') continue;
		if (i + 1 >= U.pv_out.len) break;                  /* a trailing newline does not start a line */
		if (U.pv_nl == U.pv_cap) { U.pv_cap *= 2; U.pv_lines = xrealloc(U.pv_lines, U.pv_cap * sizeof(size_t)); }
		U.pv_lines[U.pv_nl++] = i + 1;
	}
}

void pv_poll(void)
{
	if (!U.pv.live) return;
	char tmp[65536];
	for (int r = 0; r < 16; r++) {
		ssize_t n = read(U.pv.fd, tmp, sizeof tmp);
		if (n > 0) {
			size_t before = U.pv_out.len;
			buf_addn(&U.pv_out, tmp, (size_t)n);
			/* a form feed... clear-screen escape resets the preview (fzf keeps the output after the last one) */
			pv_index(before ? before - 1 : 0);
			if (U.pw_active.follow) U.pv_scroll = 1 << 28;
			continue;
		}
		if (n < 0 && errno == EAGAIN) return;
		if (n < 0 && errno == EINTR) continue;
		proc_reap(&U.pv);
		U.pv_done = true;
		return;
	}
}

int pv_nfds(void) { return U.pv.live ? U.pv.fd : -1; }

void pv_scroll(int lines)
{
	int hl = U.pw_active.header_lines;
	int maxs = (int)U.pv_nl - hl - (U.pv_rect.h - hl);
	if (maxs < 0) maxs = 0;
	long v = (long)U.pv_scroll + lines;
	if (U.pw_active.follow && lines < 0) U.pw_active.follow = false;
	if (v < 0) v = 0;
	if (v > maxs) v = maxs;
	U.pv_scroll = (int)v;
}

void pv_page(int dir, bool half)
{
	int h = U.pv_rect.h - U.pw_active.header_lines;
	if (h < 1) h = 1;
	pv_scroll(dir * (half ? (h + 1) / 2 : h));
}

void pv_toggle(void)
{
	U.pv_hidden = !U.pv_hidden;
	if (U.pv_hidden) { proc_kill(&U.pv); return; }
	pv_schedule();
}

/* ------------------------------------------------------------------ history */
void history_load(void)
{
	if (!U.o->history_file) return;
	FILE *f = fopen(U.o->history_file, "r");
	if (!f) return;
	char *line = NULL;
	size_t cap = 0;
	ssize_t n;
	while ((n = getline(&line, &cap, f)) > 0) {
		if (line[n - 1] == '\n') line[--n] = 0;
		U.hist = xrealloc(U.hist, (size_t)(U.nhist + 1) * sizeof(char *));
		U.hist[U.nhist++] = xstrdup(line);
	}
	free(line);
	fclose(f);
	U.hist_pos = U.nhist;
}

void history_save(const char *q)
{
	if (!U.o->history_file || !*q) return;
	if (U.nhist && !strcmp(U.hist[U.nhist - 1], q)) return;
	U.hist = xrealloc(U.hist, (size_t)(U.nhist + 1) * sizeof(char *));
	U.hist[U.nhist++] = xstrdup(q);
	int from = U.nhist > U.o->history_size ? U.nhist - U.o->history_size : 0;
	FILE *f = fopen(U.o->history_file, "w");
	if (!f) return;
	chmod(U.o->history_file, 0600);
	for (int i = from; i < U.nhist; i++) fprintf(f, "%s\n", U.hist[i]);
	fclose(f);
}

/* ------------------------------------------------------------------ item keys (--track, --id-nth) */
char *item_key(const Item *it)
{
	if (U.o->nid > 0) {
		Opts tmp = *U.o;
		tmp.with_nth = U.o->id_nth; tmp.nwith = U.o->nid; tmp.with_tmpl = NULL;
		Item *t = item_new(&tmp, it->orig, it->olen);
		char *r = xstrndup(t->text, t->tlen);
		item_free(t);
		return r;
	}
	return xstrndup(it->text, it->tlen);
}

/* ------------------------------------------------------------------ --listen: HTTP on localhost, POST = actions, GET = state */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

void listen_start(void)
{
	if (!U.o->listen_addr) return;
	char *spec = xstrdup(U.o->listen_addr);
	char *colon = strrchr(spec, ':');
	const char *host = "127.0.0.1";
	int port = 0;
	if (colon) { *colon = 0; port = atoi(colon + 1); if (*spec && strcmp(spec, "localhost")) host = spec; }
	else port = atoi(spec);
	int s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) die("cannot create the listen socket");
	int one = 1;
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	struct sockaddr_in a = {0};
	a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port);
	if (inet_pton(AF_INET, host, &a.sin_addr) != 1) die("invalid listen address: %s", U.o->listen_addr);
	if (bind(s, (struct sockaddr *)&a, sizeof a) < 0 || listen(s, 8) < 0) die("cannot listen on %s", U.o->listen_addr);
	socklen_t l = sizeof a;
	getsockname(s, (struct sockaddr *)&a, &l);
	char pb[16];
	snprintf(pb, sizeof pb, "%d", ntohs(a.sin_port));
	U.listen_port = xstrdup(pb);
	setenv("FZF_PORT", pb, 1);
	fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
	fcntl(s, F_SETFD, FD_CLOEXEC);
	U.listen_fd = s;
	free(spec);
}

static void json_str(Buf *b, const char *s, size_t n)
{
	buf_addc(b, '"');
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];
		if (c == '"' || c == '\\') { buf_addc(b, '\\'); buf_addc(b, (char)c); }
		else if (c == '\n') buf_adds(b, "\\n");
		else if (c == '\t') buf_adds(b, "\\t");
		else if (c < 32) buf_printf(b, "\\u%04x", c);
		else buf_addc(b, (char)c);
	}
	buf_addc(b, '"');
}

static void reply(int c, int code, const char *body, size_t n)
{
	char h[160];
	int hl = snprintf(h, sizeof h, "HTTP/1.1 %d %s\r\nContent-Length: %zu\r\nConnection: close\r\nContent-Type: application/json\r\n\r\n", code, code == 200 ? "OK" : "Bad Request", n);
	if (write(c, h, (size_t)hl) < 0) return;
	if (n && write(c, body, n) < 0) return;
}

static void serve(int c)
{
	Buf req;
	buf_init(&req);
	char tmp[4096];
	size_t need = 0;
	for (int i = 0; i < 50; i++) {
		struct pollfd pf = {c, POLLIN, 0};
		if (poll(&pf, 1, 200) <= 0) break;
		ssize_t n = read(c, tmp, sizeof tmp);
		if (n <= 0) break;
		buf_addn(&req, tmp, (size_t)n);
		char *he = req.p ? memmem(req.p, req.len, "\r\n\r\n", 4) : NULL;
		if (he) {
			size_t hl = (size_t)(he - req.p) + 4;
			const char *cl = strcasestr(req.p, "content-length:");
			size_t clen = cl && cl < he ? (size_t)atol(cl + 15) : 0;
			need = hl + clen;
			if (req.len >= need) break;
		}
	}
	if (!req.len) { buf_free(&req); return; }
	buf_addc(&req, 0);
	bool post = !strncmp(req.p, "POST", 4);
	char *body = strstr(req.p, "\r\n\r\n");
	body = body ? body + 4 : (char *)"";
	if (post) {
		while (*body == ' ' || *body == '\n' || *body == '\r') body++;
		size_t bl = strlen(body);
		while (bl && (body[bl - 1] == '\n' || body[bl - 1] == '\r' || body[bl - 1] == ' ')) body[--bl] = 0;
		ActLoc l = {NULL, 0};
		char *err = NULL;
		/* acts_from_string dies on a bad action: parse in a child-safe way by validating names first */
		if (!bl) err = "empty";
		if (!err) {
			acts_from_string(&l, body);
			free(U.last_key); U.last_key = xstrdup("");
			act_run_list(l.v, l.n);
			acts_free(&l);
			ui_dirty();
		}
		if (err) reply(c, 400, err, strlen(err)); else reply(c, 200, "", 0);
	} else {
		Buf j;
		buf_init(&j);
		char *q = ui_query_str();
		buf_printf(&j, "{\"reading\":%s,\"progress\":0,\"query\":", U.feed_done ? "false" : "true");
		json_str(&j, q, strlen(q));
		buf_printf(&j, ",\"position\":%zu,\"sort\":%s,\"totalCount\":%zu,\"matchCount\":%zu,\"current\":", U.nhits ? U.cur : 0, U.o->sort ? "true" : "false", U.items.n, U.nhits);
		Item *it = ui_current();
		if (it) { buf_printf(&j, "{\"index\":%u,\"text\":", it->index); json_str(&j, it->orig, it->olen); buf_addc(&j, '}'); } else buf_adds(&j, "null");
		buf_addc(&j, '}');
		reply(c, 200, j.p, j.len);
		free(q); buf_free(&j);
	}
	buf_free(&req);
}

void listen_poll(void)
{
	if (U.listen_fd <= 0) return;
	for (int i = 0; i < 8; i++) {
		int c = accept(U.listen_fd, NULL, NULL);
		if (c < 0) return;
		serve(c);
		close(c);
	}
}

/* ======== hints.c ======== */
/* hints.c - the key hint bar (--hints): built-in keys plus every "--bind key:action # label", drawn as key caps.
 * With the kitty keyboard protocol only the hints of the modifiers currently held are shown (like nnn). */

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
