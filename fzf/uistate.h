/* uistate.h - the state of the interactive interface, shared by ui.c (loop, layout, drawing), act.c (actions)
 * and proc.c (child processes: preview, reload, execute, placeholders, --listen). */
#ifndef FZF_UISTATE_H
#define FZF_UISTATE_H
#include "ui.h"
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
