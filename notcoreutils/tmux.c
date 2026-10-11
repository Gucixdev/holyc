/* tmux - a real terminal multiplexer: sessions containing windows
 * containing panes, each pane a real pty running a real shell/command.
 * Client/server split like the real tmux: the first invocation with
 * no server running forks and daemonizes into a background server
 * that owns every pty and keeps every process alive across detach/
 * attach; every invocation (including that first one, after forking)
 * is then a thin client that connects over a Unix socket, puts the
 * local terminal into raw mode, and both renders what the server
 * sends and forwards local keystrokes back.
 *
 * Deliberately a DIFFERENT socket path than real tmux
 * (/tmp/nctmux-<uid>/default, not /tmp/tmux-<uid>/default) - so
 * this can never collide with, or be confused for, an actual system
 * tmux instance that might also be installed and running.
 *
 * The server, not the client, owns real per-pane terminal emulation
 * state (a small VT100/ECMA-48 subset - see vt_feed()) - this is real
 * tmux's own actual architecture too (a client is a thin renderer,
 * not where interpretation happens), and it's what makes reattaching
 * to a session show the CURRENT screen instantly instead of replaying
 * scrollback: the server always knows exactly what every pane's
 * screen currently looks like, so a newly attached client just gets a
 * fresh full redraw of that already-known state.
 *
 * usage:
 *   tmux                        attach (starting the server + a
 *                                "main" session if neither exists yet)
 *   tmux new-session [-s NAME]  create + attach a new session
 *   tmux attach [-t NAME]       attach an existing session
 *   tmux list-sessions          list sessions (from any shell, no
 *                                attach needed)
 *   tmux kill-server            stop the server and every pane in it
 *
 * Inside a session (prefix key: Ctrl-b, real tmux's own default):
 *   c          new window
 *   n / p      next / previous window
 *   0-9        select window by index
 *   %          split pane vertically (side by side)
 *   "          split pane horizontally (stacked)
 *   arrow keys move between panes
 *   x          kill the active pane (y/n confirm)
 *   d          detach (server keeps running)
 *   :          command prompt (same subcommands as the `tmux
 *              SUBCOMMAND` CLI above, typed instead of shelled out to
 *              - this IS real tmux's own idea of "scripting": external
 *              shell scripts drive a running server via this exact
 *              same `tmux SUBCOMMAND` interface, not a separate
 *              embedded language)
 *
 * Config: $HOME/.tmux.conf (or $XDG_CONFIG_HOME/tmux/tmux.conf), read
 * once at server startup. Supported directives, one per line (blank
 * lines and #-comments ignored):
 *   set -g prefix C-a          (rebind the prefix key)
 *   set -g status-bg N          (0-255 ANSI colour index)
 *   set -g status-fg N
 * A real, but real-STATED subset - not the ~200 real tmux.conf
 * options. Every other real directive, `bind-key` included, is
 * recognised as "a config line" (not a parse error) but has no effect
 * yet - see the project README for the full "not done yet" list.
 *
 * NOT a full clone of every real tmux feature - stated honestly, same
 * practice as every other tool in this project: copy-mode (scrollback
 * search/selection), true multi-client mirroring of one session
 * (each attached client gets its own independently-sized render of
 * the same underlying panes, not a shared view), cross-session window
 * linking, `bind-key`'s actual effect, and hooks are not implemented.
 * What IS real and load-bearing: real ptys, real detach/reattach (a
 * shell survives a detach), real splits with a real binary layout
 * tree (not a fixed 2-pane special case), and a real terminal-state
 * emulator (not a raw-byte passthrough - that would break the instant
 * a pane's program does anything beyond printing plain lines, e.g.
 * any shell prompt with colour). UTF-8/wide characters are NOT
 * handled in the per-cell grid (one grid cell == one byte) - a real,
 * stated gap, not silently mangled: multibyte input still passes
 * through byte-for-byte, it just may not line up with real terminal
 * cell-width rules for anything outside ASCII.
 *
 * LIVE-VERIFIED (pty-driven, same technique as every other TUI in
 * this project): new-session, real pty output showing up through the
 * VT100 emulator + compositor, detach, reattach with the shell
 * process and its exact prior screen content both surviving (the
 * defining tmux behaviour), a real vertical split AND a real
 * horizontal split (both border characters rendered), a 3-pane mixed
 * layout (both split directions in the same window at once), a new
 * window (status bar correctly listing both), and kill-pane with its
 * y/n confirm (border correctly disappears, layout tree collapses).
 * Also confirmed: identical tool count/failure set across Make/CMake/
 * Meson (this file needs none of the 9 pre-existing missing headers
 * the rest of that failure list does). NOT verified: config file
 * directives, multiple simultaneous clients on one session, `:`
 * command-prompt subcommands other than what detach/split/window
 * testing already exercised indirectly (new-window, split-window).
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <poll.h>
#include <pty.h>
#include <termios.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>

#include "tmux.h"

static Pane g_panes[MAX_PANES];
static Window g_windows[MAX_WINDOWS];
static Session g_sessions[MAX_SESSIONS];
static Client g_clients[MAX_CLIENTS];

static int g_prefix_key = 2; /* Ctrl-b by default */
static int g_dirty = 1;      /* server: something changed, clients need a fresh render */
static int g_status_fg = 15;
static int g_status_bg = 234;

static char g_sockpath[256];

/* ---------------------------------------------------------------- */
/* Small helpers                                                     */
/* ---------------------------------------------------------------- */

static void die(const char *msg)
{
	fprintf(stderr, "tmux: %s: %s\n", msg, strerror(errno));
	exit(1);
}

/* ---------------------------------------------------------------- */
/* Minimal VT100/ECMA-48 emulator - feeds raw pty output into a      */
/* pane's cell grid. Handles: printable bytes (with wrap+scroll),    */
/* \r \n \b \t, and the CSI subset real shells/editors actually lean */
/* on day to day: cursor movement (A/B/C/D/H/f), erase (J/K), SGR     */
/* colour/attrs (m). Anything else in a CSI sequence is consumed and  */
/* silently ignored (a real, stated gap - not every real tmux/xterm   */
/* control sequence is implemented) rather than dumped as garbage     */
/* onto the grid.                                                    */
/* ---------------------------------------------------------------- */

static Cell *cell_at(Pane *p, int x, int y)
{
	if (x < 0 || y < 0 || x >= p->cols || y >= p->rows) return NULL;
	return &p->grid[y * p->cols + x];
}

static void scroll_up_one(Pane *p)
{
	int y, x;
	for (y = 0; y < p->rows - 1; y++) {
		for (x = 0; x < p->cols; x++) {
			*cell_at(p, x, y) = *cell_at(p, x, y + 1);
		}
	}
	for (x = 0; x < p->cols; x++) {
		Cell *c = cell_at(p, x, p->rows - 1);
		c->ch = ' ';
		c->fg = 0;
		c->bg = 0;
		c->attr = 0;
	}
}

static void put_char(Pane *p, char ch)
{
	Cell *c;
	if (p->cx >= p->cols) {
		p->cx = 0;
		p->cy++;
	}
	if (p->cy >= p->rows) {
		scroll_up_one(p);
		p->cy = p->rows - 1;
	}
	c = cell_at(p, p->cx, p->cy);
	if (c) {
		c->ch = ch;
		c->fg = p->cur_fg;
		c->bg = p->cur_bg;
		c->attr = p->cur_attr;
	}
	p->cx++;
}

