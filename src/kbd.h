/*
 * kbd.h - keyboard input through the kitty keyboard protocol, only to know which modifiers are HELD.
 *
 * A terminal never reports "Ctrl went down" in legacy mode. With the protocol enabled (flags 1|2|4|8|16:
 * disambiguate, event types, alternate keys, all keys as escape codes, associated text) every key press,
 * release and every modifier key arrives as a CSI sequence. This file decodes them and hands nnn the same
 * values curses' get_wch()/getmouse() would have, so the rest of nnn is unchanged:
 *
 *   - kb_init() asks the terminal (CSI ? u); no answer within 150 ms = not supported = everything below
 *     falls straight through to curses (legacy behaviour, the hint bar just never shows modifiers).
 *   - exitcurses() pops the protocol (kb_off) before a child program (editor, shell) gets the terminal;
 *     the next read pushes it again.
 *   - NNN_KITTY=0 disables it.
 *
 * Included after the curses headers; it overrides get_wch, unget_wch, timeout, getmouse and ungetmouse.
 */
#ifndef KBD_H
#define KBD_H

#include <errno.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <string.h>
#include <unistd.h>

#define HM_CTRL  1
#define HM_ALT   2
#define HM_SHIFT 4

#define KB_FLAGS "31" /* 1 disambiguate | 2 event types | 4 alternate keys | 8 all keys | 16 text */

static unsigned char hint_mods;      /* modifiers currently held (HM_*) */
static bool kb_on, kb_pushed;        /* protocol negotiated / flags pushed in the terminal right now */
static int kb_timeout = -1;          /* mirrors curses timeout(): -1 block, 0 poll, >0 ms */

static void draw_hints(void);

/* ---- decoded events waiting to be handed out ---- */
struct kb_ev { wint_t ch; int type; };
static struct kb_ev kb_q[16];
static int kb_qn;
static MEVENT kb_mev;
static bool kb_mev_has;

static void kb_put(wint_t ch, int type)
{
	if (kb_qn < (int)(sizeof(kb_q) / sizeof(kb_q[0])))
		kb_q[kb_qn++] = (struct kb_ev){ch, type};
}

/* ---- raw byte input ---- */
static unsigned char kb_buf[512];
static int kb_blen, kb_bpos;

/* one byte, waiting up to ms (-1 forever). Returns byte, -1 timeout, -2 interrupted by a signal */
static int kb_byte(int ms)
{
	if (kb_bpos < kb_blen)
		return kb_buf[kb_bpos++];

	fd_set fds;
	struct timeval tv, *ptv = NULL;

	FD_ZERO(&fds);
	FD_SET(0, &fds);
	if (ms >= 0) {
		tv.tv_sec = ms / 1000;
		tv.tv_usec = (ms % 1000) * 1000;
		ptv = &tv;
	}
	int r = select(1, &fds, NULL, NULL, ptv);

	if (r < 0)
		return errno == EINTR ? -2 : -1;
	if (r == 0)
		return -1;
	ssize_t n = read(0, kb_buf, sizeof(kb_buf));

	if (n <= 0)
		return -1;
	kb_blen = (int)n;
	kb_bpos = 1;
	return kb_buf[0];
}

/* ---- terminal setup ---- */
static void kb_push(void)
{
	if (kb_on && !kb_pushed) {
		(void)!write(1, "\033[>" KB_FLAGS "u", 6);
		kb_pushed = TRUE;
	}
}

static void kb_off(void)
{
	if (kb_pushed) {
		(void)!write(1, "\033[<u", 4);
		kb_pushed = FALSE;
	}
	hint_mods = 0;
}

static void kb_sig(int sig)
{
	(void)!write(1, "\033[<u", 4);
	signal(sig, SIG_DFL);
	raise(sig);
}

static void kb_init(void)
{
	const char *e = getenv("NNN_KITTY");

	if ((e && e[0] == '0') || !isatty(0) || !isatty(1))
		return;

	/* CSI ? u = query the protocol flags; CSI c = "primary attributes", answered by every terminal */
	(void)!write(1, "\033[?u\033[c", 7);

	int c, seq = 0, got = 0;

	while ((c = kb_byte(150)) >= 0) {
		if (c == 033) { seq = 1; continue; }
		if (seq == 1) { seq = (c == '[') ? 2 : 0; continue; }
		if (seq == 2 && c >= 0x40 && c <= 0x7e) {
			if (c == 'u')
				got = 1;
			seq = 0;
			if (c == 'c') /* DA1 reply is last: the terminal has answered everything it will */
				break;
		}
	}
	kb_blen = kb_bpos = 0;
	if (!got)
		return;

	kb_on = TRUE;
	kb_push();
	struct sigaction sa = {.sa_handler = kb_sig};

	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	atexit(kb_off);
}

/* ---- decoding ---- */
#define KB_MAXP 8
struct kb_csi {
	int v[4][KB_MAXP]; /* v[field][sub], ';' separates fields, ':' sub-fields; 0 = absent */
	int nf, ns[4];
	char lead;         /* '<' for SGR mouse */
};

