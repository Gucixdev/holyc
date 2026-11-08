/* tui.h - `colorado -tui`: live swatches of every role, a mock window drawn in the theme, H/S/L sliders, hex entry, WCAG warnings, save.
 * stb-style single header. Raw termios + ANSI truecolor, alternate screen, restored on every exit path (also SIGINT/SIGTERM/SIGHUP).
 * Keys: j/k role, h/l lightness, H/L lightness x5, [ ] hue, - = saturation, x hex, d dark/light flag, u revert role, w save, ? help, q quit.
 */
#ifndef COLORADO_TUI_H
#define COLORADO_TUI_H
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include "gen.h"

static struct termios co_tui_old; static int co_tui_on;
static void co_tui_off(void) {
	if (!co_tui_on) return;
	co_tui_on = 0;
	const char *rs = "\033[0m\033[?25h\033[?1049l";
	if (write(1, rs, strlen(rs)) < 0) { /* terminal gone */ }
	tcsetattr(0, TCSAFLUSH, &co_tui_old);
}
static void co_tui_sig(int s) { co_tui_off(); _exit(128 + s); }
static void co_tui_winch(int s) { (void)s; }

static void co_t_fg(co_sb *o, co_rgb c) { co_sb_f(o, "\033[38;2;%d;%d;%dm", c.r, c.g, c.b); }
static void co_t_bg(co_sb *o, co_rgb c) { co_sb_f(o, "\033[48;2;%d;%d;%dm", c.r, c.g, c.b); }

/* visible-width-padded cell: text cut/padded to w bytes (ASCII only on purpose; role names and hex are ASCII) */
static void co_t_cell(co_sb *o, co_rgb fg, co_rgb bg, int w, const char *fmt, ...) __attribute__((format(printf, 5, 6)));
static void co_t_cell(co_sb *o, co_rgb fg, co_rgb bg, int w, const char *fmt, ...) {
	char buf[256]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
	int n = (int)strlen(buf); if (n > w) n = w;
	co_t_fg(o, fg); co_t_bg(o, bg); co_sb_addn(o, buf, (size_t)n);
	for (int i = n; i < w; i++) co_sb_add(o, " ");
	co_sb_add(o, "\033[0m");
}