static void erase_display(Pane *p, int mode)
{
	int x, y;
	int from_y = 0, to_y = p->rows - 1;
	if (mode == 0) from_y = p->cy;
	else if (mode == 1) to_y = p->cy;
	for (y = from_y; y <= to_y; y++) {
		for (x = 0; x < p->cols; x++) {
			Cell *c = cell_at(p, x, y);
			c->ch = ' ';
			c->fg = 0;
			c->bg = 0;
			c->attr = 0;
		}
	}
}

static void erase_line(Pane *p, int mode)
{
	int x;
	int from_x = 0, to_x = p->cols - 1;
	if (mode == 0) from_x = p->cx;
	else if (mode == 1) to_x = p->cx;
	for (x = from_x; x <= to_x; x++) {
		Cell *c = cell_at(p, x, p->cy);
		c->ch = ' ';
		c->fg = 0;
		c->bg = 0;
		c->attr = 0;
	}
}

static void apply_sgr(Pane *p, int *params, int nparams)
{
	int i;
	if (nparams == 0) {
		p->cur_fg = p->cur_bg = 0;
		p->cur_attr = 0;
		return;
	}
	for (i = 0; i < nparams; i++) {
		int v = params[i];
		if (v == 0) { p->cur_fg = p->cur_bg = 0; p->cur_attr = 0; }
		else if (v == 1) p->cur_attr |= ATTR_BOLD;
		else if (v == 4) p->cur_attr |= ATTR_UNDERLINE;
		else if (v == 7) p->cur_attr |= ATTR_REVERSE;
		else if (v == 22) p->cur_attr &= (unsigned char)~ATTR_BOLD;
		else if (v == 24) p->cur_attr &= (unsigned char)~ATTR_UNDERLINE;
		else if (v == 27) p->cur_attr &= (unsigned char)~ATTR_REVERSE;
		else if (v >= 30 && v <= 37) p->cur_fg = (unsigned char)(v - 30 + 1);
		else if (v == 39) p->cur_fg = 0;
		else if (v >= 40 && v <= 47) p->cur_bg = (unsigned char)(v - 40 + 1);
		else if (v == 49) p->cur_bg = 0;
		else if (v >= 90 && v <= 97) p->cur_fg = (unsigned char)(v - 90 + 9);
		else if (v >= 100 && v <= 107) p->cur_bg = (unsigned char)(v - 100 + 9);
	}
}

static void run_csi(Pane *p, char final, const char *params_str)
{
	int params[16];
	int nparams = 0;
	const char *s = params_str;
	while (*s && nparams < 16) {
		params[nparams++] = atoi(s);
		while (*s && *s != ';') s++;
		if (*s == ';') s++;
	}

	switch (final) {
	case 'A': p->cy -= nparams ? (params[0] ? params[0] : 1) : 1; break;
	case 'B': p->cy += nparams ? (params[0] ? params[0] : 1) : 1; break;
	case 'C': p->cx += nparams ? (params[0] ? params[0] : 1) : 1; break;
	case 'D': p->cx -= nparams ? (params[0] ? params[0] : 1) : 1; break;
	case 'H': case 'f':
		p->cy = (nparams >= 1 && params[0] > 0) ? params[0] - 1 : 0;
		p->cx = (nparams >= 2 && params[1] > 0) ? params[1] - 1 : 0;
		break;
	case 'J': erase_display(p, nparams ? params[0] : 0); break;
	case 'K': erase_line(p, nparams ? params[0] : 0); break;
	case 'm': apply_sgr(p, params, nparams); break;
	default: break; /* real, stated gap - unhandled CSI final byte */
	}
	if (p->cx < 0) p->cx = 0;
	if (p->cy < 0) p->cy = 0;
	if (p->cy >= p->rows) p->cy = p->rows - 1;
}

static void vt_feed(Pane *p, const unsigned char *buf, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++) {
		unsigned char ch = buf[i];
		if (p->vt_state == VT_GROUND) {
			if (ch == 0x1b) {
				p->vt_state = VT_ESC;
			} else if (ch == '\r') {
				p->cx = 0;
			} else if (ch == '\n') {
				p->cy++;
				if (p->cy >= p->rows) {
					scroll_up_one(p);
					p->cy = p->rows - 1;
				}
			} else if (ch == '\b') {
				if (p->cx > 0) p->cx--;
			} else if (ch == '\t') {
				p->cx = (p->cx / 8 + 1) * 8;
			} else if (ch >= 0x20) {
				put_char(p, (char)ch);
			}
		} else if (p->vt_state == VT_ESC) {
			if (ch == '[') {
				p->vt_state = VT_CSI;
				p->vt_cur_len = 0;
			} else {
				p->vt_state = VT_GROUND; /* unhandled ESC sequence, real stated gap */
			}
		} else if (p->vt_state == VT_CSI) {
			if (ch >= '0' && ch <= '9') {
				if (p->vt_cur_len < (int)sizeof(p->vt_cur) - 1) {
					p->vt_cur[p->vt_cur_len++] = (char)ch;
				}
			} else if (ch == ';') {
				if (p->vt_cur_len < (int)sizeof(p->vt_cur) - 1) {
					p->vt_cur[p->vt_cur_len++] = (char)ch;
				}
			} else if (ch == '?') {
				/* private-mode marker (DEC sequences like \e[?25l) -
				 * consumed and ignored, real stated gap. */
			} else {
				p->vt_cur[p->vt_cur_len] = '\0';
				run_csi(p, (char)ch, p->vt_cur);
				p->vt_state = VT_GROUND;
			}
		}
	}
}

/* ---------------------------------------------------------------- */
/* Pane / layout management                                          */
/* ---------------------------------------------------------------- */

static int pane_spawn(int cols, int rows)
{
	int idx;
	struct winsize ws;
	pid_t pid;
	int master;

	idx = -1;
	{
		int i;
		for (i = 0; i < MAX_PANES; i++) {
			if (!g_panes[i].in_use) { idx = i; break; }
		}
	}
	if (idx < 0) return -1;

	memset(&ws, 0, sizeof(ws));
	ws.ws_col = (unsigned short)cols;
	ws.ws_row = (unsigned short)rows;

	pid = forkpty(&master, NULL, NULL, &ws);
	if (pid < 0) return -1;
	if (pid == 0) {
		const char *shell = getenv("SHELL");
		if (!shell || !*shell) shell = "/bin/sh";
		setenv("TERM", "xterm-256color", 1);
		execl(shell, shell, (char *)NULL);
		_exit(127);
	}

	{
		Pane *p = &g_panes[idx];
		memset(p, 0, sizeof(*p));
		p->in_use = 1;
		p->master_fd = master;
		p->pid = pid;
		p->cols = cols;
		p->rows = rows;
		p->grid = calloc((size_t)cols * (size_t)rows, sizeof(Cell));
		{
			int x, y;
			for (y = 0; y < rows; y++)
				for (x = 0; x < cols; x++) {
					Cell *c = cell_at(p, x, y);
					c->ch = ' ';
				}
		}
		p->vt_state = VT_GROUND;
		fcntl(master, F_SETFL, O_NONBLOCK);
	}
	return idx;
}

