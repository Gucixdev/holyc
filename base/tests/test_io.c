#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#include "arena_alloc.h"
#define IO_IMPLEMENTATION
#include "io.h"

int main(void)
{
    Arena a = {0};
    const char *path = "/tmp/holyc_test_io.tmp";

    print_cstr("test_io\n");

    TEST_CHECK(!io_file_exists("/tmp/holyc_test_io_does_not_exist.tmp"), "a nonexistent path reports not existing");
    TEST_CHECK(io_file_size("/tmp/holyc_test_io_does_not_exist.tmp") == -1, "size of a nonexistent file is -1");

    TEST_CHECK(io_write_file(path, "hello io", 8), "write_file succeeds for a writable path");
    TEST_CHECK(io_file_exists(path), "a written file now exists");
    TEST_CHECK(io_file_size(path) == 8, "size matches exactly what was written");

    {
        Io_File f;
        TEST_CHECK(io_read_file(&a, path, &f), "read_file succeeds on the file just written");
        TEST_CHECK(f.size == 8, "read_file reports the correct size");
        TEST_CHECK(f.data[0]=='h' && f.data[7]=='o', "read_file's contents match exactly what was written");
    }

    TEST_CHECK(io_write_file(path, "shorter", 7), "write_file overwrites an existing file rather than appending");
    TEST_CHECK(io_file_size(path) == 7, "the overwritten file's size reflects only the new content");

    {
        Io_File f;
        TEST_CHECK(!io_read_file(&a, "/tmp/holyc_test_io_does_not_exist.tmp", &f), "read_file fails cleanly on a missing file");
        TEST_CHECK(f.data == NULL && f.size == 0, "a failed read_file leaves the Io_File zeroed");
    }

    remove(path);

    print_flush();
    return test_failures_ != 0;
}