static void co_tui_frame(const co_theme *t, const co_theme *orig, int sel, int dirty, const char *msg, const char *prompt, int help, int rows, int cols, co_sb *o) {
	const co_rgb *c = t->c; co_rgb tb = co_rgb_of(0, 0, 0), tf = co_rgb_of(220, 220, 220);
	co_sb_add(o, "\033[H\033[0m");
	int leftw = 30, rightx = leftw + 2, rw = cols - rightx; if (rw > 46) rw = 46;
	int body = rows - 2; if (body < 6) body = 6;
	int first = sel - body / 2 + 1; if (first < 0) first = 0; if (first > CR_N - body + 1) first = CR_N - body + 1; if (first < 0) first = 0;
	/* right panel lines */
	co_sb rp[48]; memset(rp, 0, sizeof rp); int nr = 0;
	if (rw >= 30) {
		double h, s, l; co_to_hsl(c[sel], &h, &s, &l);
		int ww = rw - 2;
		co_t_fg(&rp[nr], c[CR_BORDER_FOCUS]); co_t_bg(&rp[nr], c[CR_BG]); co_sb_add(&rp[nr], "+"); for (int i = 0; i < ww; i++) co_sb_add(&rp[nr], "-"); co_sb_add(&rp[nr], "+\033[0m"); nr++;
#define ROWC(fgc, bgc, ...) do { co_t_fg(&rp[nr], c[CR_BORDER_FOCUS]); co_t_bg(&rp[nr], c[CR_BG]); co_sb_add(&rp[nr], "|"); co_t_cell(&rp[nr], fgc, bgc, ww, __VA_ARGS__); co_t_fg(&rp[nr], c[CR_BORDER_FOCUS]); co_t_bg(&rp[nr], c[CR_BG]); co_sb_add(&rp[nr], "|\033[0m"); nr++; } while (0)
		ROWC(c[CR_FG], c[CR_BG_ALT], " %s - preview  (title bar)", t->name);
		ROWC(c[CR_FG_DIM], c[CR_BG], " File  Edit  View  Help");
		ROWC(c[CR_FG], c[CR_BG], " Body text on the window background");
		ROWC(c[CR_FG_DIM], c[CR_BG], " Secondary text, hints, placeholders");
		ROWC(c[CR_FG], c[CR_BG_ALT], " Panel / input field (bg_alt)");
		ROWC(c[CR_SEL_FG], c[CR_SEL_BG], " Selected list item");
		ROWC(c[CR_ACCENT_FG], c[CR_ACCENT], " [ Primary button ]");
		ROWC(c[CR_LINK], c[CR_BG], " link: https://example.org/path");
		ROWC(c[CR_OK], c[CR_BG], " ok: saved");
		ROWC(c[CR_WARN], c[CR_BG], " warn: low contrast somewhere");
		ROWC(c[CR_URGENT], c[CR_BG], " urgent: failed");
		{ co_t_fg(&rp[nr], c[CR_BORDER_FOCUS]); co_t_bg(&rp[nr], c[CR_BG]); co_sb_add(&rp[nr], "|"); co_t_bg(&rp[nr], c[CR_BG]); co_sb_add(&rp[nr], " ");
		  for (int i = 0; i < 16; i++) { co_t_bg(&rp[nr], c[CR_ANSI0 + i]); co_sb_add(&rp[nr], i < 8 ? "  " : "  "); }
		  co_t_bg(&rp[nr], c[CR_BG]); for (int i = 33; i < ww; i++) co_sb_add(&rp[nr], " ");
		  co_t_fg(&rp[nr], c[CR_BORDER_FOCUS]); co_sb_add(&rp[nr], "|\033[0m"); nr++; }
		co_t_fg(&rp[nr], c[CR_BORDER_FOCUS]); co_t_bg(&rp[nr], c[CR_BG]); co_sb_add(&rp[nr], "+"); for (int i = 0; i < ww; i++) co_sb_add(&rp[nr], "-"); co_sb_add(&rp[nr], "+\033[0m"); nr++;
		nr++; /* blank */
		co_t_cell(&rp[nr], tf, tb, rw, " %s = %s   H %3.0f  S %3.0f%%  L %3.0f%%", co_role_name(sel), co_h(c[sel]), h, s * 100, l * 100); nr++;
		static const char *nm[3] = { "H", "S", "L" }; double v[3] = { h / 360, s, l };
		for (int k = 0; k < 3; k++) {
			int bw = rw - 6; if (bw > 40) bw = 40; int fill = (int)lround(v[k] * bw);
			co_t_fg(&rp[nr], tf); co_t_bg(&rp[nr], tb); co_sb_f(&rp[nr], " %s ", nm[k]);
			for (int i = 0; i < bw; i++) { co_rgb p = k == 0 ? co_from_hsl(360.0 * i / bw, s < 0.2 ? 0.7 : s, 0.5) : k == 1 ? co_from_hsl(h, (double)i / bw, l) : co_from_hsl(h, s, (double)i / bw);
				co_t_bg(&rp[nr], p); co_t_fg(&rp[nr], co_rgb_of(255, 255, 255)); if (i == fill) co_sb_add(&rp[nr], "|"); else co_sb_add(&rp[nr], " "); }
			co_sb_add(&rp[nr], "\033[0m"); nr++;
		}
		nr++;
		int fails = 0;
		for (size_t i = 0; i < sizeof co_pairs / sizeof *co_pairs; i++) {
			double r = co_contrast(c[co_pairs[i].fg], c[co_pairs[i].bg]);
			if (r + 1e-9 >= co_pairs[i].min) continue;
			fails++;
			if (nr < 46 && nr < body) { co_t_cell(&rp[nr], c[CR_URGENT], tb, rw, " ! %s on %s %.2f < %.1f", co_role_name(co_pairs[i].fg), co_role_name(co_pairs[i].bg), r, co_pairs[i].min); nr++; }
		}
		if (!fails && nr < 47) { co_t_cell(&rp[nr], c[CR_OK], tb, rw, " contrast: all %d pairs pass", (int)(sizeof co_pairs / sizeof *co_pairs)); nr++; }
#undef ROWC
	}
	co_sb_f(o, "\033[2K"); co_t_cell(o, tf, tb, cols, " colorado  theme %s (%s)%s   from %s", t->name, t->dark ? "dark" : "light", dirty ? "  [modified]" : "", orig->name); co_sb_add(o, "\r\n");
	for (int r = 0; r < body; r++) {
		int i = first + r; co_sb_add(o, "\033[2K");
		if (i < CR_N) {
			int cur = i == sel; co_rgb rowbg = cur ? co_rgb_of(60, 60, 70) : tb;
			co_t_cell(o, tf, rowbg, 1, "%s", cur ? ">" : " ");
			co_t_cell(o, tf, rowbg, 14, " %s", co_role_name(i));
			co_t_cell(o, tf, rowbg, 9, "%s", co_h(c[i]));
			co_t_fg(o, c[i]); co_t_bg(o, c[i]); co_sb_add(o, "      \033[0m");
			if (!co_eq(c[i], orig->c[i])) co_t_cell(o, tf, tb, 1, "*"); else co_sb_add(o, " ");
		} else co_t_cell(o, tf, tb, leftw + 1, "%s", "");
		if (r < nr && rp[r].p) { co_sb_add(o, "  "); co_sb_addn(o, rp[r].p, rp[r].n); }
		co_sb_add(o, "\r\n");
	}
	for (int i = 0; i < nr; i++) co_sb_free(&rp[i]);
	co_sb_add(o, "\033[2K");
	if (prompt) { co_t_cell(o, tf, tb, cols, " %s", prompt); }
	else co_t_cell(o, tf, tb, cols, " %s", msg && *msg ? msg : cols >= 84 ? "j/k role  h/l light  [/] hue  -/= sat  x hex  d dark  u revert  w save  ? help  q quit" : "j/k h/l x hex w save ? help q quit");
	if (help) {
		static const char *hl[] = { " colorado - edit a theme live ", "",
			" j / k, arrows   select role", " h / l            lightness -/+ 2%   (H / L: 10%)", " [ / ]            hue -/+ 5 degrees", " - / =            saturation -/+ 5%",
			" x                type a hex colour, Enter applies", " d                flip the dark/light flag", " u                revert this role to the loaded value",
			" w                save to the theme file", " ?                this help", " q                quit (asks once if unsaved)", "", " red lines on the right = failing WCAG pairs ", " press any key " };
		int n = (int)(sizeof hl / sizeof *hl), pw = 56, top = (rows - n) / 2, left = (cols - pw) / 2; if (top < 1) top = 1; if (left < 0) left = 0;
		for (int i = 0; i < n; i++) { co_sb_f(o, "\033[%d;%dH", top + i + 1, left + 1); co_t_cell(o, co_rgb_of(0, 0, 0), co_rgb_of(240, 240, 240), pw, "%s", hl[i]); }
	}
}

