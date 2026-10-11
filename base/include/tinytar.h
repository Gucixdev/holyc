#ifndef HOLYC_TAR_H_
#define HOLYC_TAR_H_

/* Ported from rxi's microtar (MIT). Standard POSIX ustar layout (the
 * same format `tar` itself reads/writes), unchanged - only the plumbing
 * changed: int -> i32, unsigned -> u32, the stream read/write/seek/close
 * function-pointer backend kept as-is (already backend-agnostic by
 * design, the same pluggable-backend shape arena_alloc.h's ARENA_BACKEND
 * uses, just at the struct-of-function-pointers level instead of a
 * compile-time #define) - a default FILE*-based backend is provided
 * (mtar_open), a caller can still point tar->read/write/seek/close
 * elsewhere (e.g. an in-memory buffer) without touching this file. */

#include "base.h"

/* own local memset_/strcmp_/strcpy_ - own copy per this project's
 * standing "no cross-IMPLEMENTATION-guard dependency" rule, avoiding a
 * <string.h> dependency for three small byte loops. */
static void *tar_memset_(void *dst, i32 val, size_t n)
{
    u8 *d = (u8 *)dst;
    size_t i;
    for (i = 0; i < n; i++) d[i] = (u8)val;
    return dst;
}

static i32 tar_strcmp_(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (i32)(u8)*a - (i32)(u8)*b;
}

static char *tar_strcpy_(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++) != '\0') { /* copy including the NUL */ }
    return dst;
}

typedef enum {
    TAR_ESUCCESS    =  0,
    TAR_EFAILURE    = -1,
    TAR_EOPENFAIL   = -2,
    TAR_EREADFAIL   = -3,
    TAR_EWRITEFAIL  = -4,
    TAR_ESEEKFAIL   = -5,
    TAR_EBADCHKSUM  = -6,
    TAR_ENULLRECORD = -7,
    TAR_ENOTFOUND   = -8
} Tar_Error;

typedef enum {
    TAR_TREG  = '0',
    TAR_TLNK  = '1',
    TAR_TSYM  = '2',
    TAR_TCHR  = '3',
    TAR_TBLK  = '4',
    TAR_TDIR  = '5',
    TAR_TFIFO = '6'
} Tar_Entry_Type;

typedef struct {
    u32 mode;
    u32 owner;
    u32 size;
    u32 mtime;
    u32 type;
    char name[100];
    char linkname[100];
} Tar_Header;

typedef struct Tar Tar;

struct Tar {
    i32 (*read)(Tar *tar, void *data, u32 size);
    i32 (*write)(Tar *tar, const void *data, u32 size);
    i32 (*seek)(Tar *tar, u32 pos);
    i32 (*close)(Tar *tar);
    void *stream;
    u32 pos;
    u32 remaining_data;
    u32 last_header;
};

const char *tar_strerror(i32 err);

/* mode is "r", "w", or "a" (same convention as fopen, forced to binary
 * internally) - opens a real file via the FILE*-based backend. */
i32 tar_open(Tar *tar, const char *filename, const char *mode);
i32 tar_close(Tar *tar);

i32 tar_seek(Tar *tar, u32 pos);
i32 tar_rewind(Tar *tar);
i32 tar_next(Tar *tar);
i32 tar_find(Tar *tar, const char *name, Tar_Header *h);
i32 tar_read_header(Tar *tar, Tar_Header *h);
i32 tar_read_data(Tar *tar, void *ptr, u32 size);

i32 tar_write_header(Tar *tar, const Tar_Header *h);
i32 tar_write_file_header(Tar *tar, const char *name, u32 size);
i32 tar_write_dir_header(Tar *tar, const char *name);
i32 tar_write_data(Tar *tar, const void *data, u32 size);
i32 tar_finalize(Tar *tar);

#endif /* HOLYC_TAR_H_ */

#ifdef TAR_IMPLEMENTATION
#ifndef TAR_IMPLEMENTATION_
#define TAR_IMPLEMENTATION_

#include <stdio.h>


typedef struct {
    char name[100];
    char mode[8];
    char owner[8];
    char group[8];
    char size[12];
    char mtime[12];
    char checksum[8];
    char type;
    char linkname[100];
    char padding_[255];
} Tar_Raw_Header_;

static u32 tar_round_up_(u32 n, u32 incr)
{
    return n + (incr - n % incr) % incr;
}

