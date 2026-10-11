#ifndef HOLYC_VFS_H_
#define HOLYC_VFS_H_

/* Virtual filesystem: a small read/write/exists interface dispatched
 * through function pointers, backed by either the real disk (thin
 * open/read/write/close wrappers - <fcntl.h>/<unistd.h> included
 * directly in this file, matching this project's "system headers live
 * in their sole real consumer" convention, no shared platform wrapper)
 * or a pure in-memory arena-backed store (useful for tests that
 * shouldn't touch real disk, or for embedding small assets without a
 * filesystem at all).
 *
 * Scope, stated plainly: flat read/write/exists on whole files by path
 * string. NOT implemented (real gaps, not silently dropped): directory
 * listing/iteration, delete, rename, permissions/metadata, symlinks -
 * a caller that needs any of that is doing real filesystem management,
 * which is a bigger, different tool than "read/write a named blob
 * through one interface regardless of backend".
 *
 * The disk backend is rooted at a caller-given directory (vfs_disk_init's
 * root_dir) and every path is joined against it via path.h's path_join -
 * it does NOT resolve/reject ".." path traversal above that root, so
 * this is a convenience rooting, not a security sandbox; a caller
 * exposing untrusted paths to this backend must validate them itself
 * first, the same trust boundary every other file-accepting function in
 * this project already assumes (codec decoders trust their caller to
 * have handed them a file worth decoding, not an adversarial one). */

#include "base.h"
#include "arena_alloc.h"

typedef struct Vfs Vfs;

typedef i32 (*Vfs_Read_Fn)(Vfs *vfs, const char *path, Arena *a, u8 **out_data, size_t *out_size);
typedef i32 (*Vfs_Write_Fn)(Vfs *vfs, const char *path, const u8 *data, size_t size);
typedef i32 (*Vfs_Exists_Fn)(Vfs *vfs, const char *path);

struct Vfs {
    Vfs_Read_Fn read;
    Vfs_Write_Fn write;
    Vfs_Exists_Fn exists;
    void *backend; /* opaque backend state - disk: root dir string, mem: Vfs_Mem_State_* */
};

typedef enum {
    VFS_ESUCCESS    = 0,
    VFS_ENOTFOUND   = -1,
    VFS_EIO         = -2, /* disk backend: open/read/write/close failed */
    VFS_EPATH       = -3  /* joined/stored path exceeded an internal buffer */
} Vfs_Error;

/* backend-agnostic calls - just dispatch through vfs->read/write/exists.
 * Exist as real functions (not macros) so callers can take their
 * address / pass a Vfs* around without caring which backend it is. */
i32 vfs_read(Vfs *vfs, const char *path, Arena *a, u8 **out_data, size_t *out_size);
i32 vfs_write(Vfs *vfs, const char *path, const u8 *data, size_t size);
i32 vfs_exists(Vfs *vfs, const char *path);

/* root_dir must outlive vfs (only its pointer is stored - matches this
 * project's usual "caller owns the string" convention, e.g. path.h). */
void vfs_disk_init(Vfs *vfs, const char *root_dir);

/* backend state itself is arena-allocated from `a` - every mem-backend
 * write also arena-allocates its bytes from `a`, so the whole in-memory
 * filesystem lives and dies with that one arena, same lifetime model as
 * everything else built on arena_alloc.h. */
void vfs_mem_init(Vfs *vfs, Arena *a);

#endif /* HOLYC_VFS_H_ */

#ifdef VFS_IMPLEMENTATION
#ifndef VFS_IMPLEMENTATION_
#define VFS_IMPLEMENTATION_

#include "path.h"
#include <fcntl.h>
#include <unistd.h>

#ifndef VFS_PATH_MAX
#define VFS_PATH_MAX 4096
#endif

/* ---- disk backend ---- */

static i32 vfs_disk_join_(Vfs *vfs, const char *path, char *out, size_t out_size)
{
    const char *root = (const char *)vfs->backend;
    size_t n = path_join(root, path, out, out_size);
    if (n == 0 || n >= out_size) return VFS_EPATH;
    return VFS_ESUCCESS;
}

static i32 vfs_disk_read_(Vfs *vfs, const char *path, Arena *a, u8 **out_data, size_t *out_size)
{
    char full[VFS_PATH_MAX];
    i32 fd;
    i64 size, got;
    u8 *buf;
    i32 err = vfs_disk_join_(vfs, path, full, sizeof(full));
    if (err != VFS_ESUCCESS) return err;

    fd = open(full, O_RDONLY);
    if (fd < 0) return VFS_ENOTFOUND;

    size = lseek(fd, 0, SEEK_END);
    if (size < 0 || lseek(fd, 0, SEEK_SET) < 0) {
        close(fd);
        return VFS_EIO;
    }

    buf = (u8 *)arena_alloc(a, (size_t)size);
    got = 0;
    while (got < size) {
        i64 n = read(fd, buf + got, (size_t)(size - got));
        if (n <= 0) {
            close(fd);
            return VFS_EIO;
        }
        got += n;
    }
    close(fd);

    *out_data = buf;
    *out_size = (size_t)size;
    return VFS_ESUCCESS;
}