typedef struct { co_theme t, orig; int sel, dirty; char path[PATH_MAX]; } co_tui_state;

static int co_tui_save(co_tui_state *s, char *msg, size_t mn) {
	co_sb o = { 0 }; co_theme_emit(&s->t, &o);
	char tmp[PATH_MAX + 8]; snprintf(tmp, sizeof tmp, "%s.tmp", s->path);
	FILE *f = fopen(tmp, "w");
	if (!f || fwrite(o.p, 1, o.n, f) != o.n || fclose(f) || rename(tmp, s->path)) { snprintf(msg, mn, "save FAILED: %s", strerror(errno)); co_sb_free(&o); return -1; }
	co_sb_free(&o); s->dirty = 0; s->orig = s->t; snprintf(msg, mn, "saved %s", s->path); return 0;
}

static int co_tui_key(void) {
	unsigned char b[8]; ssize_t n = read(0, b, 1);
	if (n < 0 && errno == EINTR) return 0; /* SIGWINCH: just redraw */
	if (n <= 0) return -1;
	if (b[0] != 27) return b[0];
	fd_set fs; struct timeval tv = { 0, 50000 }; FD_ZERO(&fs); FD_SET(0, &fs);
	if (select(1, &fs, NULL, NULL, &tv) <= 0) return 27;
	if (read(0, b, 2) != 2) return 27;
	if (b[0] == '[' || b[0] == 'O') return b[1] == 'A' ? 'k' : b[1] == 'B' ? 'j' : b[1] == 'C' ? 'l' : b[1] == 'D' ? 'h' : 0;
	return 0;
}

