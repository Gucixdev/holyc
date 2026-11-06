/* ui.h - theme, key bindings and actions shared between keys.c, theme.c and ui.c. */
#ifndef FZF_UI_H
#define FZF_UI_H
#include "fzf.h"
#include "opts.h"
#include "term.h"

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