static u32 tar_checksum_(const Tar_Raw_Header_ *rh)
{
    size_t i;
    const u8 *p = (const u8 *)rh;
    u32 res = 256;
    for (i = 0; i < offsetof(Tar_Raw_Header_, checksum); i++) res += p[i];
    for (i = offsetof(Tar_Raw_Header_, type); i < sizeof(*rh); i++) res += p[i];
    return res;
}

static i32 tar_tread_(Tar *tar, void *data, u32 size)
{
    i32 err = tar->read(tar, data, size);
    tar->pos += size;
    return err;
}

static i32 tar_twrite_(Tar *tar, const void *data, u32 size)
{
    i32 err = tar->write(tar, data, size);
    tar->pos += size;
    return err;
}

static i32 tar_write_null_bytes_(Tar *tar, u32 n)
{
    u32 i;
    char nul = '\0';
    i32 err;
    for (i = 0; i < n; i++) {
        err = tar_twrite_(tar, &nul, 1);
        if (err) return err;
    }
    return TAR_ESUCCESS;
}

static i32 tar_raw_to_header_(Tar_Header *h, const Tar_Raw_Header_ *rh)
{
    u32 chksum1, chksum2;

    if (*rh->checksum == '\0') return TAR_ENULLRECORD;

    chksum1 = tar_checksum_(rh);
    sscanf(rh->checksum, "%o", &chksum2);
    if (chksum1 != chksum2) return TAR_EBADCHKSUM;

    sscanf(rh->mode, "%o", &h->mode);
    sscanf(rh->owner, "%o", &h->owner);
    sscanf(rh->size, "%o", &h->size);
    sscanf(rh->mtime, "%o", &h->mtime);
    h->type = (u32)(u8)rh->type;
    tar_strcpy_(h->name, rh->name);
    tar_strcpy_(h->linkname, rh->linkname);

    return TAR_ESUCCESS;
}

static i32 tar_header_to_raw_(Tar_Raw_Header_ *rh, const Tar_Header *h)
{
    u32 chksum;

    tar_memset_(rh, 0, sizeof(*rh));
    sprintf(rh->mode, "%o", (unsigned)h->mode);
    sprintf(rh->owner, "%o", (unsigned)h->owner);
    sprintf(rh->size, "%o", (unsigned)h->size);
    sprintf(rh->mtime, "%o", (unsigned)h->mtime);
    rh->type = h->type ? (char)h->type : (char)TAR_TREG;
    tar_strcpy_(rh->name, h->name);
    tar_strcpy_(rh->linkname, h->linkname);

    chksum = tar_checksum_(rh);
    sprintf(rh->checksum, "%06o", (unsigned)chksum);
    rh->checksum[7] = ' ';

    return TAR_ESUCCESS;
}

const char *tar_strerror(i32 err)
{
    switch (err) {
    case TAR_ESUCCESS:    return "success";
    case TAR_EFAILURE:    return "failure";
    case TAR_EOPENFAIL:   return "could not open";
    case TAR_EREADFAIL:   return "could not read";
    case TAR_EWRITEFAIL:  return "could not write";
    case TAR_ESEEKFAIL:   return "could not seek";
    case TAR_EBADCHKSUM:  return "bad checksum";
    case TAR_ENULLRECORD: return "null record";
    case TAR_ENOTFOUND:   return "file not found";
    default: break;
    }
    return "unknown error";
}

static i32 tar_file_write_(Tar *tar, const void *data, u32 size)
{
    size_t res = fwrite(data, 1, size, (FILE *)tar->stream);
    return (res == size) ? TAR_ESUCCESS : TAR_EWRITEFAIL;
}

static i32 tar_file_read_(Tar *tar, void *data, u32 size)
{
    size_t res = fread(data, 1, size, (FILE *)tar->stream);
    return (res == size) ? TAR_ESUCCESS : TAR_EREADFAIL;
}

static i32 tar_file_seek_(Tar *tar, u32 offset)
{
    i32 res = fseek((FILE *)tar->stream, (long)offset, SEEK_SET);
    return (res == 0) ? TAR_ESUCCESS : TAR_ESEEKFAIL;
}

static i32 tar_file_close_(Tar *tar)
{
    fclose((FILE *)tar->stream);
    return TAR_ESUCCESS;
}

