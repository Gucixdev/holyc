#include "test_common.h"
#define ANSI_ESCAPE_IMPLEMENTATION
#include "ansi_escape.h"

#include <string.h>

int main(void)
{
    print_cstr("test_ansi_escape\n");

    TEST_CHECK(strlen(ANSI_CURSOR_HIDE) == ANSI_CURSOR_HIDE_LEN, "ANSI_CURSOR_HIDE: length constant matches actual strlen");
    TEST_CHECK(strcmp(ANSI_CURSOR_HIDE, "\033[?25l") == 0, "ANSI_CURSOR_HIDE: exact bytes");
    TEST_CHECK(strlen(ANSI_CURSOR_SHOW) == ANSI_CURSOR_SHOW_LEN, "ANSI_CURSOR_SHOW: length constant matches actual strlen");
    TEST_CHECK(strcmp(ANSI_CURSOR_SHOW, "\033[?25h") == 0, "ANSI_CURSOR_SHOW: exact bytes");

    TEST_CHECK(strlen(ANSI_CLEAR_SCREEN) == ANSI_CLEAR_SCREEN_LEN, "ANSI_CLEAR_SCREEN: length constant matches actual strlen");
    TEST_CHECK(strcmp(ANSI_CLEAR_SCREEN, "\033[2J") == 0, "ANSI_CLEAR_SCREEN: exact bytes");
    TEST_CHECK(strlen(ANSI_CLEAR_LINE) == ANSI_CLEAR_LINE_LEN, "ANSI_CLEAR_LINE: length constant matches actual strlen");
    TEST_CHECK(strcmp(ANSI_CLEAR_LINE, "\033[2K") == 0, "ANSI_CLEAR_LINE: exact bytes");

    TEST_CHECK(strlen(ANSI_ALT_SCREEN_ENTER) == ANSI_ALT_SCREEN_ENTER_LEN, "ANSI_ALT_SCREEN_ENTER: length constant matches actual strlen");
    TEST_CHECK(strcmp(ANSI_ALT_SCREEN_ENTER, "\033[?1049h") == 0, "ANSI_ALT_SCREEN_ENTER: exact bytes");
    TEST_CHECK(strlen(ANSI_ALT_SCREEN_EXIT) == ANSI_ALT_SCREEN_EXIT_LEN, "ANSI_ALT_SCREEN_EXIT: length constant matches actual strlen");
    TEST_CHECK(strcmp(ANSI_ALT_SCREEN_EXIT, "\033[?1049l") == 0, "ANSI_ALT_SCREEN_EXIT: exact bytes");

    TEST_CHECK(strlen(ANSI_BRACKETED_PASTE_ENABLE) == ANSI_BRACKETED_PASTE_ENABLE_LEN, "ANSI_BRACKETED_PASTE_ENABLE: length constant matches actual strlen");
    TEST_CHECK(strcmp(ANSI_BRACKETED_PASTE_ENABLE, "\033[?2004h") == 0, "ANSI_BRACKETED_PASTE_ENABLE: exact bytes");
    TEST_CHECK(strlen(ANSI_BRACKETED_PASTE_DISABLE) == ANSI_BRACKETED_PASTE_DISABLE_LEN, "ANSI_BRACKETED_PASTE_DISABLE: length constant matches actual strlen");
    TEST_CHECK(strcmp(ANSI_BRACKETED_PASTE_DISABLE, "\033[?2004l") == 0, "ANSI_BRACKETED_PASTE_DISABLE: exact bytes");

    {
        char buf[64];
        i32 n = ansi_write_set_title(buf, "hello");
        buf[n] = '\0';
        TEST_CHECK(strcmp(buf, "\033]0;hello\007") == 0, "write_set_title: exact bytes including OSC prefix and BEL terminator");
        TEST_CHECK(n == (i32)strlen("\033]0;hello\007"), "write_set_title: returned length matches actual bytes written");
    }

    {
        char buf[8];
        i32 n = ansi_write_set_title(buf, "");
        buf[n] = '\0';
        TEST_CHECK(strcmp(buf, "\033]0;\007") == 0, "write_set_title: empty title still produces a valid, minimal sequence");
    }

    print_flush();
    return test_failures_ != 0;
}