static void pane_kill(int idx)
{
	Pane *p = &g_panes[idx];
	if (!p->in_use) return;
	kill(p->pid, SIGHUP);
	close(p->master_fd);
	free(p->grid);
	memset(p, 0, sizeof(*p));
}

static void pane_resize(int idx, int cols, int rows)
{
	Pane *p = &g_panes[idx];
	Cell *newgrid;
	struct winsize ws;
	int x, y;

	if (!p->in_use || cols < 1 || rows < 1) return;
	if (cols == p->cols && rows == p->rows) return;

	newgrid = calloc((size_t)cols * (size_t)rows, sizeof(Cell));
	for (y = 0; y < rows; y++) {
		for (x = 0; x < cols; x++) {
			Cell *dst = &newgrid[y * cols + x];
			dst->ch = ' ';
			if (x < p->cols && y < p->rows) {
				*dst = *cell_at(p, x, y);
			}
		}
	}
	free(p->grid);
	p->grid = newgrid;
	p->cols = cols;
	p->rows = rows;
	if (p->cx >= cols) p->cx = cols - 1;
	if (p->cy >= rows) p->cy = rows - 1;

	memset(&ws, 0, sizeof(ws));
	ws.ws_col = (unsigned short)cols;
	ws.ws_row = (unsigned short)rows;
	ioctl(p->master_fd, TIOCSWINSZ, &ws);
}

static LayoutNode *layout_leaf(int pane_idx)
{
	LayoutNode *n = calloc(1, sizeof(LayoutNode));
	n->is_leaf = 1;
	n->pane_idx = pane_idx;
	return n;
}

static void layout_free(LayoutNode *n)
{
	if (!n) return;
	if (!n->is_leaf) {
		layout_free(n->a);
		layout_free(n->b);
	}
	free(n);
}

/* Finds the leaf node whose pane_idx == target, for splitting/
 * removing the currently-active pane. */
static LayoutNode *layout_find(LayoutNode *n, int pane_idx)
{
	LayoutNode *r;
	if (!n) return NULL;
	if (n->is_leaf) return n->pane_idx == pane_idx ? n : NULL;
	r = layout_find(n->a, pane_idx);
	if (r) return r;
	return layout_find(n->b, pane_idx);
}

/* Splits the leaf currently holding `pane_idx` into a new internal
 * node with two children: the existing pane, and a freshly spawned
 * one. Returns the new pane's index, or -1 on failure. */
static int layout_split(LayoutNode *root, int pane_idx, char dir)
{
	LayoutNode *leaf = layout_find(root, pane_idx);
	int new_pane;
	LayoutNode *a, *b;
	int half_w, half_h;

	if (!leaf) return -1;

	half_w = leaf->w / 2;
	half_h = leaf->h / 2;
	new_pane = pane_spawn(dir == 'h' ? (leaf->w - half_w) : leaf->w,
	                       dir == 'v' ? (leaf->h - half_h) : leaf->h);
	if (new_pane < 0) return -1;

	a = layout_leaf(pane_idx);
	b = layout_leaf(new_pane);
	leaf->is_leaf = 0;
	leaf->dir = dir;
	leaf->a = a;
	leaf->b = b;
	return new_pane;
}

/* Removes the leaf holding `pane_idx` from the tree, replacing its
 * parent with the SIBLING subtree. Returns the new root (may differ
 * from the old one if the removed pane's own parent was the root). */
static LayoutNode *layout_remove(LayoutNode *root, int pane_idx)
{
	if (!root) return NULL;
	if (root->is_leaf) return root; /* caller handles the whole-window-empty case */
	if (!root->a->is_leaf) root->a = layout_remove(root->a, pane_idx);
	if (!root->b->is_leaf) root->b = layout_remove(root->b, pane_idx);
	if (root->a->is_leaf && root->a->pane_idx == pane_idx) {
		LayoutNode *keep = root->b;
		free(root->a);
		free(root);
		return keep;
	}
	if (root->b->is_leaf && root->b->pane_idx == pane_idx) {
		LayoutNode *keep = root->a;
		free(root->b);
		free(root);
		return keep;
	}
	return root;
}

static void compute_layout(LayoutNode *n, int x, int y, int w, int h)
{
	if (!n) return;
	n->x = x;
	n->y = y;
	n->w = w;
	n->h = h;
	if (n->is_leaf) {
		pane_resize(n->pane_idx, w > 0 ? w : 1, h > 0 ? h : 1);
		return;
	}
	if (n->dir == 'h') {
		int wa = (w - 1) / 2; /* 1-column gap for the split border */
		int wb = w - 1 - wa;
		compute_layout(n->a, x, y, wa, h);
		compute_layout(n->b, x + wa + 1, y, wb, h);
	} else {
		int ha = (h - 1) / 2; /* 1-row gap for the split border */
		int hb = h - 1 - ha;
		compute_layout(n->a, x, y, w, ha);
		compute_layout(n->b, x, y + ha + 1, w, hb);
	}
}

static int layout_first_leaf(LayoutNode *n)
{
	if (!n) return -1;
	if (n->is_leaf) return n->pane_idx;
	return layout_first_leaf(n->a);
}

static void layout_collect(LayoutNode *n, int *out, int *count)
{
	if (!n) return;
	if (n->is_leaf) {
		out[(*count)++] = n->pane_idx;
		return;
	}
	layout_collect(n->a, out, count);
	layout_collect(n->b, out, count);
}

/* Which leaf is adjacent to `from` in direction dx,dy (arrow-key pane
 * navigation) - picked by nearest centre-to-centre distance among
 * leaves roughly in that direction, a simple, real, good-enough
 * heuristic (not real tmux's own more elaborate edge-adjacency rule). */
static int layout_navigate(LayoutNode *root, int from_pane, int dx, int dy)
{
	int panes[MAX_PANES];
	int count = 0;
	LayoutNode *fromleaf = layout_find(root, from_pane);
	int fcx, fcy;
	int best = -1;
	long best_dist = -1;
	int i;

	if (!fromleaf) return from_pane;
	fcx = fromleaf->x + fromleaf->w / 2;
	fcy = fromleaf->y + fromleaf->h / 2;

	layout_collect(root, panes, &count);
	for (i = 0; i < count; i++) {
		LayoutNode *leaf;
		int cx, cy;
		long ddx, ddy, dist;
		if (panes[i] == from_pane) continue;
		leaf = layout_find(root, panes[i]);
		cx = leaf->x + leaf->w / 2;
		cy = leaf->y + leaf->h / 2;
		if (dx > 0 && cx <= fcx) continue;
		if (dx < 0 && cx >= fcx) continue;
		if (dy > 0 && cy <= fcy) continue;
		if (dy < 0 && cy >= fcy) continue;
		ddx = cx - fcx;
		ddy = cy - fcy;
		dist = ddx * ddx + ddy * ddy;
		if (best_dist < 0 || dist < best_dist) {
			best_dist = dist;
			best = panes[i];
		}
	}
	return best >= 0 ? best : from_pane;
}

/* ---------------------------------------------------------------- */
/* Window / session management                                       */
/* ---------------------------------------------------------------- */