static int kb_read_csi(struct kb_csi *p)
{
	int c;

	memset(p, 0, sizeof(*p));
	p->nf = 1;
	while ((c = kb_byte(30)) >= 0) {
		if (c >= '0' && c <= '9') {
			int *v = &p->v[p->nf - 1][p->ns[p->nf - 1]];

			*v = *v * 10 + (c - '0');
		} else if (c == ':') {
			if (p->ns[p->nf - 1] < KB_MAXP - 1)
				++p->ns[p->nf - 1];
		} else if (c == ';') {
			if (p->nf < 4)
				++p->nf;
		} else if (c == '<' || c == '?' || c == '>' || c == '=') {
			p->lead = (char)c;
		} else if (c >= 0x40 && c <= 0x7e) {
			return c; /* final byte */
		}
	}
	return -1;
}

static unsigned char kb_map_mods(int m) /* protocol modifier value -> HM_* */
{
	m = m > 0 ? m - 1 : 0;
	return (unsigned char)(((m & 1) ? HM_SHIFT : 0) | ((m & 2) ? HM_ALT : 0) | ((m & 4) ? HM_CTRL : 0));
}

static void kb_set_mods(unsigned char m)
{
	if (m == hint_mods)
		return;
	hint_mods = m;

	int y, x;

	getyx(stdscr, y, x);
	draw_hints();
	move(y, x);
	refresh();
}

static wint_t kb_ctrl(wint_t k)
{
	if (k >= 'a' && k <= 'z')
		return k & 0x1f;
	if (k >= 'A' && k <= 'Z')
		return k & 0x1f;
	switch (k) {
	case ' ': case '@': return 0;
	case '[': return 27;
	case '\\': return 28;
	case ']': return 29;
	case '^': return 30;
	case '_': case '-': case '/': return 31;
	case '?': return 127;
	}
	return k;
}

static void kb_key(int key, int shifted, int mods, int event, const int *text, int ntext)
{
	if (event == 3)
		return; /* release */

	int alt = mods & HM_ALT, ctrl = mods & HM_CTRL, shift = mods & HM_SHIFT;
	wint_t c;
	int type = OK;

	switch (key) {
	case 13: c = '\r'; break;
	case 9:
		if (shift) { c = KEY_BTAB; type = KEY_CODE_YES; }
		else c = '\t';
		break;
	case 27: c = 27; break;
	case 127: c = KEY_BACKSPACE; type = KEY_CODE_YES; break;
	default:
		if (ntext > 0 && !ctrl && !alt) {
			for (int i = 0; i < ntext; ++i)
				kb_put((wint_t)text[i], OK);
			return;
		}
		if (ctrl)
			c = kb_ctrl((wint_t)key);
		else if (shift && shifted)
			c = (wint_t)shifted;
		else if (shift && key >= 'a' && key <= 'z')
			c = (wint_t)(key - 32);
		else
			c = (wint_t)key;
	}

	if (alt)
		kb_put(27, OK); /* legacy Alt+key = ESC then the key, which is what nnn's Alt handling expects */
	kb_put(c, type);
}

static void kb_mouse(const struct kb_csi *p, int final)
{
	int b = p->v[0][0], x = p->v[1][0], y = p->v[2][0];

	if (final != 'M' || (b & 32)) /* release / motion */
		return;

	mmask_t st = 0; /* nnn sets mouseinterval(0): every press is a plain BUTTONn_PRESSED */

	switch (b & 0x43) {
	case 0: st = BUTTON1_PRESSED; break;
	case 1: st = BUTTON2_PRESSED; break;
	case 2: st = BUTTON3_PRESSED; break;
#ifdef BUTTON4_PRESSED
	case 64: st = BUTTON4_PRESSED; break;
#endif
#ifdef BUTTON5_PRESSED
	case 65: st = BUTTON5_PRESSED; break;
#endif
	default: return;
	}

	kb_mev = (MEVENT){.id = 0, .x = x - 1, .y = y - 1, .z = 0, .bstate = st};
	kb_mev_has = TRUE;
	kb_put(KEY_MOUSE, KEY_CODE_YES);
}

