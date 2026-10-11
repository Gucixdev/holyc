#ifndef HOLYC_ANSI_ESCAPE_H_
#define HOLYC_ANSI_ESCAPE_H_

/* Named ANSI/VT100 escape sequences that tui.h does NOT already emit -
 * tui.h owns cursor positioning (tui_write_cursor) and basic-16-color SGR
 * (tui_write_sgr) as part of its cell-buffer diff/redraw loop; this file
 * is everything else a terminal program typically needs once per session
 * or once per mode-switch, not once per redrawn cell: cursor visibility,
 * full-screen clear, the alternate screen buffer, bracketed paste mode,
 * and the terminal title. These are all FIXED byte sequences (no
 * parameters to compute), so unlike tui_write_cursor/color_write_sgr_rgb
 * they're exposed as plain `const char *` constants (with a paired
 * length, since they're not always safely strlen-able the instant they
 * might contain embedded structure - kept as simple NUL-terminated
 * literals here in practice, but the length constant avoids a strlen()
 * call at every use site) rather than buffer-writing functions - nothing
 * to compute, so a function call would just be indirection for its own
 * sake, the "unnecessary abstraction" the design rules explicitly warn
 * against providing a fake column for. */

#define ANSI_CURSOR_HIDE        "\033[?25l"
#define ANSI_CURSOR_HIDE_LEN    6
#define ANSI_CURSOR_SHOW        "\033[?25h"
#define ANSI_CURSOR_SHOW_LEN    6

#define ANSI_CLEAR_SCREEN       "\033[2J"
#define ANSI_CLEAR_SCREEN_LEN   4
#define ANSI_CLEAR_LINE         "\033[2K"
#define ANSI_CLEAR_LINE_LEN     4

#define ANSI_ALT_SCREEN_ENTER      "\033[?1049h"
#define ANSI_ALT_SCREEN_ENTER_LEN  8
#define ANSI_ALT_SCREEN_EXIT       "\033[?1049l"
#define ANSI_ALT_SCREEN_EXIT_LEN   8

#define ANSI_BRACKETED_PASTE_ENABLE      "\033[?2004h"
#define ANSI_BRACKETED_PASTE_ENABLE_LEN  8
#define ANSI_BRACKETED_PASTE_DISABLE     "\033[?2004l"
#define ANSI_BRACKETED_PASTE_DISABLE_LEN 8

#include "base.h"

/* the one sequence here that DOES need a parameter (the title text
 * itself) - OSC 0 (icon name + window title), terminated by BEL (\007),
 * the widely-supported form (ST, \033\\, is the "more correct" xterm
 * terminator but BEL works on far more terminals in practice, same
 * pragmatic real-world-compatibility call this project already makes
 * elsewhere, e.g. print.h choosing write() over a "purer" abstraction).
 * Writes into caller's buf (>= strlen(title) + 5 bytes), returns bytes
 * written, no I/O - same pure-builder shape as tui_write_cursor. */
i32 ansi_write_set_title(char *buf, const char *title);

#endif /* HOLYC_ANSI_ESCAPE_H_ */

#ifdef ANSI_ESCAPE_IMPLEMENTATION
#ifndef ANSI_ESCAPE_IMPLEMENTATION_
#define ANSI_ESCAPE_IMPLEMENTATION_

i32 ansi_write_set_title(char *buf, const char *title)
{
    i32 n = 0;
    buf[n++] = '\033'; buf[n++] = ']'; buf[n++] = '0'; buf[n++] = ';';
    while (*title) buf[n++] = *title++;
    buf[n++] = '\007';
    return n;
}

#endif /* ANSI_ESCAPE_IMPLEMENTATION_ */
#endif /* ANSI_ESCAPE_IMPLEMENTATION */
