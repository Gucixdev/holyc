#ifndef HOLYC_TUI_H_
#define HOLYC_TUI_H_

/* Terminal UI, a scoped-down port from nsf/termbox (MIT, github.com/termbox/
 * termbox, ~1730 lines across termbox.c/.h/utf8.c). Full termbox is
 * substantially bigger than what's here - deliberately NOT ported in full
 * this pass, see the list below. What IS here is real and useful: cell
 * buffer with double-buffering + diff-based redraw (only changed cells get
 * written, termbox's own headline technique, faithfully kept), UTF-8
 * codec, raw terminal mode, and ANSI escape-sequence generation.
 *
 * NOT ported (left for a future pass, each for a specific reason):
 *   - terminfo file parsing (load_terminfo/terminfo_copy_string and the six
 *     built-in per-terminal-family escape-sequence tables in the original
 *     termbox.c) - this project targets Linux/POSIX only already (mmap,
 *     pthread used elsewhere without portability shims), so hardcoded
 *     ANSI/xterm-256color escapes cover the realistic case without
 *     needing a terminfo binary-format parser.
 *   - input event parsing (tb_poll_event/tb_peek_event, parse_escape_seq,
 *     parse_mouse_event, the terminal-family key tables) - by far the
 *     most complex and terminal-quirk-sensitive part of the original;
 *     deferred rather than rushed, since a subtly wrong raw-mode/input
 *     state machine can leave a user's actual shell session broken in a
 *     way a GUI logic bug cannot.
 *   - SIGWINCH-driven resize notification - straightforward to add later
 *     (a signal handler setting a volatile flag, exactly as the original
 *     does), just not wired up yet since there's no event loop to react
 *     to it either.
 *   - wide-character (CJK double-width) cell width via wcwidth() - every
 *     codepoint is treated as width 1 here. A real fix needs a wcwidth
 *     table, not just the libc call (locale-dependent, and this project
 *     avoids locale dependencies elsewhere too) - noted, not solved.
 *
 * Raw-mode state (`Tui_Term_State`) is a direct `struct termios` typedef,
 * not wrapped further - <termios.h> is a standard POSIX header, and this
 * project already assumes POSIX (mmap, pthread) without portability
 * shims, so there's nothing to gain from hiding it behind an opaque type.
 *
 * Buffers use malloc/realloc/free directly, not Arena: a terminal resize
 * needs to preserve the overlapping top-left region of the OLD buffer
 * inside a differently-sized new allocation, which is a realloc-shaped
 * operation Arena's bump allocator has no equivalent for (and repeated
 * resizes would otherwise leak arena memory every time, since nothing
 * about a terminal resize maps to Arena's rewind/snapshot model). Same
 * class of exception ivf_coarse.h and various bench harnesses already
 * make for genuinely realloc-shaped data. */

#include "base.h"

/* termios.h/sys/ioctl.h belong here directly, not a shared "platform"
 * wrapper - this is the ANSII/ (terminal) domain's own file, already
 * the sole consumer of raw-mode/terminal-size ioctls in this codebase.
 * stdlib.h stays direct since it's this file's own need, nobody else's. */
#include <termios.h>
#include <sys/ioctl.h>
#include <stdlib.h>

/* own local memcpy_/memset_ - own copy per this project's standing "no
 * cross-IMPLEMENTATION-guard dependency" rule, avoiding a <string.h>
 * dependency for two small byte loops (this file's cell-buffer copies
 * are terminal-size-bounded, not a hot path worth libc's SIMD memcpy
 * the way arena_alloc.h's arena_memcpy documents itself as needing). */
