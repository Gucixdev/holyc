#include "test_common.h"
#define EXE_PATH_IMPLEMENTATION
#include "exe_path.h"

#include <sys/stat.h>

int main(void)
{
    i32 length, dirname_length;
    char buf[1024];

    print_cstr("test_exe_path\n");

    length = exe_path_get((char *)0, 0, (i32 *)0);
    TEST_CHECK(length > 0, "first call (out=NULL) returns a positive required length");

    dirname_length = -1;
    length = exe_path_get(buf, (i32)sizeof(buf), &dirname_length);
    TEST_CHECK(length > 0, "second call fills the buffer and returns the same-shaped positive length");
    buf[length] = '\0';

    TEST_CHECK(buf[0] == '/', "resolved executable path is absolute");
    TEST_CHECK(dirname_length > 0 && dirname_length < length, "dirname_length lands strictly inside the path");
    TEST_CHECK(buf[dirname_length] == '/', "the byte at dirname_length is the directory-separating slash");

    {
        struct stat st;
        i32 exists = (stat(buf, &st) == 0);
        TEST_CHECK(exists, "the resolved path actually exists on disk (this test binary itself)");
    }

    print_flush();
    return test_failures_ != 0;
}
