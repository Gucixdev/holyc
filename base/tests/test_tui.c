#include "test_common.h"
#define TUI_IMPLEMENTATION
#include "tui.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void test_utf8_roundtrip_(u32 cp, const char *label)
{
    char buf[8];
    u32 decoded;
    i32 enc_len, dec_len;
    char msg[96];

    enc_len = tui_utf8_unicode_to_char(buf, cp);
    buf[enc_len] = '\0';
    dec_len = tui_utf8_char_to_unicode(&decoded, buf);

    sprintf(msg, "utf8 roundtrip: %s encode/decode length matches", label);
    TEST_CHECK(enc_len == dec_len, msg);
    sprintf(msg, "utf8 roundtrip: %s decodes back to the same codepoint", label);
    TEST_CHECK(decoded == cp, msg);
    sprintf(msg, "utf8 roundtrip: %s tui_utf8_char_length agrees with decode length", label);
    TEST_CHECK(tui_utf8_char_length(buf[0]) == enc_len, msg);
}

int main(void)
{
    Tui_Buffer front, back;

    print_cstr("test_tui\n");

    /* utf8 codec: ASCII, 2-byte, 3-byte, 4-byte codepoints */
    test_utf8_roundtrip_(0x41, "ASCII 'A'");
    test_utf8_roundtrip_(0x00F3, "2-byte (o with acute)");
    test_utf8_roundtrip_(0x4E2D, "3-byte (CJK)");
    test_utf8_roundtrip_(0x1F600, "4-byte (emoji)");

    /* buffer lifecycle */
    tui_buffer_init(&back, 10, 5);
    TEST_CHECK(back.width == 10 && back.height == 5, "buffer_init: dimensions stored");

    tui_buffer_clear(&back, TUI_WHITE, TUI_BLACK);
    TEST_CHECK(back.cells[0].ch == ' ', "buffer_clear: cells filled with blanks");
    TEST_CHECK(back.cells[0].fg == TUI_WHITE && back.cells[0].bg == TUI_BLACK, "buffer_clear: colors applied");

    tui_buffer_set_cell(&back, 3, 2, 'X', TUI_RED, TUI_DEFAULT);
    TEST_CHECK(back.cells[2 * 10 + 3].ch == 'X', "set_cell: writes at the correct row-major offset");
    tui_buffer_set_cell(&back, -1, 0, 'Y', 0, 0);
    tui_buffer_set_cell(&back, 100, 0, 'Y', 0, 0);
    TEST_CHECK(1, "set_cell: out-of-bounds coordinates are silently ignored, not a crash");

    {
        Tui_Cell before = back.cells[2 * 10 + 3];
        tui_buffer_resize(&back, 20, 3);
        TEST_CHECK(back.width == 20 && back.height == 3, "buffer_resize: new dimensions applied");
        TEST_CHECK(back.cells[2 * 20 + 3].ch == before.ch, "buffer_resize: overlapping region preserved");
    }
    tui_buffer_free(&back);
    TEST_CHECK(back.cells == NULL, "buffer_free: cells pointer cleared");

    /* escape sequence builders - pure, no tty needed */
    {
        char buf[32];
        i32 n;

        n = tui_write_cursor(buf, 0, 0);
        buf[n] = '\0';
        TEST_CHECK(strcmp(buf, "\033[1;1H") == 0, "write_cursor: (0,0) is 1-indexed in the escape sequence");

        n = tui_write_cursor(buf, 9, 4);
        buf[n] = '\0';
        TEST_CHECK(strcmp(buf, "\033[5;10H") == 0, "write_cursor: arbitrary coordinates encode correctly");

        n = tui_write_sgr(buf, TUI_DEFAULT, TUI_DEFAULT);
        TEST_CHECK(n == 0, "write_sgr: default/default produces no escape sequence");

        n = tui_write_sgr(buf, TUI_RED, TUI_DEFAULT);
        buf[n] = '\0';
        TEST_CHECK(strcmp(buf, "\033[31m") == 0, "write_sgr: foreground-only color code");

        n = tui_write_sgr(buf, TUI_RED, TUI_BLUE);
        buf[n] = '\0';
        TEST_CHECK(strcmp(buf, "\033[31;44m") == 0, "write_sgr: combined foreground+background");
    }

    /* present(): diff-based redraw - the actual point of this module */
    {
        char out[4096];
        i32 n;

        tui_buffer_init(&front, 10, 5);
        tui_buffer_clear(&front, TUI_DEFAULT, TUI_DEFAULT);
        tui_buffer_init(&back, 10, 5);
        tui_buffer_clear(&back, TUI_DEFAULT, TUI_DEFAULT);

        n = tui_present(&front, &back, out, sizeof(out));
        TEST_CHECK(n == 0, "present: identical front/back produces zero output bytes");

        tui_buffer_set_cell(&back, 2, 1, 'H', TUI_GREEN, TUI_DEFAULT);
        n = tui_present(&front, &back, out, sizeof(out));
        TEST_CHECK(n > 0, "present: a single changed cell produces non-empty output");
        TEST_CHECK(front.cells[1 * 10 + 2].ch == 'H', "present: front buffer updated to match back after presenting");

        n = tui_present(&front, &back, out, sizeof(out));
        TEST_CHECK(n == 0, "present: re-presenting the same state produces zero output (front now matches back)");

        /* undersized output buffer must fail cleanly, not overflow */
        tui_buffer_set_cell(&back, 5, 3, 'Z', TUI_YELLOW, TUI_BLUE);
        n = tui_present(&front, &back, out, 1);
        TEST_CHECK(n == TUI_EOF, "present: output buffer too small returns TUI_EOF, not a partial/garbled write");

        tui_buffer_free(&front);
        tui_buffer_free(&back);
    }

    /* raw mode / term size: best-effort against a real tty, skipped (not
     * failed) if none is available in this environment - these are thin
     * wrappers around tcgetattr/tcsetattr/ioctl(TIOCGWINSZ), the actual
     * risk in this module is leaving a REAL terminal in raw mode, which
     * is exactly why enable always pairs with disable in the same block
     * here regardless of whether a tty was found. */
    {
        i32 fd = open("/dev/tty", O_RDWR);
        if (fd >= 0) {
            Tui_Term_State saved;
            i32 w = -1, h = -1;
            i32 rc = tui_raw_mode_enable(fd, &saved);
            TEST_CHECK(rc == 0, "raw_mode_enable: succeeds against a real tty");
            if (rc == 0) {
                TEST_CHECK(tui_raw_mode_disable(fd, &saved) == 0, "raw_mode_disable: restores original state");
            }
            TEST_CHECK(tui_get_term_size(fd, &w, &h) == 0, "get_term_size: succeeds against a real tty");
            TEST_CHECK(w > 0 && h > 0, "get_term_size: returns positive dimensions");
            close(fd);
        } else {
            print_cstr("  SKIP: no /dev/tty available in this environment - raw_mode/get_term_size untested\n");
        }
    }

    print_flush();
    return test_failures_ != 0;
}
