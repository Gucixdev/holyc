#define _XOPEN_SOURCE 700 /* mkdtemp needs XOPEN2K8 - the build's own -D_POSIX_C_SOURCE=200112L predates it */
#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#define PATH_IMPLEMENTATION
#define VFS_IMPLEMENTATION
#include "arena_alloc.h"
#include "path.h"
#include "vfs.h"

#include <string.h>

/* same backend-agnostic checks run against BOTH backends below - proves
 * the vtable dispatch actually behaves identically regardless of what's
 * underneath, not just that each backend happens to work in isolation. */
static void run_backend_checks_(Vfs *vfs, Arena *a, const char *label)
{
    u8 *data;
    size_t size;
    i32 err;

    TEST_CHECK(vfs_exists(vfs, "nope.txt") == 0, "not-yet-written file reports not existing");

    err = vfs_write(vfs, "hello.txt", (const u8 *)"hello, vfs", 10);
    TEST_CHECK(err == VFS_ESUCCESS, "write succeeds");

    TEST_CHECK(vfs_exists(vfs, "hello.txt") == 1, "written file now reports existing");

    err = vfs_read(vfs, "hello.txt", a, &data, &size);
    TEST_CHECK(err == VFS_ESUCCESS, "read succeeds");
    TEST_CHECK(size == 10, "read size matches what was written");
    TEST_CHECK(memcmp(data, "hello, vfs", 10) == 0, "read bytes match what was written");

    err = vfs_write(vfs, "hello.txt", (const u8 *)"overwritten", 11);
    TEST_CHECK(err == VFS_ESUCCESS, "overwrite succeeds");
    err = vfs_read(vfs, "hello.txt", a, &data, &size);
    TEST_CHECK(err == VFS_ESUCCESS && size == 11 && memcmp(data, "overwritten", 11) == 0,
               "read after overwrite sees the NEW bytes, not the old ones");

    err = vfs_read(vfs, "never_written.txt", a, &data, &size);
    TEST_CHECK(err == VFS_ENOTFOUND, "reading a nonexistent path reports VFS_ENOTFOUND, not a crash");

    (void)label;
}

int main(void)
{
    Arena a = {0};

    print_cstr("test_vfs\n");

    {
        Vfs vfs;
        vfs_mem_init(&vfs, &a);
        run_backend_checks_(&vfs, &a, "mem");
    }

    {
        char tmpl[] = "/tmp/holyc_vfs_test_XXXXXX";
        char *root = mkdtemp(tmpl);
        TEST_CHECK(root != 0, "mkdtemp: created a real temp directory");

        {
            Vfs vfs;
            vfs_disk_init(&vfs, root);
            run_backend_checks_(&vfs, &a, "disk");
        }
    }

    arena_free(&a);
    print_flush();
    return test_failures_ != 0;
}