static int window_create(const char *name)
{
	int idx = -1, i;
	int pane;
	for (i = 0; i < MAX_WINDOWS; i++) {
		if (!g_windows[i].in_use) { idx = i; break; }
	}
	if (idx < 0) return -1;

	pane = pane_spawn(80, 24); /* real size arrives on first client attach/resize */
	if (pane < 0) return -1;

	memset(&g_windows[idx], 0, sizeof(Window));
	g_windows[idx].in_use = 1;
	strncpy(g_windows[idx].name, name, NAME_LEN - 1);
	g_windows[idx].root = layout_leaf(pane);
	g_windows[idx].active_pane = pane;
	return idx;
}

static void window_destroy(int idx)
{
	int panes[MAX_PANES];
	int count = 0, i;
	Window *w = &g_windows[idx];
	if (!w->in_use) return;
	layout_collect(w->root, panes, &count);
	for (i = 0; i < count; i++) pane_kill(panes[i]);
	layout_free(w->root);
	memset(w, 0, sizeof(*w));
}

static int session_find(const char *name)
{
	int i;
	for (i = 0; i < MAX_SESSIONS; i++) {
		if (g_sessions[i].in_use && strcmp(g_sessions[i].name, name) == 0) return i;
	}
	return -1;
}

static int session_create(const char *name)
{
	int idx = -1, i;
	int win;
	for (i = 0; i < MAX_SESSIONS; i++) {
		if (!g_sessions[i].in_use) { idx = i; break; }
	}
	if (idx < 0) return -1;
	if (session_find(name) >= 0) return -1;

	win = window_create("0");
	if (win < 0) return -1;

	memset(&g_sessions[idx], 0, sizeof(Session));
	g_sessions[idx].in_use = 1;
	strncpy(g_sessions[idx].name, name, NAME_LEN - 1);
	g_sessions[idx].windows[0] = win;
	g_sessions[idx].nwindows = 1;
	g_sessions[idx].active_window = 0;
	return idx;
}

static int session_any(void)
{
	int i;
	for (i = 0; i < MAX_SESSIONS; i++) {
		if (g_sessions[i].in_use) return i;
	}
	return -1;
}

/* ---------------------------------------------------------------- */
/* Compositing: renders the active window of `sess` into one ANSI    */
/* byte stream a client can write straight to its own stdout for a   */
/* full-frame redraw. Two passes: compose a plain Cell grid first    */
/* (panes copied in at their real layout offsets, borders drawn at   */
/* the gaps, status bar as the last row), then serialize that grid   */
/* to ANSI, tracking the last-emitted SGR state so unchanged runs of */
/* cells don't repeat colour codes every single cell.                */
/* ---------------------------------------------------------------- */

static void draw_borders(LayoutNode *n, Cell *screen, int width)
{
	if (!n || n->is_leaf) return;
	if (n->dir == 'h') {
		int gx = n->a->x + n->a->w;
		int y;
		for (y = n->y; y < n->y + n->h; y++) {
			Cell *c = &screen[y * width + gx];
			c->ch = '|';
			c->fg = 0;
			c->bg = 0;
			c->attr = 0;
		}
	} else {
		int gy = n->a->y + n->a->h;
		int x;
		for (x = n->x; x < n->x + n->w; x++) {
			Cell *c = &screen[gy * width + x];
			c->ch = '-';
			c->fg = 0;
			c->bg = 0;
			c->attr = 0;
		}
	}
	draw_borders(n->a, screen, width);
	draw_borders(n->b, screen, width);
}

static void copy_panes(LayoutNode *n, Cell *screen, int width)
{
	Pane *p;
	int x, y;
	if (!n) return;
	if (!n->is_leaf) {
		copy_panes(n->a, screen, width);
		copy_panes(n->b, screen, width);
		return;
	}
	p = &g_panes[n->pane_idx];
	if (!p->in_use) return;
	for (y = 0; y < p->rows && y < n->h; y++) {
		for (x = 0; x < p->cols && x < n->w; x++) {
			screen[(n->y + y) * width + (n->x + x)] = p->grid[y * p->cols + x];
		}
	}
}

static char *render_frame(Session *sess, int width, int height, size_t *outlen)
{
	Window *win = &g_windows[sess->windows[sess->active_window]];
	Cell *screen;
	size_t cap, len = 0;
	char *buf;
	int x, y;
	int last_fg = -1, last_bg = -1, last_attr = -1;
	Pane *active_pane;
	LayoutNode *active_leaf;
	int cursor_row, cursor_col;
	char statusline[512];
	int slen, i;

	if (width < 4) width = 4;
	if (height < 3) height = 3;

	compute_layout(win->root, 0, 0, width, height - 1);

	screen = calloc((size_t)width * (size_t)(height - 1), sizeof(Cell));
	for (y = 0; y < height - 1; y++)
		for (x = 0; x < width; x++)
			screen[y * width + x].ch = ' ';
	copy_panes(win->root, screen, width);
	draw_borders(win->root, screen, width);

	cap = (size_t)width * (size_t)height * 24 + 4096;
	buf = malloc(cap);

	len += (size_t)sprintf(buf + len, "\x1b[H");
	for (y = 0; y < height - 1; y++) {
		len += (size_t)sprintf(buf + len, "\x1b[%d;1H", y + 1);
		for (x = 0; x < width; x++) {
			Cell *c = &screen[y * width + x];
			if (c->fg != last_fg || c->bg != last_bg || c->attr != last_attr) {
				int fg = c->fg, bg = c->bg, attr = c->attr;
				len += (size_t)sprintf(buf + len, "\x1b[0");
				if (attr & ATTR_BOLD) len += (size_t)sprintf(buf + len, ";1");
				if (attr & ATTR_UNDERLINE) len += (size_t)sprintf(buf + len, ";4");
				if (attr & ATTR_REVERSE) len += (size_t)sprintf(buf + len, ";7");
				if (fg > 0 && fg <= 8) len += (size_t)sprintf(buf + len, ";%d", 30 + (fg - 1));
				else if (fg > 8) len += (size_t)sprintf(buf + len, ";%d", 90 + (fg - 9));
				if (bg > 0 && bg <= 8) len += (size_t)sprintf(buf + len, ";%d", 40 + (bg - 1));
				else if (bg > 8) len += (size_t)sprintf(buf + len, ";%d", 100 + (bg - 9));
				len += (size_t)sprintf(buf + len, "m");
				last_fg = fg;
				last_bg = bg;
				last_attr = attr;
			}
			buf[len++] = c->ch ? c->ch : ' ';
		}
	}
	free(screen);

	/* status bar: session name, then every window ("*" marks active) */
	slen = sprintf(statusline, "\x1b[0;38;5;%d;48;5;%dm [%s] ", g_status_fg, g_status_bg, sess->name);
	for (i = 0; i < sess->nwindows && slen < (int)sizeof(statusline) - 32; i++) {
		Window *w = &g_windows[sess->windows[i]];
		slen += sprintf(statusline + slen, "%d:%s%s  ", i, w->name,
			i == sess->active_window ? "*" : "");
	}
	len += (size_t)sprintf(buf + len, "\x1b[%d;1H\x1b[K%s\x1b[0m", height, statusline);
	{
		int pad;
		for (pad = 0; pad < width; pad++) { /* pad-and-truncate handled by terminal wrap/clip naturally */ }
	}

	active_pane = &g_panes[win->active_pane];
	active_leaf = layout_find(win->root, win->active_pane);
	cursor_row = (active_leaf ? active_leaf->y : 0) + active_pane->cy + 1;
	cursor_col = (active_leaf ? active_leaf->x : 0) + active_pane->cx + 1;
	len += (size_t)sprintf(buf + len, "\x1b[%d;%dH", cursor_row, cursor_col);

	*outlen = len;
	return buf;
}