static i32 vfs_disk_write_(Vfs *vfs, const char *path, const u8 *data, size_t size)
{
    char full[VFS_PATH_MAX];
    i32 fd;
    size_t written;
    i32 err = vfs_disk_join_(vfs, path, full, sizeof(full));
    if (err != VFS_ESUCCESS) return err;

    fd = open(full, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return VFS_EIO;

    written = 0;
    while (written < size) {
        i64 n = write(fd, data + written, size - written);
        if (n <= 0) {
            close(fd);
            return VFS_EIO;
        }
        written += (size_t)n;
    }
    close(fd);
    return VFS_ESUCCESS;
}

static i32 vfs_disk_exists_(Vfs *vfs, const char *path)
{
    char full[VFS_PATH_MAX];
    i32 fd;
    i32 err = vfs_disk_join_(vfs, path, full, sizeof(full));
    if (err != VFS_ESUCCESS) return 0;

    fd = open(full, O_RDONLY);
    if (fd < 0) return 0;
    close(fd);
    return 1;
}

void vfs_disk_init(Vfs *vfs, const char *root_dir)
{
    vfs->read = vfs_disk_read_;
    vfs->write = vfs_disk_write_;
    vfs->exists = vfs_disk_exists_;
    vfs->backend = (void *)root_dir;
}

/* ---- mem backend ---- */

typedef struct Vfs_Mem_Entry_ Vfs_Mem_Entry_;
struct Vfs_Mem_Entry_ {
    Vfs_Mem_Entry_ *next;
    char path[256];
    u8 *data;
    size_t size;
};

typedef struct {
    Arena *a;
    Vfs_Mem_Entry_ *head;
} Vfs_Mem_State_;

static Vfs_Mem_Entry_ *vfs_mem_find_(Vfs_Mem_State_ *st, const char *path)
{
    Vfs_Mem_Entry_ *e;
    for (e = st->head; e != NULL; e = e->next) {
        if (strcmp(e->path, path) == 0) return e;
    }
    return NULL;
}

static i32 vfs_mem_read_(Vfs *vfs, const char *path, Arena *a, u8 **out_data, size_t *out_size)
{
    Vfs_Mem_State_ *st = (Vfs_Mem_State_ *)vfs->backend;
    Vfs_Mem_Entry_ *e = vfs_mem_find_(st, path);
    if (e == NULL) return VFS_ENOTFOUND;

    *out_data = (u8 *)arena_memdup(a, e->data, e->size);
    *out_size = e->size;
    return VFS_ESUCCESS;
}

static i32 vfs_mem_write_(Vfs *vfs, const char *path, const u8 *data, size_t size)
{
    Vfs_Mem_State_ *st = (Vfs_Mem_State_ *)vfs->backend;
    Vfs_Mem_Entry_ *e = vfs_mem_find_(st, path);
    size_t n = arena_strlen(path);

    if (e == NULL) {
        if (n >= sizeof(((Vfs_Mem_Entry_ *)0)->path)) return VFS_EPATH;
        e = (Vfs_Mem_Entry_ *)arena_alloc(st->a, sizeof(*e));
        arena_memcpy(e->path, path, n);
        e->path[n] = '\0';
        e->next = st->head;
        st->head = e;
    }

    /* overwrite = point at a freshly arena_memdup'd copy - the old bytes
     * (if any) are simply abandoned in the arena rather than freed, the
     * same "no individual frees" tradeoff every other arena-backed
     * structure in this project already makes. */
    e->data = (u8 *)arena_memdup(st->a, (void *)data, size);
    e->size = size;
    return VFS_ESUCCESS;
}

static i32 vfs_mem_exists_(Vfs *vfs, const char *path)
{
    Vfs_Mem_State_ *st = (Vfs_Mem_State_ *)vfs->backend;
    return vfs_mem_find_(st, path) != NULL;
}

void vfs_mem_init(Vfs *vfs, Arena *a)
{
    Vfs_Mem_State_ *st = (Vfs_Mem_State_ *)arena_alloc(a, sizeof(*st));
    st->a = a;
    st->head = NULL;

    vfs->read = vfs_mem_read_;
    vfs->write = vfs_mem_write_;
    vfs->exists = vfs_mem_exists_;
    vfs->backend = (void *)st;
}

/* ---- dispatch ---- */

i32 vfs_read(Vfs *vfs, const char *path, Arena *a, u8 **out_data, size_t *out_size)
{
    return vfs->read(vfs, path, a, out_data, out_size);
}

i32 vfs_write(Vfs *vfs, const char *path, const u8 *data, size_t size)
{
    return vfs->write(vfs, path, data, size);
}

i32 vfs_exists(Vfs *vfs, const char *path)
{
    return vfs->exists(vfs, path);
}

#endif /* VFS_IMPLEMENTATION_ */
#endif /* VFS_IMPLEMENTATION */
