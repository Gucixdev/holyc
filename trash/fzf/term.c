/* term.c - raw terminal, key and mouse decoding (xterm SGR mouse, bracketed paste, CSI/SS3 keys, alt-prefix). */
#include "term.h"
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
