#include "test_common.h"
#define PATH_IMPLEMENTATION
#include "path.h"

#include <string.h>

static i32 streq_(const char *a, const char *b) { return strcmp(a, b) == 0; }

int main(void)
{
    char buf[256];

    print_cstr("test_path\n");

    /* basename */
    {
        const char *b; size_t n;
        path_get_basename("/a/b/c.txt", &b, &n);
        TEST_CHECK(n == 5 && strncmp(b, "c.txt", n) == 0, "basename of a normal path");
        path_get_basename("/a/b/", &b, &n);
        TEST_CHECK(n == 1 && strncmp(b, "b", n) == 0, "basename ignores a trailing separator");
        path_get_basename("/", &b, &n);
        TEST_CHECK(b == (const char *)0 && n == 0, "root path has no basename");
        path_get_basename("file.txt", &b, &n);
        TEST_CHECK(n == 8 && strncmp(b, "file.txt", n) == 0, "basename of a relative path with no directory");
    }

    /* dirname */
    {
        size_t n;
        path_get_dirname("/a/b/c.txt", &n);
        TEST_CHECK(n == 4 && strncmp("/a/b/c.txt", "/a/b", n) == 0, "dirname of a nested path");
        path_get_dirname("/file", &n);
        TEST_CHECK(n == 1, "dirname of a top-level absolute file is just the root separator");
        path_get_dirname("file", &n);
        TEST_CHECK(n == 0, "relative path with no directory component has dirname length 0");
    }

    /* extension */
    {
        const char *e; size_t n;
        TEST_CHECK(path_get_extension("archive.tar.gz", &e, &n) && n == 3 && strncmp(e, ".gz", n) == 0,
                   "extension is the LAST dot-suffix, not everything after the first dot");
        TEST_CHECK(!path_get_extension(".gitignore", &e, &n), "a leading dot alone is not an extension");
        TEST_CHECK(!path_get_extension("noext", &e, &n), "a name with no dot has no extension");
        TEST_CHECK(path_has_extension("a.c"), "has_extension true case");
        TEST_CHECK(!path_has_extension("Makefile"), "has_extension false case");
    }

    /* is_absolute */
    TEST_CHECK(path_is_absolute("/a/b"), "absolute path recognized");
    TEST_CHECK(!path_is_absolute("a/b"), "relative path recognized");

    /* join */
    {
        size_t n = path_join("a", "b", buf, sizeof(buf));
        TEST_CHECK(n == 3 && streq_(buf, "a/b"), "join inserts exactly one separator");
        n = path_join("a/", "/b", buf, sizeof(buf));
        TEST_CHECK(n == 3 && streq_(buf, "a/b"), "join collapses a doubled separator at the seam");
        n = path_join("", "b", buf, sizeof(buf));
        TEST_CHECK(n == 1 && streq_(buf, "b"), "join with an empty first path is just the second path");
        n = path_join("/root", "sub", buf, 4);
        TEST_CHECK(n == 9 && streq_(buf, "/ro"), "join truncates safely and still NUL-terminates, returning the untruncated length");
    }

    /* normalize */
    {
        size_t n = path_normalize("/a/b/../c", buf, sizeof(buf));
        TEST_CHECK(n == 4 && streq_(buf, "/a/c"), "normalize resolves a '..' against a real segment");
        n = path_normalize("/a/./b", buf, sizeof(buf));
        TEST_CHECK(n == 4 && streq_(buf, "/a/b"), "normalize drops '.' segments");
        n = path_normalize("/a//b///c", buf, sizeof(buf));
        TEST_CHECK(n == 6 && streq_(buf, "/a/b/c"), "normalize collapses repeated separators");
        n = path_normalize("/../a", buf, sizeof(buf));
        TEST_CHECK(n == 2 && streq_(buf, "/a"), "'..' above an absolute root is simply dropped");
        n = path_normalize("../../a", buf, sizeof(buf));
        TEST_CHECK(n == 7 && streq_(buf, "../../a"), "leading '..' in a relative path is preserved, nothing to resolve against");
        n = path_normalize("a/..", buf, sizeof(buf));
        TEST_CHECK(n == 1 && streq_(buf, "."), "a path that fully cancels out normalizes to '.'");
        n = path_normalize("/", buf, sizeof(buf));
        TEST_CHECK(n == 1 && streq_(buf, "/"), "root normalizes to itself");
    }

    print_flush();
    return test_failures_ != 0;
}