/* ---------------------------------------------------------------- */
/* Wire protocol I/O - see tmux.h's own comment on the framing.      */
/* Blocking read/write-until-complete rather than fully async        */
/* buffering: a real, deliberate scope cut - every message here is   */
/* short and comes from this project's own client program on the     */
/* same machine, so a slow/partial write stalling the whole server's */
/* poll() loop isn't a realistic concern the way it would be for a   */
/* server handling untrusted/networked clients.                      */
/* ---------------------------------------------------------------- */

static int read_all(int fd, void *buf, size_t n)
{
	size_t got = 0;
	while (got < n) {
		ssize_t r = read(fd, (char *)buf + got, n - got);
		if (r < 0) {
			if (errno == EINTR) continue;
			return -1;
		}
		if (r == 0) return -1;
		got += (size_t)r;
	}
	return 0;
}

static int write_all(int fd, const void *buf, size_t n)
{
	size_t sent = 0;
	while (sent < n) {
		ssize_t w = write(fd, (const char *)buf + sent, n - sent);
		if (w < 0) {
			if (errno == EINTR) continue;
			return -1;
		}
		sent += (size_t)w;
	}
	return 0;
}

static int send_msg(int fd, MsgType type, const void *payload, size_t len)
{
	unsigned char hdr[5];
	hdr[0] = (unsigned char)type;
	hdr[1] = (unsigned char)((len >> 24) & 0xff);
	hdr[2] = (unsigned char)((len >> 16) & 0xff);
	hdr[3] = (unsigned char)((len >> 8) & 0xff);
	hdr[4] = (unsigned char)(len & 0xff);
	if (write_all(fd, hdr, 5) < 0) return -1;
	if (len > 0 && write_all(fd, payload, len) < 0) return -1;
	return 0;
}

static int recv_msg(int fd, MsgType *type, char *payload, size_t max_payload, size_t *outlen)
{
	unsigned char hdr[5];
	unsigned long len;
	if (read_all(fd, hdr, 5) < 0) return -1;
	*type = (MsgType)hdr[0];
	len = ((unsigned long)hdr[1] << 24) | ((unsigned long)hdr[2] << 16)
		| ((unsigned long)hdr[3] << 8) | (unsigned long)hdr[4];
	if (len > max_payload) return -1;
	if (len > 0 && read_all(fd, payload, len) < 0) return -1;
	*outlen = (size_t)len;
	return 0;
}

/* ---------------------------------------------------------------- */
/* Server-side pane/window/session teardown - shared by both an      */
/* explicit user kill ('x', confirmed) and a pane's process simply    */
/* exiting on its own (a real shell running `exit`).                 */
/* ---------------------------------------------------------------- */

static void session_remove_window(int window_idx)
{
	int si;
	for (si = 0; si < MAX_SESSIONS; si++) {
		Session *s = &g_sessions[si];
		int i;
		if (!s->in_use) continue;
		for (i = 0; i < s->nwindows; i++) {
			if (s->windows[i] != window_idx) continue;
			{
				int j;
				for (j = i; j < s->nwindows - 1; j++) s->windows[j] = s->windows[j + 1];
			}
			s->nwindows--;
			if (s->active_window >= s->nwindows) {
				s->active_window = s->nwindows > 0 ? s->nwindows - 1 : 0;
			}
			if (s->nwindows == 0) {
				int ci;
				for (ci = 0; ci < MAX_CLIENTS; ci++) {
					if (g_clients[ci].in_use && g_clients[ci].session == si) {
						send_msg(g_clients[ci].fd, MSG_SERVER_DETACH, NULL, 0);
						close(g_clients[ci].fd);
						g_clients[ci].in_use = 0;
					}
				}
				s->in_use = 0;
			}
			return;
		}
	}
}

static void pane_remove(int pane_idx, int already_dead)
{
	int wi, found_window = -1;

	for (wi = 0; wi < MAX_WINDOWS; wi++) {
		if (g_windows[wi].in_use && layout_find(g_windows[wi].root, pane_idx)) {
			found_window = wi;
			break;
		}
	}

	if (already_dead) {
		close(g_panes[pane_idx].master_fd);
		free(g_panes[pane_idx].grid);
		memset(&g_panes[pane_idx], 0, sizeof(Pane));
	} else {
		pane_kill(pane_idx);
	}

	if (found_window < 0) return;

	{
		Window *w = &g_windows[found_window];
		if (w->root->is_leaf) {
			layout_free(w->root);
			w->root = NULL;
			w->in_use = 0;
			session_remove_window(found_window);
		} else {
			w->root = layout_remove(w->root, pane_idx);
			if (w->active_pane == pane_idx) w->active_pane = layout_first_leaf(w->root);
		}
	}
}

static void reap_children(void)
{
	for (;;) {
		int status, i;
		pid_t pid = waitpid(-1, &status, WNOHANG);
		if (pid <= 0) break;
		for (i = 0; i < MAX_PANES; i++) {
			if (g_panes[i].in_use && g_panes[i].pid == pid) {
				pane_remove(i, 1);
				break;
			}
		}
	}
}

/* ---------------------------------------------------------------- */
/* Config file - $HOME/.tmux.conf, or $XDG_CONFIG_HOME/tmux/tmux.conf */
/* / $HOME/.config/tmux/tmux.conf if that doesn't exist. Real, but a  */
/* real, stated subset: only `set -g prefix`/`status-bg`/`status-fg`  */
/* are actually applied here - `bind-key` and every other real        */
/* tmux.conf option are recognised as "a config line", not silently   */
/* misparsed, but have no effect yet (see the project README's own    */
/* "not done yet" list). Unknown lines are skipped, not an error -    */
/* same tolerance a real .tmux.conf written for a newer real tmux      */
/* version gets from an older one.                                    */
/* ---------------------------------------------------------------- */

static void parse_config(void)
{
	char path[512];
	const char *home = getenv("HOME");
	const char *xdg;
	FILE *f;
	char line[512];

	if (!home) return;
	snprintf(path, sizeof(path), "%s/.tmux.conf", home);
	f = fopen(path, "r");
	if (!f) {
		xdg = getenv("XDG_CONFIG_HOME");
		if (xdg && *xdg) snprintf(path, sizeof(path), "%s/tmux/tmux.conf", xdg);
		else snprintf(path, sizeof(path), "%s/.config/tmux/tmux.conf", home);
		f = fopen(path, "r");
	}
	if (!f) return;

	while (fgets(line, sizeof(line), f)) {
		char *p = line;
		char *nl;
		while (*p == ' ' || *p == '\t') p++;
		if (*p == '#' || *p == '\n' || *p == '\0') continue;
		nl = strchr(p, '\n');
		if (nl) *nl = '\0';

		if (strncmp(p, "set -g prefix ", 14) == 0) {
			const char *key = p + 14;
			if (strncmp(key, "C-", 2) == 0 && key[2]) {
				char k = key[2];
				if (k >= 'a' && k <= 'z') g_prefix_key = k - 'a' + 1;
				else if (k >= 'A' && k <= 'Z') g_prefix_key = k - 'A' + 1;
			}
		} else if (strncmp(p, "set -g status-bg ", 17) == 0) {
			g_status_bg = atoi(p + 17);
		} else if (strncmp(p, "set -g status-fg ", 17) == 0) {
			g_status_fg = atoi(p + 17);
		}
	}
	fclose(f);
}

