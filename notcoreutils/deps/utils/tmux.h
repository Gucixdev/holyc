/* tmux.h - shared types/constants for tmux.c, split out here (not
 * inline in the .c file) per the same "own .h files live in utils/"
 * convention this project's other shared headers (sbase-util.h/
 * ubase-util.h/base.h/...) already follow - see tmux.c's own top
 * comment for the full design. tmux.c stays the ONLY .c file (this
 * project's build systems all still compile one source file per
 * tool), just #including this instead of defining these inline.
 */
#ifndef CAVEUTILS_TMUX_H
#define CAVEUTILS_TMUX_H

#include <sys/types.h>

/* Limits - fixed-size tables rather than fully dynamic allocation for
 * everything except the per-window layout tree (which is genuinely
 * recursive) and each pane's own screen grid (which is genuinely
 * sized by real terminal dimensions) - same "small fixed tables, no
 * malloc sprawl" style as the rest of this project's own code. */
#define MAX_PANES 64
#define MAX_WINDOWS 32
#define MAX_SESSIONS 16
#define MAX_CLIENTS 16
#define NAME_LEN 64

typedef struct {
	char ch;
	unsigned char fg, bg;   /* 0 = default */
	unsigned char attr;     /* bit 0 bold, bit 1 reverse, bit 2 underline */
} Cell;

#define ATTR_BOLD 1
#define ATTR_REVERSE 2
#define ATTR_UNDERLINE 4

/* Minimal VT100/ECMA-48 parser state, persistent per pane so a read()
 * that splits an escape sequence mid-way (a real, common occurrence)
 * doesn't corrupt the grid or desync - the state machine just keeps
 * collecting bytes across calls until a sequence is complete. */
typedef enum { VT_GROUND, VT_ESC, VT_CSI } VtState;

typedef struct {
	int in_use;
	int master_fd;
	pid_t pid;
	int cols, rows;
	Cell *grid;              /* cols*rows cells, row-major */
	int cx, cy;               /* cursor, 0-based */
	unsigned char cur_fg, cur_bg, cur_attr;
	VtState vt_state;
	char vt_params[32];
	int vt_nparams;
	char vt_cur[16];
	int vt_cur_len;
} Pane;

typedef struct LayoutNode {
	int is_leaf;
	int pane_idx;             /* valid if is_leaf */
	char dir;                  /* 'h' (side by side) or 'v' (stacked) */
	int x, y, w, h;            /* computed on-screen region */
	struct LayoutNode *a, *b;
} LayoutNode;

typedef struct {
	int in_use;
	char name[NAME_LEN];
	LayoutNode *root;
	int active_pane;           /* index into g_panes */
} Window;

typedef struct {
	int in_use;
	char name[NAME_LEN];
	int windows[MAX_WINDOWS];  /* indices into g_windows, in order */
	int nwindows;
	int active_window;         /* index into this session's windows[] */
} Session;

/* Every real tmux SUBCOMMAND this binary understands, both from argv
 * (the `tmux SUBCOMMAND` CLI) and from a client's ':' command prompt -
 * one shared enum + dispatcher so the two entry points can't drift
 * out of sync with each other. */
typedef enum {
	CMD_NONE = 0,
	CMD_NEW_SESSION,
	CMD_ATTACH,
	CMD_LIST_SESSIONS,
	CMD_KILL_SERVER,
	CMD_NEW_WINDOW,
	CMD_NEXT_WINDOW,
	CMD_PREV_WINDOW,
	CMD_SELECT_WINDOW,
	CMD_RENAME_WINDOW,
	CMD_SPLIT_H,
	CMD_SPLIT_V,
	CMD_KILL_PANE,
	CMD_DETACH
} CmdKind;

typedef struct {
	int in_use;
	int fd;
	int session;                /* index into g_sessions this client is attached to */
	int cols, rows;
	int have_prefix;            /* true right after the prefix key, waiting for the next one */
	int prefix_esc;             /* 0 none, 1 saw ESC after prefix, 2 saw ESC[ - collecting an arrow-key escape sequence for pane navigation */
	int pending_kill_confirm;   /* true after 'x', waiting for y/n */
	int in_cmdline;             /* true while typing after ':' */
	char cmdline[256];
	int cmdline_len;
} Client;

/* Wire protocol - one Unix domain socket per server, shared by every
 * attached client. Each message is [1 byte type][4 byte big-endian
 * length][length bytes payload] so binary-safe pane output (any byte
 * value, including embedded control bytes) can share the same stream
 * as short text control messages without ambiguity. */
typedef enum {
	MSG_CLIENT_ATTACH = 1,  /* payload: session name (may be empty = "any/most recent") */
	MSG_CLIENT_KEYS,        /* payload: raw keystroke bytes typed by the user */
	MSG_CLIENT_RESIZE,      /* payload: 4 bytes cols, 4 bytes rows, both big-endian */
	MSG_CLIENT_LIST_SESSIONS, /* payload: empty - a one-shot query, not an attach */
	MSG_CLIENT_KILL_SERVER,   /* payload: empty - a one-shot command, not an attach */
	MSG_SERVER_RENDER,      /* payload: a full-frame ANSI redraw, write verbatim to stdout */
	MSG_SERVER_TEXT,        /* payload: plain text (e.g. a session list) - print and exit, not a redraw */
	MSG_SERVER_DETACH       /* payload: empty - client should restore its terminal and exit */
} MsgType;

#endif
