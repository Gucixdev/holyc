#include "test_common.h"
#define CURSOR_IMPLEMENTATION
#include "cursor.h"

#include <string.h>

int main(void)
{
    print_cstr("test_cursor\n");

    {
        char buf[32];
        i32 n;

        n = cursor_write_up(buf, 3);
        buf[n] = '\0';
        TEST_CHECK(strcmp(buf, "\033[3A") == 0, "write_up: exact bytes for n=3");

        n = cursor_write_down(buf, 5);
        buf[n] = '\0';
        TEST_CHECK(strcmp(buf, "\033[5B") == 0, "write_down: exact bytes for n=5");

        n = cursor_write_right(buf, 1);
        buf[n] = '\0';
        TEST_CHECK(strcmp(buf, "\033[1C") == 0, "write_right: exact bytes for n=1");

        n = cursor_write_left(buf, 10);
        buf[n] = '\0';
        TEST_CHECK(strcmp(buf, "\033[10D") == 0, "write_left: exact bytes for n=10 (multi-digit)");
    }

    /* n <= 0 defaults to 1, matching CUU/CUD/CUF/CUB's own default */
    {
        char buf[32];
        i32 n;

        n = cursor_write_up(buf, 0);
        buf[n] = '\0';
        TEST_CHECK(strcmp(buf, "\033[1A") == 0, "write_up: n=0 defaults to 1");

        n = cursor_write_down(buf, -5);
        buf[n] = '\0';
        TEST_CHECK(strcmp(buf, "\033[1B") == 0, "write_down: negative n defaults to 1");
    }

    {
        char buf[8];
        i32 n;

        n = cursor_write_save(buf);
        buf[n] = '\0';
        TEST_CHECK(strcmp(buf, "\033[s") == 0, "write_save: exact bytes");
        TEST_CHECK(n == 3, "write_save: returns 3 bytes written");

        n = cursor_write_restore(buf);
        buf[n] = '\0';
        TEST_CHECK(strcmp(buf, "\033[u") == 0, "write_restore: exact bytes");
    }

    /* Cursor_Pos tracking */
    {
        Cursor_Pos pos;
        pos.row = 5;
        pos.col = 5;

        cursor_pos_move(&pos, -2, 3);
        TEST_CHECK(pos.row == 3 && pos.col == 8, "pos_move: row decreases, col increases correctly");

        cursor_pos_move(&pos, -100, -100);
        TEST_CHECK(pos.row == 0 && pos.col == 0, "pos_move: clamps at 0, never goes negative");
    }

    print_flush();
    return test_failures_ != 0;
}