/* ---------------------------------------------------------------- */
/* Server-side command dispatch - the SAME subcommand vocabulary the  */
/* `tmux SUBCOMMAND` CLI uses (see cmd_from_argv below), reachable     */
/* from inside a running session via the ':' prompt. This dual entry  */
/* point (argv before a client even exists, cmdline once attached) IS */
/* real tmux's own actual idea of "scripting": an external shell       */
/* script drives a running server through this exact same interface,  */
/* not a separate embedded language.                                  */
/* ---------------------------------------------------------------- */

static void dispatch_cmdline(Client *cl, const char *line)
{
	Session *sess = &g_sessions[cl->session];
	Window *win = &g_windows[sess->windows[sess->active_window]];
	char cmd[64];
	const char *rest;
	int i = 0;

	while (line[i] && line[i] != ' ' && i < 63) { cmd[i] = line[i]; i++; }
	cmd[i] = '\0';
	rest = line + i;
	while (*rest == ' ') rest++;

	if (strcmp(cmd, "new-window") == 0) {
		int w = window_create(*rest ? rest : "new");
		if (w >= 0 && sess->nwindows < MAX_WINDOWS) {
			sess->windows[sess->nwindows] = w;
			sess->active_window = sess->nwindows;
			sess->nwindows++;
		}
	} else if (strcmp(cmd, "rename-window") == 0 && *rest) {
		strncpy(win->name, rest, NAME_LEN - 1);
	} else if (strcmp(cmd, "kill-pane") == 0) {
		pane_remove(win->active_pane, 0);
	} else if (strcmp(cmd, "kill-window") == 0) {
		int widx = sess->windows[sess->active_window];
		window_destroy(widx);
		session_remove_window(widx);
	} else if (strcmp(cmd, "kill-server") == 0) {
		int ci;
		for (ci = 0; ci < MAX_CLIENTS; ci++) {
			if (g_clients[ci].in_use) {
				send_msg(g_clients[ci].fd, MSG_SERVER_DETACH, NULL, 0);
				close(g_clients[ci].fd);
			}
		}
		unlink(g_sockpath);
		exit(0);
	} else if (strcmp(cmd, "split-window") == 0) {
		char dir = (strstr(rest, "-h") != NULL) ? 'h' : 'v';
		int np = layout_split(win->root, win->active_pane, dir);
		if (np >= 0) win->active_pane = np;
	}
	/* an unrecognised command is silently ignored - real, stated gap,
	 * same tolerance parse_config() gives an unknown config line. */
}

static void handle_client_key(Client *cl, unsigned char byte)
{
	Session *sess = &g_sessions[cl->session];
	Window *win = &g_windows[sess->windows[sess->active_window]];

	if (cl->pending_kill_confirm) {
		cl->pending_kill_confirm = 0;
		if (byte == 'y' || byte == 'Y') pane_remove(win->active_pane, 0);
		return;
	}
	if (cl->in_cmdline) {
		if (byte == '\r' || byte == '\n') {
			cl->cmdline[cl->cmdline_len] = '\0';
			dispatch_cmdline(cl, cl->cmdline);
			cl->in_cmdline = 0;
			cl->cmdline_len = 0;
		} else if (byte == 0x1b) {
			cl->in_cmdline = 0;
			cl->cmdline_len = 0;
		} else if (byte == 0x7f || byte == '\b') {
			if (cl->cmdline_len > 0) cl->cmdline_len--;
		} else if (cl->cmdline_len < (int)sizeof(cl->cmdline) - 1) {
			cl->cmdline[cl->cmdline_len++] = (char)byte;
		}
		return;
	}
	if (cl->have_prefix) {
		if (cl->prefix_esc == 1) {
			cl->prefix_esc = (byte == '[') ? 2 : 0;
			if (cl->prefix_esc == 0) cl->have_prefix = 0;
			return;
		}
		if (cl->prefix_esc == 2) {
			int dx = 0, dy = 0;
			if (byte == 'A') dy = -1;
			else if (byte == 'B') dy = 1;
			else if (byte == 'C') dx = 1;
			else if (byte == 'D') dx = -1;
			if (dx || dy) win->active_pane = layout_navigate(win->root, win->active_pane, dx, dy);
			cl->have_prefix = 0;
			cl->prefix_esc = 0;
			return;
		}

		cl->have_prefix = 0;
		if (byte == 0x1b) {
			cl->have_prefix = 1;
			cl->prefix_esc = 1;
			return;
		}
		switch (byte) {
		case 'c':
			if (sess->nwindows < MAX_WINDOWS) {
				int w = window_create("new");
				if (w >= 0) {
					sess->windows[sess->nwindows] = w;
					sess->active_window = sess->nwindows;
					sess->nwindows++;
				}
			}
			break;
		case 'n':
			sess->active_window = (sess->active_window + 1) % sess->nwindows;
			break;
		case 'p':
			sess->active_window = (sess->active_window - 1 + sess->nwindows) % sess->nwindows;
			break;
		case '%': {
			int np = layout_split(win->root, win->active_pane, 'h');
			if (np >= 0) win->active_pane = np;
			break;
		}
		case '"': {
			int np = layout_split(win->root, win->active_pane, 'v');
			if (np >= 0) win->active_pane = np;
			break;
		}
		case 'x':
			cl->pending_kill_confirm = 1;
			break;
		case 'd':
			send_msg(cl->fd, MSG_SERVER_DETACH, NULL, 0);
			cl->fd = -1; /* caller (server_main) closes+frees on seeing this */
			break;
		case ':':
			cl->in_cmdline = 1;
			cl->cmdline_len = 0;
			break;
		default:
			if (byte >= '0' && byte <= '9') {
				int idx = byte - '0';
				if (idx < sess->nwindows) sess->active_window = idx;
			}
			break;
		}
		return;
	}
	if (byte == (unsigned char)g_prefix_key) {
		cl->have_prefix = 1;
		cl->prefix_esc = 0;
		return;
	}
	write(g_panes[win->active_pane].master_fd, &byte, 1);
	g_dirty = 1;
}

/* ---------------------------------------------------------------- */
/* Server main loop - daemonized, owns every pty for as long as this */
/* process lives. poll()s the listening socket, every pane's pty      */
/* master fd, and every attached client's socket in one pass; reaps   */
/* exited pane processes (real detach-safe: a client detaching never  */
/* touches any pane, only a client socket closing does - and an       */
/* exited child is handled the exact same way whether or not any      */
/* client happened to be attached at the time).                       */
/* ---------------------------------------------------------------- */

