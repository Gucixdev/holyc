/* tty.c - the real terminal: capability detection, raw mode, output, and decoding of keys, mouse, paste and the
 * kitty keyboard protocol (which is the only way to learn which modifiers are HELD). Written to work on everything
 * from a kernel console (TERM=linux: no alternate screen, 16 colours, no unicode drawing) to kitty/foot. */
#include "lazytui.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

struct lt_tty {
	int fd;
	struct termios saved;
	bool raw, alt, mouse_on, motion_on, kitty_pushed;
	lt_caps caps;
	int origin, inline_rows;
	unsigned char in[65536];
	size_t len, pos;
	int held;
	lt_event queued[8]; int nq;
};

static volatile sig_atomic_t g_winch;
static void on_winch(int s) { (void)s; g_winch = 1; }

double lt_now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

lt_caps lt_caps_detect(void)
{
	lt_caps c = {true, true, false, true, 256};
	const char *term = getenv("TERM"), *ct = getenv("COLORTERM"), *lc = getenv("LC_ALL");
	if (!lc || !*lc) lc = getenv("LC_CTYPE");
	if (!lc || !*lc) lc = getenv("LANG");
	bool utf = lc && (strstr(lc, "UTF-8") || strstr(lc, "utf-8") || strstr(lc, "UTF8") || strstr(lc, "utf8"));
	bool linux_console = term && !strncmp(term, "linux", 5);
	c.unicode = utf && !linux_console;
	c.altscreen = !linux_console && !(term && !strcmp(term, "dumb"));
	c.mouse = !linux_console;
	if (linux_console) c.colors = 16;                          /* an inherited COLORTERM (ssh, sudo) must not promise colours the console lacks */
	else if (ct && (strstr(ct, "truecolor") || strstr(ct, "24bit"))) c.colors = 1 << 24;
	else if (term && strstr(term, "256")) c.colors = 256;
	else if (term && !strcmp(term, "dumb")) c.colors = 8;
	else if (term && (strstr(term, "xterm") || strstr(term, "foot") || strstr(term, "kitty") || strstr(term, "alacritty") || strstr(term, "tmux") || strstr(term, "screen"))) c.colors = 256;
	else c.colors = 16;
	return c;
}

static void raw_write(lt_tty *t, const char *s, size_t n)
{
	size_t off = 0;
	while (off < n) {
		ssize_t w = write(t->fd, s + off, n - off);
		if (w < 0) { if (errno == EINTR || errno == EAGAIN) continue; return; }
		off += (size_t)w;
	}
}
void lt_tty_write(lt_tty *t, const char *s, size_t n) { raw_write(t, s, n); }
static void puts_tty(lt_tty *t, const char *s) { raw_write(t, s, strlen(s)); }

static int cursor_row(lt_tty *t)
{
	puts_tty(t, "\033[6n");
	unsigned char b[32];
	size_t n = 0;
	struct pollfd p = {t->fd, POLLIN, 0};
	while (n < sizeof b - 1 && poll(&p, 1, 300) > 0) {
		if (read(t->fd, b + n, 1) != 1) break;
		n++;
		if (b[n - 1] == 'R') break;
	}
	b[n] = 0;
	int row, col;
	return sscanf((char *)b, "\033[%d;%dR", &row, &col) == 2 ? row : -1;
}

