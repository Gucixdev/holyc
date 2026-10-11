#include "test_common.h"
#define TAR_IMPLEMENTATION
#include "codec/tinytar.h"

#include <string.h>
#include <stdio.h>

int main(void)
{
    Tar tar;
    const char *path = "/tmp/holyc_test_tar_XXXX.tar";
    const char *data_a = "contents of file a";
    const char *data_b = "different contents for b, a bit longer than a";
    i32 err;

    print_cstr("test_tar\n");

    err = tar_open(&tar, path, "w");
    TEST_CHECK(err == TAR_ESUCCESS, "tar_open for writing succeeds");

    err = tar_write_dir_header(&tar, "adir");
    TEST_CHECK(err == TAR_ESUCCESS, "writing a directory header succeeds");

    err = tar_write_file_header(&tar, "a.txt", (u32)strlen(data_a));
    TEST_CHECK(err == TAR_ESUCCESS, "writing a's file header succeeds");
    err = tar_write_data(&tar, data_a, (u32)strlen(data_a));
    TEST_CHECK(err == TAR_ESUCCESS, "writing a's data succeeds");

    err = tar_write_file_header(&tar, "b.txt", (u32)strlen(data_b));
    TEST_CHECK(err == TAR_ESUCCESS, "writing b's file header succeeds");
    err = tar_write_data(&tar, data_b, (u32)strlen(data_b));
    TEST_CHECK(err == TAR_ESUCCESS, "writing b's data succeeds");

    err = tar_finalize(&tar);
    TEST_CHECK(err == TAR_ESUCCESS, "tar_finalize succeeds");
    tar_close(&tar);

    err = tar_open(&tar, path, "r");
    TEST_CHECK(err == TAR_ESUCCESS, "tar_open for reading the file just written succeeds");

    {
        Tar_Header h;
        i32 all_ok = 1;
        i32 saw_dir = 0, saw_a = 0, saw_b = 0;

        err = tar_rewind(&tar);
        TEST_CHECK(err == TAR_ESUCCESS, "tar_rewind succeeds");

        while ((err = tar_read_header(&tar, &h)) == TAR_ESUCCESS) {
            if (strcmp(h.name, "adir") == 0) {
                if (h.type != TAR_TDIR) all_ok = 0;
                saw_dir = 1;
            } else if (strcmp(h.name, "a.txt") == 0) {
                char buf[64];
                if (h.type != TAR_TREG || h.size != strlen(data_a)) all_ok = 0;
                if (tar_read_data(&tar, buf, h.size) != TAR_ESUCCESS) all_ok = 0;
                buf[h.size] = '\0';
                if (strcmp(buf, data_a) != 0) all_ok = 0;
                saw_a = 1;
            } else if (strcmp(h.name, "b.txt") == 0) {
                char buf[64];
                if (h.type != TAR_TREG || h.size != strlen(data_b)) all_ok = 0;
                if (tar_read_data(&tar, buf, h.size) != TAR_ESUCCESS) all_ok = 0;
                buf[h.size] = '\0';
                if (strcmp(buf, data_b) != 0) all_ok = 0;
                saw_b = 1;
            }
            tar_next(&tar);
        }

        TEST_CHECK(saw_dir && saw_a && saw_b, "iterating the archive visits all three entries");
        TEST_CHECK(all_ok, "every entry's type/size/data round-trips exactly");
    }

    {
        Tar_Header h;
        err = tar_find(&tar, "b.txt", &h);
        TEST_CHECK(err == TAR_ESUCCESS, "tar_find locates an entry by name");
        TEST_CHECK(h.size == strlen(data_b), "tar_find's returned header has the right size");

        err = tar_find(&tar, "does_not_exist.txt", &h);
        TEST_CHECK(err == TAR_ENOTFOUND, "tar_find reports ENOTFOUND for a missing entry");
    }

    tar_close(&tar);
    remove(path);
    print_flush();
    return test_failures_ != 0;
}