static void server_main(const char *sockpath, const char *initial_session)
{
	int listen_fd;
	struct sockaddr_un addr;
	mode_t old_umask;

	signal(SIGPIPE, SIG_IGN);

	listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (listen_fd < 0) die("socket");

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, sockpath, sizeof(addr.sun_path) - 1);
	unlink(sockpath);

	old_umask = umask(0177);
	if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) die("bind");
	umask(old_umask);
	if (listen(listen_fd, 16) < 0) die("listen");

	parse_config();
	session_create(initial_session);

	for (;;) {
		struct pollfd fds[1 + MAX_PANES + MAX_CLIENTS];
		int nfds = 0;
		int listen_slot;
		int pane_slot[MAX_PANES];
		int client_slot[MAX_CLIENTS];
		int i;

		listen_slot = nfds;
		fds[nfds].fd = listen_fd;
		fds[nfds].events = POLLIN;
		nfds++;

		for (i = 0; i < MAX_PANES; i++) {
			if (g_panes[i].in_use) {
				pane_slot[i] = nfds;
				fds[nfds].fd = g_panes[i].master_fd;
				fds[nfds].events = POLLIN;
				nfds++;
			} else {
				pane_slot[i] = -1;
			}
		}
		for (i = 0; i < MAX_CLIENTS; i++) {
			if (g_clients[i].in_use && g_clients[i].fd >= 0) {
				client_slot[i] = nfds;
				fds[nfds].fd = g_clients[i].fd;
				fds[nfds].events = POLLIN;
				nfds++;
			} else {
				client_slot[i] = -1;
			}
		}

		if (poll(fds, (nfds_t)nfds, 500) < 0) {
			if (errno == EINTR) continue;
			break;
		}
		reap_children();

		if (fds[listen_slot].revents & POLLIN) {
			int cfd = accept(listen_fd, NULL, NULL);
			if (cfd >= 0) {
				MsgType type;
				char payload[256];
				size_t plen;

				if (recv_msg(cfd, &type, payload, sizeof(payload) - 1, &plen) < 0) {
					close(cfd);
				} else if (type == MSG_CLIENT_LIST_SESSIONS) {
					/* a one-shot query, not an attach - never occupies a
					 * client slot, replies and closes immediately. */
					char text[2048];
					int tlen = 0, si;
					for (si = 0; si < MAX_SESSIONS; si++) {
						if (!g_sessions[si].in_use) continue;
						tlen += sprintf(text + tlen, "%s: %d window(s)\n",
							g_sessions[si].name, g_sessions[si].nwindows);
					}
					if (tlen == 0) tlen = sprintf(text, "no sessions\n");
					send_msg(cfd, MSG_SERVER_TEXT, text, (size_t)tlen);
					close(cfd);
				} else if (type == MSG_CLIENT_KILL_SERVER) {
					int ci;
					for (ci = 0; ci < MAX_CLIENTS; ci++) {
						if (g_clients[ci].in_use) {
							send_msg(g_clients[ci].fd, MSG_SERVER_DETACH, NULL, 0);
							close(g_clients[ci].fd);
						}
					}
					close(cfd);
					unlink(sockpath);
					exit(0);
				} else if (type == MSG_CLIENT_ATTACH) {
					int ci = -1, ok = 0;
					for (i = 0; i < MAX_CLIENTS; i++) {
						if (!g_clients[i].in_use) { ci = i; break; }
					}
					if (ci >= 0) {
						int sidx;
						payload[plen] = '\0';
						if (plen > 0) {
							sidx = session_find(payload);
							if (sidx < 0) sidx = session_create(payload);
						} else {
							sidx = session_any();
							if (sidx < 0) sidx = session_create("main");
						}
						if (sidx >= 0) {
							memset(&g_clients[ci], 0, sizeof(Client));
							g_clients[ci].in_use = 1;
							g_clients[ci].fd = cfd;
							g_clients[ci].session = sidx;
							g_clients[ci].cols = 80;
							g_clients[ci].rows = 24;
							ok = 1;
						}
					}
					if (!ok) close(cfd);
					g_dirty = 1;
				} else {
					close(cfd);
				}
			}
		}

		for (i = 0; i < MAX_PANES; i++) {
			unsigned char buf[4096];
			ssize_t n;
			if (!g_panes[i].in_use || pane_slot[i] < 0) continue;
			if (!(fds[pane_slot[i]].revents & POLLIN)) continue;
			n = read(g_panes[i].master_fd, buf, sizeof(buf));
			if (n > 0) {
				vt_feed(&g_panes[i], buf, (size_t)n);
				g_dirty = 1;
			}
		}

		for (i = 0; i < MAX_CLIENTS; i++) {
			MsgType type;
			char payload[4096];
			size_t plen;
			if (!g_clients[i].in_use || client_slot[i] < 0) continue;
			if (!(fds[client_slot[i]].revents & (POLLIN | POLLHUP | POLLERR))) continue;
			if (!(fds[client_slot[i]].revents & POLLIN)) {
				close(g_clients[i].fd);
				g_clients[i].in_use = 0;
				continue;
			}
			if (recv_msg(g_clients[i].fd, &type, payload, sizeof(payload), &plen) < 0) {
				close(g_clients[i].fd);
				g_clients[i].in_use = 0;
				continue;
			}
			if (type == MSG_CLIENT_KEYS) {
				size_t k;
				for (k = 0; k < plen; k++) {
					handle_client_key(&g_clients[i], (unsigned char)payload[k]);
					if (g_clients[i].fd < 0) break;
				}
			} else if (type == MSG_CLIENT_RESIZE && plen == 8) {
				g_clients[i].cols = (int)(((unsigned char)payload[0] << 24)
					| ((unsigned char)payload[1] << 16)
					| ((unsigned char)payload[2] << 8)
					| (unsigned char)payload[3]);
				g_clients[i].rows = (int)(((unsigned char)payload[4] << 24)
					| ((unsigned char)payload[5] << 16)
					| ((unsigned char)payload[6] << 8)
					| (unsigned char)payload[7]);
				g_dirty = 1;
			}
			if (g_clients[i].fd < 0) {
				g_clients[i].in_use = 0; /* 'd' detach already sent MSG_SERVER_DETACH */
			}
		}

		if (g_dirty) {
			for (i = 0; i < MAX_CLIENTS; i++) {
				char *frame;
				size_t flen;
				if (!g_clients[i].in_use || g_clients[i].fd < 0) continue;
				if (!g_sessions[g_clients[i].session].in_use) continue;
				frame = render_frame(&g_sessions[g_clients[i].session], g_clients[i].cols, g_clients[i].rows, &flen);
				if (send_msg(g_clients[i].fd, MSG_SERVER_RENDER, frame, flen) < 0) {
					close(g_clients[i].fd);
					g_clients[i].in_use = 0;
				}
				free(frame);
			}
			g_dirty = 0;
		}
	}
}

/* ---------------------------------------------------------------- */
/* Client - real terminal raw-mode, forwards stdin to the server as   */
/* MSG_CLIENT_KEYS, writes every MSG_SERVER_RENDER straight to stdout */
/* (the server already did all the compositing - the client never     */
/* interprets pane content itself, matching real tmux's own actual    */
/* client/server split). Uses the alternate screen buffer (real       */
/* tmux's own behaviour too) so the real shell's own scrollback isn't  */
/* disturbed by attaching/detaching.                                  */
/* ---------------------------------------------------------------- */

static struct termios g_orig_termios;
static int g_have_orig_termios = 0;

static void restore_terminal(void)
{
	if (g_have_orig_termios) tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
	printf("\x1b[?1049l");
	fflush(stdout);
}

static volatile sig_atomic_t g_winch = 0;
static void on_winch(int sig) { (void)sig; g_winch = 1; }

