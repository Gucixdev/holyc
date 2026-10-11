#ifndef HOLYC_IO_H_
#define HOLYC_IO_H_

/* Whole-file read/write - the "slurp a file into memory" / "dump a
 * buffer to a file" primitive every codec header and project tool in
 * this repo would otherwise reimplement from scratch (tinytar.h already needs
 * its own streaming fopen/fread for archive I/O - a genuinely
 * different, incremental-access need this file doesn't try to cover).
 * Uses real stdio (fopen/fread/fwrite) and POSIX stat() rather than
 * print.h's write(2)-only model - same reasoning hardscan.h's own
 * disk probe already gives for the same choice: this is genuine file
 * I/O against arbitrary paths, not the "write bytes to our own
 * stdout" job print.h exists for. */

#include "core/base.h"
#include "core/arena_alloc.h"

typedef struct {
    u8 *data;
    size_t size;
} Io_File;

/* reads the whole file into an Arena-owned buffer. Returns 0 (leaving
 * *out zeroed) if the file can't be opened or read. */
i32 io_read_file(Arena *a, const char *path, Io_File *out);

/* overwrites (or creates) the file at path with exactly `size` bytes
 * from `data`. Returns 0 on failure. */
i32 io_write_file(const char *path, const void *data, size_t size);

i32 io_file_exists(const char *path);
i64 io_file_size(const char *path); /* -1 if the file doesn't exist / can't be stat'd */

#endif /* HOLYC_IO_H_ */

#ifdef IO_IMPLEMENTATION
#ifndef IO_IMPLEMENTATION_
#define IO_IMPLEMENTATION_

#include <stdio.h>
#include <sys/stat.h>

i32 io_read_file(Arena *a, const char *path, Io_File *out)
{
    FILE *f;
    long size;
    size_t got;

    out->data = 0;
    out->size = 0;

    f = fopen(path, "rb");
    if (!f) return 0;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    size = ftell(f);
    if (size < 0) { fclose(f); return 0; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 0; }

    out->data = (u8 *)arena_alloc(a, (size_t)size);
    got = fread(out->data, 1, (size_t)size, f);
    fclose(f);

    if (got != (size_t)size) { out->data = 0; out->size = 0; return 0; }

    out->size = (size_t)size;
    return 1;
}

i32 io_write_file(const char *path, const void *data, size_t size)
{
    FILE *f;
    size_t written;

    f = fopen(path, "wb");
    if (!f) return 0;

    written = fwrite(data, 1, size, f);
    fclose(f);

    return written == size;
}

i32 io_file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

i64 io_file_size(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (i64)st.st_size;
}

#endif /* IO_IMPLEMENTATION_ */
#endif /* IO_IMPLEMENTATION */
