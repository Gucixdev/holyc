#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#define PKG_IMPLEMENTATION
#include "arena_alloc.h"
#include "pkg.h"

#include <string.h>

int main(void)
{
    Arena a = {0};
    Pkg p;

    print_cstr("test_pkg\n");

    pkg_init(&a, &p, "holyc-demo", "0.1.0", "main");
    TEST_CHECK(strcmp(p.name, "holyc-demo") == 0, "pkg_init sets the name");
    TEST_CHECK(strcmp(p.version, "0.1.0") == 0, "pkg_init sets the version");
    TEST_CHECK(strcmp(p.entry_point, "main") == 0, "pkg_init sets the entry point");
    TEST_CHECK(p.dep_count == 0, "a freshly initialized package has no dependencies");

    TEST_CHECK(pkg_add_dependency(&a, &p, "sdl2", ">=2.0") == 1, "adding a dependency succeeds");
    TEST_CHECK(pkg_add_dependency(&a, &p, "vulkan", "*") == 1, "adding a second dependency succeeds");
    TEST_CHECK(p.dep_count == 2, "dep_count reflects both additions");

    {
        const Pkg_Dependency *d = pkg_find_dependency(&p, "sdl2");
        TEST_CHECK(d != NULL, "pkg_find_dependency locates a real dependency by name");
        TEST_CHECK(strcmp(d->version_constraint, ">=2.0") == 0, "the found dependency's version constraint matches what was added");
    }

    TEST_CHECK(pkg_find_dependency(&p, "does-not-exist") == NULL, "pkg_find_dependency returns NULL for a name never added");

    {
        i32 i;
        Pkg full;
        pkg_init(&a, &full, "x", "1", "main");
        for (i = 0; i < PKG_MAX_DEPS; i++) {
            char name[16];
            name[0] = 'd'; name[1] = (char)('0' + i / 10); name[2] = (char)('0' + i % 10); name[3] = '\0';
            TEST_CHECK(pkg_add_dependency(&a, &full, name, "*") == 1, "filling up to PKG_MAX_DEPS all succeed");
        }
        TEST_CHECK(pkg_add_dependency(&a, &full, "one-too-many", "*") == 0, "adding one more past PKG_MAX_DEPS fails cleanly, doesn't overflow");
    }

    arena_free(&a);
    print_flush();
    return test_failures_ != 0;
}