static bool kitty_query(lt_tty *t)
{
	puts_tty(t, "\033[?u\033[c");                     /* the terminal answers CSI c whatever it is: no answer to ?u = no protocol */
	unsigned char c;
	int state = 0;
	bool got_u = false;
	char seq[32];
	size_t sl = 0;
	struct pollfd p = {t->fd, POLLIN, 0};
	while (poll(&p, 1, 150) > 0 && read(t->fd, &c, 1) == 1) {
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
	return got_u;
}

lt_tty *lt_tty_open(lt_tty_opts o)
{
	lt_tty *t = calloc(1, sizeof *t);
	t->fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
	if (t->fd < 0 || tcgetattr(t->fd, &t->saved) < 0) { if (t->fd >= 0) close(t->fd); free(t); return NULL; }
	struct termios r = t->saved;
	r.c_iflag &= ~(unsigned)(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
	r.c_oflag &= ~(unsigned)OPOST;
	r.c_lflag &= ~(unsigned)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
	r.c_cflag |= CS8;
	r.c_cc[VMIN] = 1; r.c_cc[VTIME] = 0;
	tcsetattr(t->fd, TCSANOW, &r);
	t->raw = true;
	struct sigaction sa = {0};
	sa.sa_handler = on_winch;
	sigaction(SIGWINCH, &sa, NULL);
	t->caps = lt_caps_detect();
	t->inline_rows = o.inline_rows;
	t->alt = o.alt_screen && t->caps.altscreen && o.inline_rows <= 0;
	if (t->alt) puts_tty(t, "\033[?1049h");
	puts_tty(t, "\033[?7l\033[?25l\033[?2004h");                  /* no autowrap, hidden cursor, bracketed paste */
	if (o.mouse && t->caps.mouse) {
		puts_tty(t, "\033[?1000h\033[?1002h\033[?1006h");
		t->mouse_on = true;
		if (o.motion) { puts_tty(t, "\033[?1003h"); t->motion_on = true; }
	} else t->caps.mouse = false;
	if (o.kitty && t->caps.altscreen && kitty_query(t)) { puts_tty(t, "\033[>31u\033[?1004h"); t->kitty_pushed = true; t->caps.kitty = true; }
	if (t->inline_rows > 0) {                                     /* reserve rows below the cursor */
		int rows, cols;
		lt_tty_size(t, &rows, &cols);
		int r0 = cursor_row(t);
		if (r0 < 1) r0 = rows;
		puts_tty(t, "\r");
		for (int i = 0; i < t->inline_rows - 1; i++) puts_tty(t, "\n");
		int last = r0 + t->inline_rows - 1;
		if (last > rows) last = rows;
		t->origin = last - t->inline_rows;
	}
	return t;
}

void lt_tty_close(lt_tty *t)
{
	if (!t) return;
	if (t->kitty_pushed) puts_tty(t, "\033[?1004l\033[<u");
	if (t->motion_on) puts_tty(t, "\033[?1003l");
	if (t->mouse_on) puts_tty(t, "\033[?1006l\033[?1002l\033[?1000l");
	if (t->inline_rows > 0) {                                     /* wipe the reserved rows, leave the cursor at their top */
		char b[32];
		for (int i = 0; i < t->inline_rows; i++) { int n = snprintf(b, sizeof b, "\033[%d;1H\033[2K", t->origin + i + 1); raw_write(t, b, (size_t)n); }
		int n = snprintf(b, sizeof b, "\033[%d;1H", t->origin + 1);
		raw_write(t, b, (size_t)n);
	}
	puts_tty(t, "\033[?2004l\033[?7h\033[?25h\033[0m");
	if (t->alt) puts_tty(t, "\033[?1049l");
	tcsetattr(t->fd, TCSANOW, &t->saved);
	close(t->fd);
	free(t);
}

lt_caps lt_tty_caps(const lt_tty *t) { return t->caps; }
int lt_tty_origin(const lt_tty *t) { return t->origin; }
int lt_tty_mods(const lt_tty *t) { return t->held; }
void lt_tty_size(const lt_tty *t, int *rows, int *cols)
{
	struct winsize ws;
	if (ioctl(t->fd, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) { *rows = t->inline_rows > 0 ? t->inline_rows : ws.ws_row; *cols = ws.ws_col; }
	else { *rows = t->inline_rows > 0 ? t->inline_rows : 24; *cols = 80; }
}

/* ------------------------------------------------------------------ decoding */
static int fill(lt_tty *t, int ms)
{
	if (t->pos > 0 && t->pos == t->len) t->pos = t->len = 0;
	if (t->len == sizeof t->in) return 0;
	struct pollfd p = {t->fd, POLLIN, 0};
	if (poll(&p, 1, ms) <= 0) return 0;
	ssize_t n = read(t->fd, t->in + t->len, sizeof t->in - t->len);
	if (n <= 0) return 0;
	t->len += (size_t)n;
	return 1;
}

static int csi_mods(int m)
{
	int r = 0;
	m = m > 0 ? m - 1 : 0;
	if (m & 1) r |= LT_MOD_SHIFT;
	if (m & 2) r |= LT_MOD_ALT;
	if (m & 4) r |= LT_MOD_CTRL;
	return r;
}

typedef struct { int v[16]; bool sub[16]; int n; } par;

/* returns: -1 incomplete, 1 = event in ev, 2 = swallowed, 3 = paste starts */
static int decode_csi(lt_tty *t, const unsigned char *s, size_t n, size_t *used, lt_event *ev)
{
	size_t i = 0;
	par P = {{0}, {false}, 0};
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
		have = true; i++;
	}
	if (i >= n) return -1;
	char f = (char)s[i++];
	*used = i;
	int nv = P.n, *v = P.v;
	int mfield = -1, event = 1;
	for (int k = 1, fld = 0; k < nv; k++) if (!P.sub[k]) { fld++; if (fld == 1) { mfield = k; break; } }
	if (mfield > 0 && mfield + 1 < nv && P.sub[mfield + 1]) event = v[mfield + 1];
	int mods = mfield > 0 ? csi_mods(v[mfield]) : 0;
	if (lead == '<' && (f == 'M' || f == 'm')) {
		int b = v[0], x = nv > 1 ? v[1] - 1 : 0, y = nv > 2 ? v[2] - 1 : 0;
		int m = 0;
		if (b & 4) m |= LT_MOD_SHIFT;
		if (b & 8) m |= LT_MOD_ALT;
		if (b & 16) m |= LT_MOD_CTRL;
		ev->kind = LT_EV_MOUSE; ev->x = x; ev->y = y - t->origin;
		if (b & 32) {                                                                                        /* a motion report */
			if ((b & 3) == 0 && f == 'M') { ev->key = LT_K_DRAG | m; return 1; }                            /* ...with the left button down */
			if ((b & 3) == 3 && f == 'M') { ev->key = LT_K_MOVE | m; return 1; }                            /* ...with no button held (needs motion mode) */
			return 2;
		}
		if (b & 64) { ev->key = ((b & 3) == 0 ? LT_K_WHEEL_UP : LT_K_WHEEL_DOWN) | m; return 1; }
		if (f == 'm') { if ((b & 3) == 0) { ev->key = LT_K_RELEASE | m; return 1; } return 2; }
		int btn = b & 3;
		if (btn == 0) ev->key = LT_K_CLICK | m; else if (btn == 2) ev->key = LT_K_RCLICK | m; else return 2;
		return 1;
	}
	if (lead) return 2;
	ev->kind = LT_EV_KEY;
	if (f == 'I' || f == 'O') { if (t->held) { t->held = 0; ev->kind = LT_EV_MODS; ev->mods = 0; return 1; } return 2; }
	if (t->kitty_pushed && f != 'u' && t->held != mods) { t->held = mods; }
	if (event == 3 && f != 'u') return 2;
	int base = 0;
	switch (f) {
	case 'A': base = LT_K_UP; break; case 'B': base = LT_K_DOWN; break; case 'C': base = LT_K_RIGHT; break; case 'D': base = LT_K_LEFT; break;
	case 'H': base = LT_K_HOME; break; case 'F': base = LT_K_END; break;
	case 'Z': base = LT_K_BTAB; mods = 0; break;
	case 'P': base = LT_K_F1; break; case 'Q': base = LT_K_F2; break; case 'R': base = LT_K_F3; break; case 'S': base = LT_K_F4; break;
	case '~':
		switch (v[0]) {
		case 1: case 7: base = LT_K_HOME; break; case 2: base = LT_K_INS; break; case 3: base = LT_K_DEL; break;
		case 4: case 8: base = LT_K_END; break; case 5: base = LT_K_PGUP; break; case 6: base = LT_K_PGDN; break;
		case 11: base = LT_K_F1; break; case 12: base = LT_K_F2; break; case 13: base = LT_K_F3; break; case 14: base = LT_K_F4; break;
		case 15: base = LT_K_F5; break; case 17: base = LT_K_F6; break; case 18: base = LT_K_F7; break; case 19: base = LT_K_F8; break;
		case 20: base = LT_K_F9; break; case 21: base = LT_K_F10; break; case 23: base = LT_K_F11; break; case 24: base = LT_K_F12; break;
		case 200: return 3;
		default: return 2;
		}
		break;
	case 'u': {
		int code = v[0];
		if (code >= 57441 && code <= 57454) {                      /* a modifier key itself: only the held set changes */
			int bit = (code == 57441 || code == 57447) ? LT_MOD_SHIFT : (code == 57442 || code == 57448) ? LT_MOD_CTRL : (code == 57443 || code == 57449) ? LT_MOD_ALT : 0;
			if (!bit) return 2;
			int nh = event == 3 ? (t->held & ~bit) : (t->held | bit);
			if (nh == t->held) return 2;
			t->held = nh; ev->kind = LT_EV_MODS; ev->mods = nh;
			return 1;
		}
		if (code >= 57358 && code <= 57360) return 2;
		if (event == 3) return 2;
		if (t->kitty_pushed) t->held = mods;
		int fld = 0, text = 0;
		for (int k = 1; k < nv; k++) { if (!P.sub[k]) fld++; if (fld == 2 && !text && v[k] >= 32) text = v[k]; }
		if (text && !(mods & (LT_MOD_CTRL | LT_MOD_ALT))) { ev->key = text; return 1; }
		/* keypad keys and the shifted alternate key (code:shifted:base) */
		static const struct { int code; int ch; } kp[] = {{57399, '0'}, {57400, '1'}, {57401, '2'}, {57402, '3'}, {57403, '4'}, {57404, '5'}, {57405, '6'},
		                                                 {57406, '7'}, {57407, '8'}, {57408, '9'}, {57409, '.'}, {57410, '/'}, {57411, '*'}, {57412, '-'}, {57413, '+'}, {57415, '='}};
		for (size_t q = 0; q < sizeof kp / sizeof kp[0]; q++) if (kp[q].code == code) code = kp[q].ch;
		if ((mods & LT_MOD_SHIFT) && !(mods & (LT_MOD_CTRL | LT_MOD_ALT)) && nv > 1 && P.sub[1] && v[1] >= 32) { ev->key = v[1]; return 1; }
		if ((mods & LT_MOD_SHIFT) && !(mods & (LT_MOD_CTRL | LT_MOD_ALT)) && code >= 32 && code < 127) {      /* no alternate key: assume a US layout */
			int ch = code;
			if (code >= 'a' && code <= 'z') ch = code - 32;
			else if (code == '=') ch = '+'; else if (code == '-') ch = '_'; else if (code == '1') ch = '!'; else if (code == '2') ch = '@'; else if (code == '3') ch = '#';
			else if (code == '4') ch = '$'; else if (code == '5') ch = '%'; else if (code == '6') ch = '^'; else if (code == '7') ch = '&'; else if (code == '8') ch = '*';
			else if (code == '9') ch = '('; else if (code == '0') ch = ')'; else if (code == ';') ch = ':'; else if (code == '\'') ch = '"'; else if (code == ',') ch = '<';
			else if (code == '.') ch = '>'; else if (code == '/') ch = '?'; else if (code == '[') ch = '{'; else if (code == ']') ch = '}'; else if (code == '\\') ch = '|';
			else if (code == '`') ch = '~';
			ev->key = ch;
			return 1;
		}
		if (!mods && code >= 32 && code < 0x110000 && code != 127) { ev->key = code; return 1; }
		base = code == 13 || code == 57414 ? LT_K_ENTER : code == 9 ? LT_K_TAB : code == 27 ? LT_K_ESC : code == 127 ? LT_K_BSPACE : code;
		if (mods & LT_MOD_CTRL) {
			if (base >= 'A' && base <= 'Z') base += 32;
			if (base == '/') base = '_';
			if (!(mods & ~LT_MOD_CTRL)) { if (base == 'i') { ev->key = LT_K_TAB; return 1; } if (base == 'm') { ev->key = LT_K_ENTER; return 1; } if (base == '[') { ev->key = LT_K_ESC; return 1; } }
		}
		if (base == LT_K_TAB && (mods & LT_MOD_SHIFT)) { ev->key = LT_K_BTAB | (mods & ~LT_MOD_SHIFT); return 1; }
		break;
	}
	default: return 2;
	}
	ev->key = base | mods;
	return 1;
}

static int decode(lt_tty *t, lt_event *ev)
{
	memset(ev, 0, sizeof *ev);
	for (;;) {
		if (t->pos >= t->len) return 0;
		const unsigned char *s = t->in + t->pos;
		size_t n = t->len - t->pos;
		unsigned char c = s[0];
		if (c == 27) {
			if (n == 1) { if (!fill(t, 25)) { t->pos++; ev->kind = LT_EV_KEY; ev->key = LT_K_ESC; return 1; } continue; }
			if (s[1] == '[' && n >= 3 && s[2] == '[') {                      /* Linux console F1-F5 */
				if (n < 4) { if (!fill(t, 25)) { t->pos = t->len; return 0; } continue; }
				if (s[3] >= 'A' && s[3] <= 'E') { t->pos += 4; ev->kind = LT_EV_KEY; ev->key = LT_K_F1 + (s[3] - 'A'); return 1; }
			}
			if (s[1] == '[') {
				size_t used = 0;
				int r = decode_csi(t, s + 2, n - 2, &used, ev);
				if (r < 0) { if (!fill(t, 25)) { t->pos = t->len; return 0; } continue; }
				t->pos += 2 + used;
				if (r == 3) {
					size_t cap = 256, len = 0;
					char *b = malloc(cap);
					for (;;) {
						while (t->pos < t->len) {
							if (t->len - t->pos >= 6 && !memcmp(t->in + t->pos, "\033[201~", 6)) { t->pos += 6; goto done; }
							if (len + 1 >= cap) { cap *= 2; b = realloc(b, cap); }
							b[len++] = (char)t->in[t->pos++];
						}
						if (!fill(t, 500)) break;
					}
				done:
					b[len] = 0;
					ev->kind = LT_EV_PASTE; ev->paste = b;
					return 1;
				}
				if (r == 2) { memset(ev, 0, sizeof *ev); continue; }
				return 1;
			}
			if (s[1] == 'O' && n >= 3) {
				t->pos += 3;
				ev->kind = LT_EV_KEY;
				switch (s[2]) {
				case 'P': ev->key = LT_K_F1; break; case 'Q': ev->key = LT_K_F2; break; case 'R': ev->key = LT_K_F3; break; case 'S': ev->key = LT_K_F4; break;
				case 'A': ev->key = LT_K_UP; break; case 'B': ev->key = LT_K_DOWN; break; case 'C': ev->key = LT_K_RIGHT; break; case 'D': ev->key = LT_K_LEFT; break;
				case 'H': ev->key = LT_K_HOME; break; case 'F': ev->key = LT_K_END; break;
				default: continue;
				}
				return 1;
			}
			if (s[1] == 'O' && n < 3) { if (!fill(t, 25)) { t->pos++; ev->kind = LT_EV_KEY; ev->key = LT_K_ESC; return 1; } continue; }
			t->pos++;                                                       /* alt + key */
			lt_event inner;
			int r = decode(t, &inner);
			if (!r) { ev->kind = LT_EV_KEY; ev->key = LT_K_ESC; return 1; }
			*ev = inner;
			if (ev->kind == LT_EV_KEY) ev->key |= LT_MOD_ALT;
			return 1;
		}
		ev->kind = LT_EV_KEY;
		if (c == 13) { t->pos++; ev->key = LT_K_ENTER; return 1; }
		if (c == 9) { t->pos++; ev->key = LT_K_TAB; return 1; }
		if (c == 127) { t->pos++; ev->key = LT_K_BSPACE; return 1; }
		if (c == 0) { t->pos++; ev->key = LT_MOD_CTRL | ' '; return 1; }
		if (c < 32) { t->pos++; ev->key = LT_MOD_CTRL | (c >= 28 ? ("\\]^_"[c - 28]) : ('a' + c - 1)); return 1; }
		uint32_t r;
		size_t l = lt_utf8_decode((const char *)s, &r);
		if (l > 1 && l > n) { if (!fill(t, 25)) { t->pos = t->len; return 0; } continue; }
		t->pos += l;
		ev->key = (int)r;
		return 1;
	}
}

int lt_tty_wait(lt_tty *t, int timeout_ms, lt_event *ev)
{
	double deadline = timeout_ms >= 0 ? lt_now() + timeout_ms / 1000.0 : 0;
	for (;;) {
		if (g_winch) { g_winch = 0; memset(ev, 0, sizeof *ev); ev->kind = LT_EV_RESIZE; return 1; }
		if (decode(t, ev)) return 1;
		int wait = -1;
		if (timeout_ms >= 0) { wait = (int)((deadline - lt_now()) * 1000.0 + 0.999); if (wait < 0) return 0; }
		struct pollfd p = {t->fd, POLLIN, 0};
		int r = poll(&p, 1, wait);
		if (r < 0 && errno == EINTR) continue;
		if (r == 0) return 0;
		if (r > 0) {
			if (t->pos > 0 && t->pos == t->len) t->pos = t->len = 0;
			ssize_t n = read(t->fd, t->in + t->len, sizeof t->in - t->len);
			if (n <= 0) return 0;
			t->len += (size_t)n;
		}
	}
}

/* ------------------------------------------------------------------ key names */
int lt_key_parse(const char *name)
{
	static const struct { const char *n; int k; } t[] = {
		{"enter", LT_K_ENTER}, {"return", LT_K_ENTER}, {"space", ' '}, {"tab", LT_K_TAB}, {"btab", LT_K_BTAB}, {"esc", LT_K_ESC}, {"escape", LT_K_ESC},
		{"bspace", LT_K_BSPACE}, {"backspace", LT_K_BSPACE}, {"del", LT_K_DEL}, {"delete", LT_K_DEL}, {"ins", LT_K_INS}, {"insert", LT_K_INS},
		{"up", LT_K_UP}, {"down", LT_K_DOWN}, {"left", LT_K_LEFT}, {"right", LT_K_RIGHT}, {"home", LT_K_HOME}, {"end", LT_K_END},
		{"pgup", LT_K_PGUP}, {"pgdn", LT_K_PGDN}, {"click", LT_K_CLICK}, {"double-click", LT_K_DCLICK}, {"right-click", LT_K_RCLICK},
		{"wheel-up", LT_K_WHEEL_UP}, {"wheel-down", LT_K_WHEEL_DOWN}};
	int mods = 0;
	const char *p = name;
	for (;;) {
		if (!strncmp(p, "ctrl-", 5) && p[5]) { mods |= LT_MOD_CTRL; p += 5; }
		else if (!strncmp(p, "alt-", 4) && p[4]) { mods |= LT_MOD_ALT; p += 4; }
		else if (!strncmp(p, "shift-", 6) && p[6]) { mods |= LT_MOD_SHIFT; p += 6; }
		else break;
	}
	int base = -1;
	for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (!strcmp(t[i].n, p)) base = t[i].k;
	if (base < 0 && (p[0] == 'f' || p[0] == 'F') && p[1] >= '1' && p[1] <= '9') { int v = atoi(p + 1); if (v >= 1 && v <= 12) base = LT_K_F1 + v - 1; }
	if (base < 0) { uint32_t r; size_t l = lt_utf8_decode(p, &r); if (l && p[l] == 0) base = (int)r; else return -1; }
	if ((mods & LT_MOD_CTRL) && base >= 'A' && base <= 'Z') base += 32;
	if ((mods & LT_MOD_CTRL) && base == '/') base = '_';
	return base | mods;
}

void lt_key_name(int key, char *out, size_t n)
{
	static const struct { int k; const char *n; } t[] = {
		{LT_K_ENTER, "enter"}, {LT_K_TAB, "tab"}, {LT_K_BTAB, "btab"}, {LT_K_ESC, "esc"}, {LT_K_BSPACE, "bspace"}, {LT_K_UP, "up"}, {LT_K_DOWN, "down"},
		{LT_K_LEFT, "left"}, {LT_K_RIGHT, "right"}, {LT_K_HOME, "home"}, {LT_K_END, "end"}, {LT_K_PGUP, "pgup"}, {LT_K_PGDN, "pgdn"}, {LT_K_INS, "ins"},
		{LT_K_DEL, "del"}, {LT_K_CLICK, "click"}, {LT_K_DCLICK, "double-click"}, {LT_K_RCLICK, "right-click"}, {LT_K_WHEEL_UP, "wheel-up"},
		{LT_K_WHEEL_DOWN, "wheel-down"}, {' ', "space"}};
	int base = key & 0xFFFFFF, m = key & ~0xFFFFFF;
	char tmp[16];
	const char *nm = NULL;
	for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (t[i].k == base) nm = t[i].n;
	if (!nm) {
		if (base >= LT_K_F1 && base <= LT_K_F12) { snprintf(tmp, sizeof tmp, "f%d", base - LT_K_F1 + 1); nm = tmp; }
		else { size_t l = lt_utf8_encode((uint32_t)base, tmp); tmp[l] = 0; nm = tmp; }
	}
	snprintf(out, n, "%s%s%s%s", m & LT_MOD_CTRL ? "ctrl-" : "", m & LT_MOD_ALT ? "alt-" : "", m & LT_MOD_SHIFT ? "shift-" : "", nm);
}
