/* term.h - the terminal: raw mode on /dev/tty, buffered output, keys, mouse. */
#ifndef FZF_TERM_H
#define FZF_TERM_H
#include "fzf.h"

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