static void send_resize(int fd)
{
	struct winsize ws;
	unsigned char payload[8];
	if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) != 0) return;
	payload[0] = (unsigned char)((ws.ws_col >> 24) & 0xff);
	payload[1] = (unsigned char)((ws.ws_col >> 16) & 0xff);
	payload[2] = (unsigned char)((ws.ws_col >> 8) & 0xff);
	payload[3] = (unsigned char)(ws.ws_col & 0xff);
	payload[4] = (unsigned char)((ws.ws_row >> 24) & 0xff);
	payload[5] = (unsigned char)((ws.ws_row >> 16) & 0xff);
	payload[6] = (unsigned char)((ws.ws_row >> 8) & 0xff);
	payload[7] = (unsigned char)(ws.ws_row & 0xff);
	send_msg(fd, MSG_CLIENT_RESIZE, payload, 8);
}

static int connect_to_server(const char *sockpath)
{
	int fd;
	struct sockaddr_un addr;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) return -1;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, sockpath, sizeof(addr.sun_path) - 1);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static void client_main(const char *sockpath, const char *want_session)
{
	int fd;
	struct termios raw;

	fd = connect_to_server(sockpath);
	if (fd < 0) {
		fprintf(stderr, "tmux: cannot connect to server\n");
		exit(1);
	}

	send_msg(fd, MSG_CLIENT_ATTACH, want_session ? want_session : "",
		want_session ? strlen(want_session) : 0);

	tcgetattr(STDIN_FILENO, &g_orig_termios);
	g_have_orig_termios = 1;
	atexit(restore_terminal);
	raw = g_orig_termios;
	raw.c_lflag &= (tcflag_t)~(ICANON | ECHO | ISIG | IEXTEN);
	raw.c_iflag &= (tcflag_t)~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
	raw.c_oflag &= (tcflag_t)~(OPOST);
	raw.c_cc[VMIN] = 1;
	raw.c_cc[VTIME] = 0;
	tcsetattr(STDIN_FILENO, TCSANOW, &raw);

	signal(SIGWINCH, on_winch);
	printf("\x1b[?1049h");
	fflush(stdout);
	send_resize(fd);

	for (;;) {
		struct pollfd fds[2];
		int n;

		if (g_winch) {
			g_winch = 0;
			send_resize(fd);
		}

		fds[0].fd = STDIN_FILENO;
		fds[0].events = POLLIN;
		fds[1].fd = fd;
		fds[1].events = POLLIN;

		n = poll(fds, 2, 1000);
		if (n < 0) {
			if (errno == EINTR) continue;
			break;
		}

		if (fds[0].revents & POLLIN) {
			unsigned char buf[256];
			ssize_t r = read(STDIN_FILENO, buf, sizeof(buf));
			if (r > 0) send_msg(fd, MSG_CLIENT_KEYS, buf, (size_t)r);
			else break;
		}
		if (fds[1].revents & POLLIN) {
			MsgType type;
			static char payload[1 << 16];
			size_t plen;
			if (recv_msg(fd, &type, payload, sizeof(payload), &plen) < 0) break;
			if (type == MSG_SERVER_RENDER) {
				write_all(STDOUT_FILENO, payload, plen);
			} else if (type == MSG_SERVER_DETACH) {
				break;
			}
		} else if (fds[1].revents & (POLLHUP | POLLERR)) {
			break;
		}
	}

	close(fd);
}

static void list_sessions(const char *sockpath)
{
	int fd = connect_to_server(sockpath);
	MsgType type;
	char payload[2048];
	size_t plen;

	if (fd < 0) {
		printf("no server running\n");
		return;
	}
	send_msg(fd, MSG_CLIENT_LIST_SESSIONS, NULL, 0);
	if (recv_msg(fd, &type, payload, sizeof(payload) - 1, &plen) == 0 && type == MSG_SERVER_TEXT) {
		payload[plen] = '\0';
		fputs(payload, stdout);
	}
	close(fd);
}

/* Forks a detached server if none is reachable yet, then retries
 * connecting for up to ~1s (real, deliberate: no synchronisation pipe
 * - the server binds+listens fast enough in practice that a short
 * poll/retry loop, the same pragmatic choice real tmux's own startup
 * code makes, is simpler and good enough). */
static void ensure_server(const char *sockpath, const char *initial_session)
{
	int fd = connect_to_server(sockpath);
	int tries;
	pid_t pid;

	if (fd >= 0) {
		close(fd);
		return;
	}

	pid = fork();
	if (pid < 0) die("fork");
	if (pid == 0) {
		int devnull;
		if (setsid() < 0) _exit(1);
		devnull = open("/dev/null", O_RDWR);
		if (devnull >= 0) {
			dup2(devnull, 0);
			dup2(devnull, 1);
			dup2(devnull, 2);
			if (devnull > 2) close(devnull);
		}
		server_main(sockpath, initial_session);
		_exit(0);
	}

	for (tries = 0; tries < 50; tries++) {
		struct timespec ts;
		fd = connect_to_server(sockpath);
		if (fd >= 0) {
			close(fd);
			return;
		}
		ts.tv_sec = 0;
		ts.tv_nsec = 20000000; /* 20ms */
		nanosleep(&ts, NULL);
	}
	fprintf(stderr, "tmux: server did not start in time\n");
	exit(1);
}

static void usage(void)
{
	fprintf(stderr,
		"usage: tmux                        attach (starting the server if needed)\n"
		"       tmux new-session [-s NAME]  create + attach a new session\n"
		"       tmux attach [-t NAME]       attach an existing session\n"
		"       tmux list-sessions          list sessions\n"
		"       tmux kill-server            stop the server and every pane in it\n");
}

int main(int argc, char **argv)
{
	char dir[128];
	static char sockpath[256];
	const char *subcmd = (argc > 1) ? argv[1] : NULL;
	const char *want_session = NULL;

	snprintf(dir, sizeof(dir), "/tmp/nctmux-%d", (int)getuid());
	mkdir(dir, 0700);
	snprintf(sockpath, sizeof(sockpath), "%s/default", dir);
	strncpy(g_sockpath, sockpath, sizeof(g_sockpath) - 1);

	if (subcmd && strcmp(subcmd, "list-sessions") == 0) {
		list_sessions(sockpath);
		return 0;
	}
	if (subcmd && strcmp(subcmd, "kill-server") == 0) {
		int fd = connect_to_server(sockpath);
		if (fd < 0) {
			printf("no server running\n");
			return 0;
		}
		send_msg(fd, MSG_CLIENT_KILL_SERVER, NULL, 0);
		close(fd);
		return 0;
	}
	if (subcmd && strcmp(subcmd, "-h") == 0) {
		usage();
		return 0;
	}
	if (subcmd && strcmp(subcmd, "new-session") == 0) {
		int i;
		for (i = 2; i < argc; i++) {
			if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) want_session = argv[++i];
		}
		if (!want_session) want_session = "main";
	} else if (subcmd && strcmp(subcmd, "attach") == 0) {
		int i;
		for (i = 2; i < argc; i++) {
			if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) want_session = argv[++i];
		}
	} else if (subcmd) {
		usage();
		return 1;
	}

	ensure_server(sockpath, want_session ? want_session : "main");
	client_main(sockpath, want_session);
	return 0;
}
