#ifndef HOLYC_CURSOR_H_
#define HOLYC_CURSOR_H_

/* Cursor movement that tui.h does NOT already provide - tui_write_cursor
 * (tui.h) only writes ABSOLUTE positioning (CUP, "move to row,col"),
 * which is all tui.h's own diff/redraw loop ever needs (it always knows
 * the exact target cell). This file covers RELATIVE movement (move up/
 * down/left/right by N - the CUU/CUD/CUF/CUB sequences) and save/restore
 * (the real \033[s / \033[u DECSC/DECRC-adjacent sequences, not
 * reproduced anywhere else in this codebase) - for a caller doing
 * incremental terminal writes OUTSIDE tui.h's full-buffer render loop
 * (e.g. a simple line-based prompt that doesn't want to own a whole
 * Tui_Buffer just to move the cursor a few cells).
 *
 * Cursor_Pos is a plain logical-position tracker a caller can keep
 * updated locally (this file does not query the real terminal for the
 * cursor's actual position - there is no portable escape-sequence-based
 * way to read that back without a request/response round-trip through
 * the tty, which is a genuinely different, heavier feature than simple
 * movement, not implemented here). */

#include "base.h"

typedef struct { i32 row, col; } Cursor_Pos;

/* pure escape-sequence builders, same shape as tui_write_cursor (write
 * into caller's buf, return bytes written, no I/O, fully testable
 * without a real tty). n <= 0 is treated as n == 1 (moving by "zero" or
 * a negative count in the wrong direction isn't a valid request - CUU/
 * CUD/CUF/CUB all default to 1 when their parameter is omitted, mirrored
 * here rather than silently emitting a malformed "\033[0A"). */
i32 cursor_write_up(char *buf, i32 n);
i32 cursor_write_down(char *buf, i32 n);
i32 cursor_write_left(char *buf, i32 n);
i32 cursor_write_right(char *buf, i32 n);

/* DECSC/DECRC-style save/restore - simpler than tui_write_cursor's
 * absolute CUP for a caller that just wants "come back to where I was"
 * without tracking row/col itself. Fixed byte sequences (no parameters),
 * still exposed as buffer-writing functions rather than ansi_escape.h-
 * style constants for consistency with this file's other cursor_write_*
 * functions - a caller mixing relative moves and save/restore in the
 * same code path gets one consistent calling convention throughout. */
i32 cursor_write_save(char *buf);
i32 cursor_write_restore(char *buf);

/* updates a caller-tracked Cursor_Pos to match one of the relative moves
 * above, clamping row/col at 0 (a terminal cursor cannot go negative) -
 * for a caller that wants to keep local row/col bookkeeping in sync with
 * what it's actually sent to the terminal, without re-deriving it from
 * the escape bytes. Does not itself write anything. */
void cursor_pos_move(Cursor_Pos *pos, i32 drow, i32 dcol);

#endif /* HOLYC_CURSOR_H_ */

#ifdef CURSOR_IMPLEMENTATION
#ifndef CURSOR_IMPLEMENTATION_
#define CURSOR_IMPLEMENTATION_

static i32 cursor_write_uint_(char *buf, u32 v)
{
    i32 l = 0;
    char tmp[16];
    i32 i;
    do { tmp[l++] = (char)('0' + v % 10); v /= 10; } while (v);
    for (i = 0; i < l; i++) buf[i] = tmp[l - 1 - i];
    return l;
}

static i32 cursor_write_move_(char *buf, i32 n, char code)
{
    i32 wn = 0;
    if (n <= 0) n = 1;
    buf[wn++] = '\033'; buf[wn++] = '[';
    wn += cursor_write_uint_(buf + wn, (u32)n);
    buf[wn++] = code;
    return wn;
}

i32 cursor_write_up(char *buf, i32 n)    { return cursor_write_move_(buf, n, 'A'); }
i32 cursor_write_down(char *buf, i32 n)  { return cursor_write_move_(buf, n, 'B'); }
i32 cursor_write_right(char *buf, i32 n) { return cursor_write_move_(buf, n, 'C'); }
i32 cursor_write_left(char *buf, i32 n)  { return cursor_write_move_(buf, n, 'D'); }

i32 cursor_write_save(char *buf)
{
    buf[0] = '\033'; buf[1] = '['; buf[2] = 's';
    return 3;
}

i32 cursor_write_restore(char *buf)
{
    buf[0] = '\033'; buf[1] = '['; buf[2] = 'u';
    return 3;
}

void cursor_pos_move(Cursor_Pos *pos, i32 drow, i32 dcol)
{
    pos->row += drow;
    pos->col += dcol;
    if (pos->row < 0) pos->row = 0;
    if (pos->col < 0) pos->col = 0;
}

#endif /* CURSOR_IMPLEMENTATION_ */
#endif /* CURSOR_IMPLEMENTATION */