/* returns process exit code */
static int co_tui(const co_theme *start, const char *savepath) {
	if (!isatty(0) || !isatty(1)) { fprintf(stderr, "colorado: the TUI needs a terminal\n"); return 2; }
	co_tui_state s; memset(&s, 0, sizeof s); s.t = s.orig = *start; snprintf(s.path, sizeof s.path, "%s", savepath);
	tcgetattr(0, &co_tui_old); struct termios raw = co_tui_old; cfmakeraw(&raw); raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0;
	tcsetattr(0, TCSAFLUSH, &raw); co_tui_on = 1;
	{ struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = co_tui_winch; sigaction(SIGWINCH, &sa, NULL); }
	signal(SIGINT, co_tui_sig); signal(SIGTERM, co_tui_sig); signal(SIGHUP, co_tui_sig);
	if (write(1, "\033[?1049h\033[?25l", 14) < 0) { /* ignore */ }
	char msg[300] = "", prompt[80] = ""; int help = 0, in_hex = 0, quit_armed = 0, hl = 0;
	for (;;) {
		struct winsize ws; int rows = 24, cols = 80; if (!ioctl(1, TIOCGWINSZ, &ws) && ws.ws_row > 4 && ws.ws_col > 20) { rows = ws.ws_row; cols = ws.ws_col; }
		co_sb o = { 0 }; char pr[120];
		if (in_hex) snprintf(pr, sizeof pr, "hex for %s (Enter apply, Esc cancel): #%s_", co_role_name(s.sel), prompt); 
		co_tui_frame(&s.t, &s.orig, s.sel, s.dirty, msg, in_hex ? pr : NULL, help, rows, cols, &o);
		if (write(1, o.p, o.n) < 0) { co_sb_free(&o); break; }
		co_sb_free(&o);
		int k = co_tui_key(); if (k < 0) break;
		if (help) { help = 0; continue; }
		msg[0] = 0;
		if (in_hex) {
			if (k == 27) { in_hex = 0; hl = 0; prompt[0] = 0; }
			else if (k == '\r' || k == '\n') {
				co_rgb v; char tmpx[16]; snprintf(tmpx, sizeof tmpx, "#%s", prompt);
				if (!co_hex_parse(tmpx, &v)) { s.t.c[s.sel] = v; s.dirty = 1; } else snprintf(msg, sizeof msg, "not a hex colour: %s", tmpx);
				in_hex = 0; hl = 0; prompt[0] = 0;
			} else if ((k == 127 || k == 8) && hl) prompt[--hl] = 0;
			else if (isxdigit(k) && hl < 6) { prompt[hl++] = (char)k; prompt[hl] = 0; }
			continue;
		}
		double h, sa, l; co_to_hsl(s.t.c[s.sel], &h, &sa, &l); int ch = 0;
		if (k != 'q') quit_armed = 0;
		switch (k) {
		case 'j': s.sel = (s.sel + 1) % CR_N; break;
		case 'k': s.sel = (s.sel + CR_N - 1) % CR_N; break;
		case 'h': l -= 0.02; ch = 1; break;
		case 'l': l += 0.02; ch = 1; break;
		case 'H': l -= 0.10; ch = 1; break;
		case 'L': l += 0.10; ch = 1; break;
		case '[': h -= 5; ch = 1; break;
		case ']': h += 5; ch = 1; break;
		case '-': sa -= 0.05; ch = 1; break;
		case '=': case '+': sa += 0.05; ch = 1; break;
		case 'x': in_hex = 1; hl = 0; prompt[0] = 0; break;
		case 'd': s.t.dark = !s.t.dark; s.dirty = 1; break;
		case 'u': s.t.c[s.sel] = s.orig.c[s.sel]; break;
		case 'w': co_tui_save(&s, msg, sizeof msg); break;
		case '?': help = 1; break;
		case 3: co_tui_off(); return 0; /* Ctrl-C: raw mode delivers it as a key; never leave the terminal raw */
		case 'q': if (s.dirty && !quit_armed) { quit_armed = 1; snprintf(msg, sizeof msg, "unsaved changes - q again to quit, w to save"); break; } co_tui_off(); return 0;
		default: break;
		}
		if (ch) { s.t.c[s.sel] = co_from_hsl(h, sa, l); s.dirty = 1; }
		{ int d = 0; for (int i = 0; i < CR_N; i++) if (!co_eq(s.t.c[i], s.orig.c[i])) d = 1; if (s.t.dark != s.orig.dark) d = 1; s.dirty = d; }
	}
	co_tui_off();
	return 0;
}
#endif
