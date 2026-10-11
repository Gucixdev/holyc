#define _XOPEN_SOURCE 700 /* mkdtemp needs XOPEN2K8 - the build's own -D_POSIX_C_SOURCE=200112L predates it */
#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#define TUI_IMPLEMENTATION
#define FILE_DIALOGUE_IMPLEMENTATION
#include "arena_alloc.h"
#include "tui.h"
#include "filedialogue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void make_dir_(const char *path)
{
    mkdir(path, 0700);
}

static void touch_file_(const char *path)
{
    FILE *f = fopen(path, "w");
    if (f) fclose(f);
}

int main(void)
{
    Arena a = {0};
    File_Dialogue fd;
    char root[256];
    char sub[512];
    char path[512];
    i32 err;
    char selected[FILE_DIALOGUE_PATH_CAP];

    print_cstr("test_filedialogue\n");

    {
        char tmpl[] = "/tmp/holyc_filedialogue_test_XXXXXX";
        char *made = mkdtemp(tmpl);
        TEST_CHECK(made != 0, "mkdtemp: created a real temp directory");
        strcpy(root, made);
    }

    sprintf(sub, "%s/a_subdir", root);
    make_dir_(sub);
    sprintf(path, "%s/z_file.txt", root);
    touch_file_(path);
    sprintf(path, "%s/b_file.txt", root);
    touch_file_(path);

    err = file_dialogue_init(&fd, &a, root);
    TEST_CHECK(err == FILE_DIALOGUE_ESUCCESS, "init: scans the temp directory successfully");
    /* entries: ".." + "a_subdir" + "b_file.txt" + "z_file.txt" = 4 */
    TEST_CHECK(fd.entry_count == 4, "init: found exactly the 4 expected entries (.., 1 dir, 2 files)");

    TEST_CHECK(strcmp(fd.entries[0].name, "..") == 0, "sort: '..' sorts first (directory group)");
    TEST_CHECK(fd.entries[1].is_dir == 1 && strcmp(fd.entries[1].name, "a_subdir") == 0,
               "sort: a_subdir is the second entry (still in the directory group)");
    TEST_CHECK(strcmp(fd.entries[2].name, "b_file.txt") == 0, "sort: files sorted alphabetically after directories (b before z)");
    TEST_CHECK(strcmp(fd.entries[3].name, "z_file.txt") == 0, "sort: z_file.txt last");

    TEST_CHECK(fd.cursor == 0, "init: cursor starts at 0");
    file_dialogue_move_cursor(&fd, 1);
    TEST_CHECK(fd.cursor == 1, "move_cursor: +1 moves onto a_subdir");
    file_dialogue_move_cursor(&fd, -5);
    TEST_CHECK(fd.cursor == 0, "move_cursor: clamps at 0, doesn't go negative");
    file_dialogue_move_cursor(&fd, 100);
    TEST_CHECK(fd.cursor == fd.entry_count - 1, "move_cursor: clamps at entry_count-1, doesn't overrun");

    file_dialogue_move_cursor(&fd, -100);
    TEST_CHECK(fd.cursor == 0, "move_cursor: back to 0 (on '..')");
    file_dialogue_move_cursor(&fd, 1); /* onto a_subdir */
    err = file_dialogue_enter(&fd, selected);
    TEST_CHECK(err == 1, "enter: descending into a directory returns 1 (navigated, not selected)");
    TEST_CHECK(strstr(fd.cwd, "a_subdir") != 0, "enter: cwd now points inside a_subdir");
    TEST_CHECK(fd.entry_count == 1, "enter: empty subdirectory has just '..'");

    err = file_dialogue_up(&fd);
    TEST_CHECK(err == 1, "up: returns to the parent directory");
    TEST_CHECK(strcmp(fd.cwd, root) == 0, "up: cwd is back to the original temp root");
    TEST_CHECK(fd.entry_count == 4, "up: rescanned, all 4 entries visible again");

    file_dialogue_move_cursor(&fd, 2); /* b_file.txt */
    err = file_dialogue_enter(&fd, selected);
    TEST_CHECK(err == 2, "enter: selecting a file returns 2 (selected, not navigated)");
    TEST_CHECK(strstr(selected, "b_file.txt") != 0, "enter: out_path contains the selected file's name");

    err = file_dialogue_up(&fd); /* cwd was never changed by selecting a file, still at root */
    (void)err;

    {
        char bogus[300];
        i32 e;
        strcpy(bogus, root);
        strcat(bogus, "/does_not_exist_at_all");
        e = file_dialogue_init(&fd, &a, bogus);
        TEST_CHECK(e == FILE_DIALOGUE_ENOTFOUND, "init: a nonexistent directory is rejected cleanly, not crashed on");
    }

    {
        Tui_Buffer buf;
        i32 y;
        file_dialogue_init(&fd, &a, root);
        tui_buffer_init(&buf, 40, 10);
        tui_buffer_clear(&buf, 0, 0);
        file_dialogue_render(&fd, &buf, 0, 0, 10, 1, 2);
        y = 0; /* row 0 is '..' - first cell should be non-blank ('.') */
        TEST_CHECK(buf.cells[y * buf.width + 0].ch == '.', "render: first row's first cell is '.' from '..'");
        tui_buffer_free(&buf);
    }

    arena_free(&a);
    print_flush();
    return test_failures_ != 0;
}
