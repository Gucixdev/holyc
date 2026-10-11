#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#define INI_DEC_IMPLEMENTATION
#include "alloc/arena_alloc.h"
#include "codec/ini_dec.h"

#include <stdio.h>
#include <string.h>

static i32 streq_(const char *a, const char *b)
{
    if (!a || !b) return a == b;
    return strcmp(a, b) == 0;
}

int main(void)
{
    Arena a = {0};
    Ini ini;
    const char *path = "/tmp/holyc_test_ini_XXXX.ini";
    FILE *f;
    i32 loaded;

    print_cstr("test_ini\n");

    f = fopen(path, "wb");
    fprintf(f,
        "top_key = top_value\n"
        "; a comment line, discarded\n"
        "[Section One]\n"
        "name = value\n"
        "quoted = \"has spaces and a \\ttab\"\n"
        "MixedCase = yes\n"
        "[section two]\n"
        "name = other_value\n"
        "no_equals_sign_gets_discarded\n");
    fclose(f);

    loaded = ini_load(&a, &ini, path);
    TEST_CHECK(loaded, "ini_load succeeds on a real file");

    TEST_CHECK(streq_(ini_get(&ini, (const char *)0, "top_key"), "top_value"),
               "top-level key (before any section) is retrievable with section=NULL");

    TEST_CHECK(streq_(ini_get(&ini, "Section One", "name"), "value"),
               "key inside a named section is retrievable");
    TEST_CHECK(streq_(ini_get(&ini, "section one", "name"), "value"),
               "section name lookup is case-insensitive");
    TEST_CHECK(streq_(ini_get(&ini, "Section One", "mixedcase"), "yes"),
               "key name lookup is case-insensitive");

    TEST_CHECK(streq_(ini_get(&ini, "section two", "name"), "other_value"),
               "second section's same-named key resolves independently");

    TEST_CHECK(ini_get(&ini, "Section One", "nonexistent") == (const char *)0,
               "missing key returns NULL");
    TEST_CHECK(ini_get(&ini, "nonexistent section", "name") == (const char *)0,
               "missing section returns NULL");

    {
        const char *q = ini_get(&ini, "Section One", "quoted");
        TEST_CHECK(q != (const char *)0 && streq_(q, "has spaces and a \ttab"),
                   "quoted value is unescaped (spaces preserved, \\t becomes a real tab)");
    }

    remove(path);
    arena_free(&a);
    print_flush();
    return test_failures_ != 0;
}