i32 tar_open(Tar *tar, const char *filename, const char *mode)
{
    i32 err;
    Tar_Header h;

    tar_memset_(tar, 0, sizeof(*tar));
    tar->write = tar_file_write_;
    tar->read = tar_file_read_;
    tar->seek = tar_file_seek_;
    tar->close = tar_file_close_;

    /* mode is documented/contracted as "r"/"w"/"a" (fopen convention),
     * so the first byte alone is a safe, exhaustive dispatch key. */
    switch (mode[0]) {
    case 'r': mode = "rb"; break;
    case 'w': mode = "wb"; break;
    case 'a': mode = "ab"; break;
    default: break;
    }

    tar->stream = fopen(filename, mode);
    if (!tar->stream) return TAR_EOPENFAIL;

    if (*mode == 'r') {
        err = tar_read_header(tar, &h);
        if (err != TAR_ESUCCESS) {
            tar_close(tar);
            return err;
        }
    }

    return TAR_ESUCCESS;
}

i32 tar_close(Tar *tar)
{
    return tar->close(tar);
}

i32 tar_seek(Tar *tar, u32 pos)
{
    i32 err = tar->seek(tar, pos);
    tar->pos = pos;
    return err;
}

i32 tar_rewind(Tar *tar)
{
    tar->remaining_data = 0;
    tar->last_header = 0;
    return tar_seek(tar, 0);
}

i32 tar_next(Tar *tar)
{
    i32 err;
    u32 n;
    Tar_Header h;

    err = tar_read_header(tar, &h);
    if (err) return err;

    n = tar_round_up_(h.size, 512) + (u32)sizeof(Tar_Raw_Header_);
    return tar_seek(tar, tar->pos + n);
}

i32 tar_find(Tar *tar, const char *name, Tar_Header *h)
{
    i32 err;
    Tar_Header header;

    err = tar_rewind(tar);
    if (err) return err;

    while ((err = tar_read_header(tar, &header)) == TAR_ESUCCESS) {
        if (!tar_strcmp_(header.name, name)) {
            if (h) *h = header;
            return TAR_ESUCCESS;
        }
        tar_next(tar);
    }

    if (err == TAR_ENULLRECORD) err = TAR_ENOTFOUND;
    return err;
}

i32 tar_read_header(Tar *tar, Tar_Header *h)
{
    i32 err;
    Tar_Raw_Header_ rh;

    tar->last_header = tar->pos;

    err = tar_tread_(tar, &rh, sizeof(rh));
    if (err) return err;

    err = tar_seek(tar, tar->last_header);
    if (err) return err;

    return tar_raw_to_header_(h, &rh);
}

i32 tar_read_data(Tar *tar, void *ptr, u32 size)
{
    i32 err;

    if (tar->remaining_data == 0) {
        Tar_Header h;
        err = tar_read_header(tar, &h);
        if (err) return err;

        err = tar_seek(tar, tar->pos + (u32)sizeof(Tar_Raw_Header_));
        if (err) return err;
        tar->remaining_data = h.size;
    }

    err = tar_tread_(tar, ptr, size);
    if (err) return err;
    tar->remaining_data -= size;

    if (tar->remaining_data == 0) return tar_seek(tar, tar->last_header);
    return TAR_ESUCCESS;
}

i32 tar_write_header(Tar *tar, const Tar_Header *h)
{
    Tar_Raw_Header_ rh;
    tar_header_to_raw_(&rh, h);
    tar->remaining_data = h->size;
    return tar_twrite_(tar, &rh, sizeof(rh));
}

i32 tar_write_file_header(Tar *tar, const char *name, u32 size)
{
    Tar_Header h;
    tar_memset_(&h, 0, sizeof(h));
    tar_strcpy_(h.name, name);
    h.size = size;
    h.type = TAR_TREG;
    h.mode = 0664;
    return tar_write_header(tar, &h);
}

i32 tar_write_dir_header(Tar *tar, const char *name)
{
    Tar_Header h;
    tar_memset_(&h, 0, sizeof(h));
    tar_strcpy_(h.name, name);
    h.type = TAR_TDIR;
    h.mode = 0775;
    return tar_write_header(tar, &h);
}

i32 tar_write_data(Tar *tar, const void *data, u32 size)
{
    i32 err;

    err = tar_twrite_(tar, data, size);
    if (err) return err;
    tar->remaining_data -= size;

    if (tar->remaining_data == 0) {
        return tar_write_null_bytes_(tar, tar_round_up_(tar->pos, 512) - tar->pos);
    }
    return TAR_ESUCCESS;
}

i32 tar_finalize(Tar *tar)
{
    return tar_write_null_bytes_(tar, (u32)sizeof(Tar_Raw_Header_) * 2);
}

#endif /* TAR_IMPLEMENTATION_ */
#endif /* TAR_IMPLEMENTATION */