/* decode one CSI sequence (ESC [ already consumed) */
static void kb_csi(void)
{
	struct kb_csi p;
	int f = kb_read_csi(&p);

	if (f < 0)
		return;

	if (p.lead == '<') {
		kb_mouse(&p, f);
		return;
	}
	if (p.lead)
		return; /* an answer to some query: not a key */

	unsigned char mods = kb_map_mods(p.v[1][0]);
	int event = p.v[1][1] ? p.v[1][1] : 1;
	int key = p.v[0][0];

	if (f == 'u') {
		/* modifier / lock keys only change the held state */
		if ((key >= 57441 && key <= 57454) || (key >= 57358 && key <= 57360)) {
			kb_set_mods(event == 3 ? mods : mods);
			return;
		}
		kb_set_mods(mods);
		kb_key(key, p.v[0][1], mods, event, p.v[2], p.ns[2] + (p.v[2][0] ? 1 : 0));
		return;
	}

	kb_set_mods(mods);
	if (event == 3)
		return;

	wint_t k = 0;

	switch (f) {
	case 'A': k = KEY_UP; break;
	case 'B': k = KEY_DOWN; break;
	case 'C': k = KEY_RIGHT; break;
	case 'D': k = KEY_LEFT; break;
	case 'H': k = KEY_HOME; break;
	case 'F': k = KEY_END; break;
	case 'P': k = KEY_F(1); break;
	case 'Q': k = KEY_F(2); break;
	case 'S': k = KEY_F(4); break;
	case 'Z': k = KEY_BTAB; break;
	case '~':
		switch (key) {
		case 1: case 7: k = KEY_HOME; break;
		case 4: case 8: k = KEY_END; break;
		case 2: k = KEY_IC; break;
		case 3: k = KEY_DC; break;
		case 5: k = KEY_PPAGE; break;
		case 6: k = KEY_NPAGE; break;
		case 13: k = KEY_F(3); break;
		case 15: k = KEY_F(5); break;
		case 17: k = KEY_F(6); break;
		case 18: k = KEY_F(7); break;
		case 19: k = KEY_F(8); break;
		case 20: k = KEY_F(9); break;
		case 21: k = KEY_F(10); break;
		case 23: k = KEY_F(11); break;
		case 24: k = KEY_F(12); break;
		}
		break;
	}
	if (k)
		kb_put(k, KEY_CODE_YES);
}

/* Fill the queue; returns FALSE on timeout. */
static bool kb_fill(int ms)
{
	while (kb_qn == 0) {
		int c = kb_byte(ms);

		if (c == -2) { /* signal: most likely SIGWINCH */
			struct winsize ws;

			if (ioctl(1, TIOCGWINSZ, &ws) == 0 && (ws.ws_row != LINES || ws.ws_col != COLS)) {
				resizeterm(ws.ws_row, ws.ws_col);
				kb_put(KEY_RESIZE, KEY_CODE_YES);
			}
			continue;
		}
		if (c < 0)
			return FALSE;
		if (c == 033) {
			int n = kb_byte(20);

			if (n == '[')
				kb_csi();
			else if (n == 'O') { /* SS3: F1-F4 / keypad in legacy form */
				int s = kb_byte(20);

				switch (s) {
				case 'P': kb_put(KEY_F(1), KEY_CODE_YES); break;
				case 'Q': kb_put(KEY_F(2), KEY_CODE_YES); break;
				case 'R': kb_put(KEY_F(3), KEY_CODE_YES); break;
				case 'S': kb_put(KEY_F(4), KEY_CODE_YES); break;
				}
			} else {
				kb_put(27, OK);
				if (n >= 0)
					kb_buf[--kb_bpos] = (unsigned char)n; /* not part of an escape: read it again */
			}
			continue;
		}
		if (c >= 0x80) { /* UTF-8 (not expected with flag 8, but stay safe) */
			int len = c >= 0xf0 ? 3 : (c >= 0xe0 ? 2 : 1);
			wint_t w = (wint_t)(c & (0x3f >> len));

			for (int i = 0; i < len; ++i) {
				int n = kb_byte(20);

				if (n < 0)
					break;
				w = (w << 6) | (wint_t)(n & 0x3f);
			}
			kb_put(w, OK);
			continue;
		}
		kb_put((wint_t)c, OK);
	}
	return TRUE;
}

/* ---- curses replacements ---- */
static int kb_get_wch(wint_t *ch)
{
	if (!kb_on)
		return get_wch(ch);

	kb_push();
	refresh(); /* curses' own get_wch() flushes the screen before it waits; we bypass it */
	if (!kb_fill(kb_timeout))
		return ERR;

	*ch = kb_q[0].ch;
	int t = kb_q[0].type;

	memmove(kb_q, kb_q + 1, (size_t)(--kb_qn) * sizeof(kb_q[0]));
	return t;
}

static int kb_unget_wch(wchar_t c)
{
	if (!kb_on)
		return unget_wch(c);
	if (kb_qn >= (int)(sizeof(kb_q) / sizeof(kb_q[0])))
		return ERR;
	memmove(kb_q + 1, kb_q, (size_t)kb_qn * sizeof(kb_q[0]));
	kb_q[0] = (struct kb_ev){(wint_t)c, c >= KEY_MIN ? KEY_CODE_YES : OK};
	++kb_qn;
	return OK;
}

static int kb_getmouse(MEVENT *e)
{
	if (!kb_on)
		return getmouse(e);
	if (!kb_mev_has)
		return ERR;
	*e = kb_mev;
	kb_mev_has = FALSE;
	return OK;
}

static int kb_ungetmouse(MEVENT *e)
{
	if (!kb_on)
		return ungetmouse(e);
	kb_mev = *e;
	kb_mev_has = TRUE;
	return OK;
}

#undef get_wch
#undef timeout
#define get_wch(p)     kb_get_wch(p)
#define unget_wch(c)   kb_unget_wch(c)
#define getmouse(e)    kb_getmouse(e)
#define ungetmouse(e)  kb_ungetmouse(e)
#define timeout(t)     (kb_timeout = (t), wtimeout(stdscr, (t)))

#endif /* KBD_H */
