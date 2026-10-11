#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#define FUZZY_MATCH_IMPLEMENTATION
#include "core/arena_alloc.h"
#include "util/fuzzy_match.h"

#include <string.h>

static i32 score(Arena *a, const char *text, const char *pattern)
{
    return fuzzy_match(a, text, strlen(text), pattern, strlen(pattern));
}

int main(void)
{
    Arena a = {0};

    /* basic properties */
    TEST_CHECK(score(&a, "abc", "xyz") < 0, "no subsequence at all -> negative (no match)");
    TEST_CHECK(score(&a, "abc", "abc") >= 0, "exact match is a real match");
    TEST_CHECK(score(&a, "axbxcx", "abc") >= 0, "scattered subsequence still matches");
    TEST_CHECK(score(&a, "ab", "abc") < 0, "pattern longer than text -> no match");
    TEST_CHECK(score(&a, "anything", "") == 0, "empty pattern matches trivially with score 0");

    /* consecutive matches should outscore scattered ones for the same characters */
    TEST_CHECK(score(&a, "abcdef", "abc") > score(&a, "axbxcx", "abc"),
               "a consecutive match scores higher than the same letters scattered with gaps");

    /* word-boundary matches should outscore mid-word matches */
    TEST_CHECK(score(&a, "foo_bar", "bar") > score(&a, "foobar_x", "bar"),
               "a match starting right after a word-boundary delimiter scores higher than a mid-word match");
    TEST_CHECK(score(&a, "main.c", "main") > score(&a, "xxmainxx", "main"),
               "a match at the very start of the string scores higher than the same match buried mid-string");

    /* camelCase boundary bonus */
    TEST_CHECK(score(&a, "MainWindow", "win") > score(&a, "mainwindow", "win"),
               "a camelCase boundary (Window's W) gives a real bonus over the same match with no case transition");

    /* cross-validated against the real `fzf --filter` binary (see the
     * shell session this test's own commit was verified against) -
     * not just internal self-consistency: these are the REAL relative
     * orderings `printf "...candidates..." | fzf --filter=QUERY`
     * produced on this machine (fzf 0.74.2). */
    {
        /* printf "src/main.c\nsrc/foo/main_test.c\nmain.py\nMainWindow.java\nsomething_main_related.txt\nxxmainxx\n" | fzf --filter="main"
         * ->  main.py > MainWindow.java > src/main.c > src/foo/main_test.c > something_main_related.txt > xxmainxx */
        i32 s_main_py = score(&a, "main.py", "main");
        i32 s_MainWindow = score(&a, "MainWindow.java", "main");
        i32 s_src_main_c = score(&a, "src/main.c", "main");
        i32 s_main_test = score(&a, "src/foo/main_test.c", "main");
        i32 s_related = score(&a, "something_main_related.txt", "main");
        i32 s_xxmainxx = score(&a, "xxmainxx", "main");

        TEST_CHECK(s_main_py >= s_MainWindow, "cross-validated vs real fzf: main.py ranks >= MainWindow.java for query \"main\"");
        TEST_CHECK(s_MainWindow >= s_src_main_c, "cross-validated vs real fzf: MainWindow.java ranks >= src/main.c");
        TEST_CHECK(s_src_main_c >= s_main_test, "cross-validated vs real fzf: src/main.c ranks >= src/foo/main_test.c");
        TEST_CHECK(s_main_test >= s_related, "cross-validated vs real fzf: src/foo/main_test.c ranks >= something_main_related.txt");
        TEST_CHECK(s_related >= s_xxmainxx, "cross-validated vs real fzf: something_main_related.txt ranks >= xxmainxx");
    }

    {
        /* printf "config.json\nConfigLoader.java\nconfig_loader.py\ncfg.json\nmy_config_file.txt\n" | fzf --filter="cfg"
         * -> cfg.json > config.json > config_loader.py > ConfigLoader.java > my_config_file.txt */
        i32 s_cfg_json = score(&a, "cfg.json", "cfg");
        i32 s_config_json = score(&a, "config.json", "cfg");
        i32 s_config_loader_py = score(&a, "config_loader.py", "cfg");
        i32 s_ConfigLoader = score(&a, "ConfigLoader.java", "cfg");
        i32 s_my_config_file = score(&a, "my_config_file.txt", "cfg");

        TEST_CHECK(s_cfg_json >= s_config_json, "cross-validated vs real fzf: cfg.json ranks >= config.json for query \"cfg\"");
        TEST_CHECK(s_config_json >= s_config_loader_py, "cross-validated vs real fzf: config.json ranks >= config_loader.py");
        TEST_CHECK(s_config_loader_py >= s_ConfigLoader, "cross-validated vs real fzf: config_loader.py ranks >= ConfigLoader.java");
        TEST_CHECK(s_ConfigLoader >= s_my_config_file, "cross-validated vs real fzf: ConfigLoader.java ranks >= my_config_file.txt");
    }

    arena_free(&a);
    print_flush();
    return test_failures_ != 0;
}