static void *tui_memcpy_(void *dst, const void *src, size_t n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    size_t i;
    for (i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

static void *tui_memset_(void *dst, i32 val, size_t n)
{
    u8 *d = (u8 *)dst;
    size_t i;
    for (i = 0; i < n; i++) d[i] = (u8)val;
    return dst;
}

#define TUI_DEFAULT 0x00
#define TUI_BLACK   0x01
#define TUI_RED     0x02
#define TUI_GREEN   0x03
#define TUI_YELLOW  0x04
#define TUI_BLUE    0x05
#define TUI_MAGENTA 0x06
#define TUI_CYAN    0x07
#define TUI_WHITE   0x08

#define TUI_BOLD      0x0100
#define TUI_UNDERLINE 0x0200
#define TUI_REVERSE   0x0400

#define TUI_EOF -1

typedef struct {
    u32 ch; /* unicode codepoint, 0 == blank/untouched */
    u16 fg;
    u16 bg;
} Tui_Cell;

typedef struct {
    Tui_Cell *cells;
    i32 width;
    i32 height;
} Tui_Buffer;

typedef struct termios Tui_Term_State;

i32 tui_utf8_char_length(char c);
i32 tui_utf8_char_to_unicode(u32 *out, const char *c);
i32 tui_utf8_unicode_to_char(char *out, u32 c);

void tui_buffer_init(Tui_Buffer *buf, i32 width, i32 height);
void tui_buffer_resize(Tui_Buffer *buf, i32 width, i32 height);
void tui_buffer_clear(Tui_Buffer *buf, u16 fg, u16 bg);
void tui_buffer_free(Tui_Buffer *buf);
void tui_buffer_set_cell(Tui_Buffer *buf, i32 x, i32 y, u32 ch, u16 fg, u16 bg);

/* fd must refer to an open tty. Returns 0 on success, -1 on failure (errno
 * set by the underlying tcgetattr/tcsetattr call). *out_saved receives the
 * pre-raw-mode termios state - pass it to tui_raw_mode_disable to restore
 * the user's shell to a sane state; losing this on a crash path is exactly
 * the failure mode that makes rushing this module not worth it. */
i32 tui_raw_mode_enable(i32 fd, Tui_Term_State *out_saved);
i32 tui_raw_mode_disable(i32 fd, const Tui_Term_State *saved);
i32 tui_get_term_size(i32 fd, i32 *out_w, i32 *out_h);

/* pure escape-sequence builders - write into caller's buf (>= 32 bytes),
 * return bytes written, no I/O. Testable without a real tty. */
i32 tui_write_cursor(char *buf, i32 x, i32 y);
i32 tui_write_sgr(char *buf, u16 fg, u16 bg);

/* diffs front against back cell-by-cell; for every differing cell, appends
 * a cursor-position + SGR + UTF-8-char escape sequence to out_buf (caller-
 * owned, out_cap bytes) and copies that cell from back into front. Returns
 * bytes written (0 if front == back already). Caller is responsible for
 * actually writing the result to the terminal fd - kept separate so this
 * stays a pure, testable function, matching tui_write_cursor/tui_write_sgr
 * above. Returns TUI_EOF (without modifying out_buf/front) if out_cap
 * would be exceeded partway through - caller should retry with a larger
 * buffer or flush and call again from where it left off; buffers sized
 * for a full terminal repaint in one call are the common case and won't
 * hit this in practice. */
i32 tui_present(Tui_Buffer *front, const Tui_Buffer *back, char *out_buf, size_t out_cap);

#endif /* HOLYC_TUI_H_ */

#ifdef TUI_IMPLEMENTATION
#ifndef TUI_IMPLEMENTATION_
#define TUI_IMPLEMENTATION_


static const u8 tui_utf8_length_[256] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
    4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 1, 1
};

static const u8 tui_utf8_mask_[6] = { 0x7F, 0x1F, 0x0F, 0x07, 0x03, 0x01 };

i32 tui_utf8_char_length(char c)
{
    return tui_utf8_length_[(u8)c];
}

i32 tui_utf8_char_to_unicode(u32 *out, const char *c)
{
    u32 result;
    u8 len, mask;
    i32 i;

    *out = 0; /* defined even on the early-return path below - callers
               * who read *out without checking the return value first
               * get 0, not an uninitialized read */
    if (*c == 0) return TUI_EOF;

    len = tui_utf8_length_[(u8)*c];
    mask = tui_utf8_mask_[len - 1];
    result = (u32)(c[0] & mask);
    for (i = 1; i < len; i++) {
        result <<= 6;
        result |= (u32)(c[i] & 0x3f);
    }
    *out = result;
    return (i32)len;
}

static const i32 tui_utf8_enc_first_[6] = { 0x00, 0xc0, 0xe0, 0xf0, 0xf8, 0xfc };

i32 tui_utf8_unicode_to_char(char *out, u32 c)
{
    i32 len, first, i;

    /* number of bytes needed is 1 plus how many of these six thresholds c
     * clears - equivalent to the original if/else-if range chain, just
     * expressed as a sum instead of a cascade */
    len = 1 + (c >= 0x80) + (c >= 0x800) + (c >= 0x10000) + (c >= 0x200000) + (c >= 0x4000000);
    first = tui_utf8_enc_first_[len - 1];

    for (i = len - 1; i > 0; i--) {
        out[i] = (char)((c & 0x3f) | 0x80);
        c >>= 6;
    }
    out[0] = (char)(c | (u32)first);

    return len;
}

void tui_buffer_init(Tui_Buffer *buf, i32 width, i32 height)
{
    buf->cells = (Tui_Cell *)malloc(sizeof(Tui_Cell) * (size_t)width * (size_t)height);
    buf->width = width;
    buf->height = height;
}

void tui_buffer_clear(Tui_Buffer *buf, u16 fg, u16 bg)
{
    i32 i, n = buf->width * buf->height;
    for (i = 0; i < n; i++) {
        buf->cells[i].ch = ' ';
        buf->cells[i].fg = fg;
        buf->cells[i].bg = bg;
    }
}

void tui_buffer_resize(Tui_Buffer *buf, i32 width, i32 height)
{
    i32 oldw, oldh, minw, minh, i;
    Tui_Cell *oldcells;

    if (buf->width == width && buf->height == height) return;

    oldw = buf->width;
    oldh = buf->height;
    oldcells = buf->cells;

    tui_buffer_init(buf, width, height);
    tui_buffer_clear(buf, TUI_DEFAULT, TUI_DEFAULT);

    minw = (width < oldw) ? width : oldw;
    minh = (height < oldh) ? height : oldh;

    for (i = 0; i < minh; i++) {
        Tui_Cell *csrc = oldcells + (i * oldw);
        Tui_Cell *cdst = buf->cells + (i * width);
        tui_memcpy_(cdst, csrc, sizeof(Tui_Cell) * (size_t)minw);
    }

    free(oldcells);
}

void tui_buffer_free(Tui_Buffer *buf)
{
    free(buf->cells);
    buf->cells = NULL;
}

void tui_buffer_set_cell(Tui_Buffer *buf, i32 x, i32 y, u32 ch, u16 fg, u16 bg)
{
    Tui_Cell *cell;
    if (x < 0 || x >= buf->width || y < 0 || y >= buf->height) return;
    cell = &buf->cells[y * buf->width + x];
    cell->ch = ch;
    cell->fg = fg;
    cell->bg = bg;
}

i32 tui_raw_mode_enable(i32 fd, Tui_Term_State *out_saved)
{
    struct termios tios;

    if (tcgetattr(fd, out_saved) != 0) return -1;

    tios = *out_saved;
    tios.c_iflag &= (u32)~(ICRNL | IXON);
    tios.c_lflag &= (u32)~(ICANON | ECHO | IEXTEN);
    tios.c_cc[VMIN] = 1;
    tios.c_cc[VTIME] = 0;

    return tcsetattr(fd, TCSAFLUSH, &tios);
}

i32 tui_raw_mode_disable(i32 fd, const Tui_Term_State *saved)
{
    return tcsetattr(fd, TCSAFLUSH, saved);
}

i32 tui_get_term_size(i32 fd, i32 *out_w, i32 *out_h)
{
    struct winsize sz;
    tui_memset_(&sz, 0, sizeof(sz));
    if (ioctl(fd, TIOCGWINSZ, &sz) != 0) return -1;
    *out_w = sz.ws_col > 0 ? sz.ws_col : 80;
    *out_h = sz.ws_row > 0 ? sz.ws_row : 24;
    return 0;
}

static i32 tui_write_uint_(char *buf, u32 v)
{
    i32 l = 0;
    char tmp[16];
    i32 i;

    /* do-while instead of while + a v==0 special case: the loop body must
     * run at least once so v==0 still emits a single '0' digit */
    do { tmp[l++] = (char)('0' + v % 10); v /= 10; } while (v);
    for (i = 0; i < l; i++) buf[i] = tmp[l - 1 - i];
    return l;
}

i32 tui_write_cursor(char *buf, i32 x, i32 y)
{
    i32 n = 0;
    buf[n++] = '\033'; buf[n++] = '[';
    n += tui_write_uint_(buf + n, (u32)(y + 1));
    buf[n++] = ';';
    n += tui_write_uint_(buf + n, (u32)(x + 1));
    buf[n++] = 'H';
    return n;
}

i32 tui_write_sgr(char *buf, u16 fg, u16 bg)
{
    i32 n = 0;
    u16 fg_col = fg & 0x0F;
    u16 bg_col = bg & 0x0F;

    if (fg == TUI_DEFAULT && bg == TUI_DEFAULT) return 0;

    buf[n++] = '\033'; buf[n++] = '[';
    if (fg != TUI_DEFAULT) {
        buf[n++] = '3';
        n += tui_write_uint_(buf + n, fg_col - 1u);
        if (bg != TUI_DEFAULT) buf[n++] = ';';
    }
    if (bg != TUI_DEFAULT) {
        buf[n++] = '4';
        n += tui_write_uint_(buf + n, bg_col - 1u);
    }
    buf[n++] = 'm';
    return n;
}

i32 tui_present(Tui_Buffer *front, const Tui_Buffer *back, char *out_buf, size_t out_cap)
{
    i32 x, y;
    size_t n = 0;
    u16 last_fg = 0xFFFF, last_bg = 0xFFFF;
    i32 last_x = -2, last_y = -2;

    for (y = 0; y < back->height; y++) {
        for (x = 0; x < back->width; x++) {
            const Tui_Cell *bk = &back->cells[y * back->width + x];
            Tui_Cell *fr = &front->cells[y * front->width + x];
            char tmp[32];
            i32 tn;

            if (bk->ch == fr->ch && bk->fg == fr->fg && bk->bg == fr->bg) continue;

            if (x - 1 != last_x || y != last_y) {
                tn = tui_write_cursor(tmp, x, y);
                if (n + (size_t)tn > out_cap) return TUI_EOF;
                tui_memcpy_(out_buf + n, tmp, (size_t)tn);
                n += (size_t)tn;
            }
            if (bk->fg != last_fg || bk->bg != last_bg) {
                tn = tui_write_sgr(tmp, bk->fg, bk->bg);
                if (n + (size_t)tn > out_cap) return TUI_EOF;
                tui_memcpy_(out_buf + n, tmp, (size_t)tn);
                n += (size_t)tn;
                last_fg = bk->fg;
                last_bg = bk->bg;
            }
            tn = tui_utf8_unicode_to_char(tmp, bk->ch ? bk->ch : ' ');
            if (n + (size_t)tn > out_cap) return TUI_EOF;
            tui_memcpy_(out_buf + n, tmp, (size_t)tn);
            n += (size_t)tn;

            last_x = x;
            last_y = y;
            *fr = *bk;
        }
    }

    return (i32)n;
}

#endif /* TUI_IMPLEMENTATION_ */
#endif /* TUI_IMPLEMENTATION */
