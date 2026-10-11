#ifndef HOLYC_PERSISTANT_ARENA_H_
#define HOLYC_PERSISTANT_ARENA_H_

/* A bump-pointer arena backed by a real file via mmap(MAP_SHARED, ...),
 * not anonymous memory: every byte handed out by persistant_arena_alloc
 * is a real offset into that file, so the arena's own bookkeeping (how
 * much is used) AND its actual content both survive a process restart -
 * close the file, reopen it later (same process or a different one) and
 * persistant_arena_open picks up exactly where the last session left
 * off. arena_alloc.h's own ARENA_BACKEND_LINUX_MMAP is NOT this - that
 * backend is anonymous (MAP_ANONYMOUS), real RAM-only storage that just
 * happens to come from mmap() instead of malloc(); nothing in it is
 * backed by a file, and none of it is there anymore once the process
 * exits. This file exists because that distinction was findable ONLY by
 * actually reading arena_alloc.h's own MAP_ANONYMOUS flag - "mmap-backed"
 * alone doesn't imply "persistent" the way this header's own name did.
 *
 * Deliberately NOT the same shape as Arena's own growable linked list of
 * regions: growing a file-backed mapping means re-mmap()ing at a
 * (possibly different) address, which would invalidate every pointer
 * this arena already handed out to a caller - a real, silent
 * use-after-free footgun for something that's supposed to be MORE
 * durable than a plain Arena, not less. So: one real fixed-size region,
 * capacity decided once at creation and written into the file's own
 * header, matching arena_alloc.h's own ARENA_BACKEND_FLAT reasoning
 * ("reserve one big chunk up front, growth only as a safety net") but
 * taken all the way to "no growth at all" here - persistant_arena_alloc
 * returns NULL on real exhaustion rather than pretending it can always
 * grow. A caller that needs more room decides its own capacity up front
 * (this is a "save file", not a general-purpose heap) or creates a
 * second persistant arena at a second path.
 *
 * Pointers persistant_arena_alloc returns are ONLY valid while this
 * exact Persistant_Arena is open (i.e. between persistant_arena_open and
 * persistant_arena_close) - real virtual addresses from THIS mapping,
 * not portable across a close/reopen (mmap gives no base-address
 * guarantee, and none is needed: persistant_arena_offset/at below are
 * the real way to save "where something lives" across a reopen, the
 * same offset-not-pointer discipline persistant_pool.h's own free-list
 * needs for the identical reason - see that file's own doc comment). */

#include "base.h"
#include "contracts.h"
#include <string.h> /* memcpy */

/* ftruncate/msync are POSIX.1-2001, not strict ISO C89 - same real gap
 * arena_alloc.h's own arena_vsprintf documents for vsnprintf. Compile
 * with -D_POSIX_C_SOURCE=200112L (or -std=gnu89) to expose them under
 * -std=c89; verified this actually clears the implicit-declaration
 * warning, not assumed. */

#ifndef PERSISTANT_ARENA_ASSERT
#define PERSISTANT_ARENA_ASSERT HOLYC_ASSERT
#endif

/* Written verbatim at the start of the mapped file; every field is a
 * fixed-width type and there are no pointers in it, on purpose - this
 * struct's own bytes ARE the durable state, read back by a possibly
 * different process (even a possibly different architecture, though
 * this project targets one platform elsewhere too - see arena_alloc.h's
 * own stance). u64 magic catches "this isn't one of our files" (a wrong
 * path, a truncated/corrupted file) before anything else touches it. */
typedef struct {
    u64 magic;
    u64 capacity;  /* total allocatable bytes, fixed for the file's whole life */
    u64 count;     /* bytes actually used so far - the one field that changes on every alloc */
} Persistant_Arena_Header;

#define PERSISTANT_ARENA_MAGIC ((u64)0x484f4c5943504100ull) /* "HOLYCPA\0" as big-endian bytes, arbitrary but stable */

typedef struct {
    int fd;
    void *base;                 /* mmap() base - the header lives at base[0] */
    Persistant_Arena_Header *header;
    u8 *data;                   /* base + sizeof(Persistant_Arena_Header) - where real allocations start */
} Persistant_Arena;

/* persistant_arena_open - creates `path` at `capacity` bytes if it
 * doesn't exist (or is empty), otherwise reopens it and trusts the
 * REAL persisted capacity/count over whatever `capacity` the caller
 * passed this time (the file is the actual source of truth - a caller
 * that opens an existing arena with a different `capacity` argument
 * gets the file's own real size back, not a mismatch it has to reconcile
 * itself). Returns 1 on success, 0 on any real failure (open/ftruncate/
 * mmap failed, or an existing file's magic doesn't match - a real
 * corrupt-or-wrong file, not silently treated as empty). */
int persistant_arena_open(Persistant_Arena *pa, const char *path, size_t capacity);
void *persistant_arena_alloc(Persistant_Arena *pa, size_t size_bytes);

/* persistant_arena_offset / persistant_arena_at - the real way to save
 * "where something lives" across a close+reopen: a plain pointer from
 * persistant_arena_alloc is only valid for this open session (mmap gives
 * no base-address guarantee across separate mappings), but its OFFSET
 * from the arena's own data start is exactly as durable as the file
 * itself - save the offset (e.g. as a field inside something else this
 * same arena holds), not the pointer. */
size_t persistant_arena_offset(Persistant_Arena *pa, void *ptr);
void *persistant_arena_at(Persistant_Arena *pa, size_t offset);

/* persistant_arena_sync - forces the mapped pages out to disk NOW
 * (msync) rather than waiting for the kernel's own writeback or the
 * real close() to do it - real durability point for a caller that needs
 * "this is on disk before I tell anything else it happened" (a crash
 * between alloc and close would otherwise lose whatever the kernel
 * hadn't flushed yet, same as any other mmap(MAP_SHARED) - not a
 * property specific to this file, just worth real caller-facing control
 * over rather than an implicit "close() alone is enough" assumption). */
void persistant_arena_sync(Persistant_Arena *pa);
void persistant_arena_close(Persistant_Arena *pa);

#endif /* HOLYC_PERSISTANT_ARENA_H_ */

#ifdef PERSISTANT_ARENA_IMPLEMENTATION
#ifndef PERSISTANT_ARENA_IMPLEMENTATION_
#define PERSISTANT_ARENA_IMPLEMENTATION_

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

int persistant_arena_open(Persistant_Arena *pa, const char *path, size_t capacity)
{
    struct stat st;
    size_t total_bytes;
    int is_new;

    pa->fd = open(path, O_RDWR | O_CREAT, 0644);
    if (pa->fd < 0) return 0;

    if (fstat(pa->fd, &st) != 0) {
        close(pa->fd);
        return 0;
    }

    is_new = (st.st_size == 0);
    total_bytes = is_new
        ? sizeof(Persistant_Arena_Header) + capacity
        : (size_t)st.st_size;

    if (is_new) {
        if (ftruncate(pa->fd, (off_t)total_bytes) != 0) {
            close(pa->fd);
            return 0;
        }
    }

    pa->base = mmap(NULL, total_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, pa->fd, 0);
    if (pa->base == MAP_FAILED) {
        close(pa->fd);
        return 0;
    }

    pa->header = (Persistant_Arena_Header *)pa->base;
    pa->data = (u8 *)pa->base + sizeof(Persistant_Arena_Header);

    if (is_new) {
        pa->header->magic = PERSISTANT_ARENA_MAGIC;
        pa->header->capacity = (u64)capacity;
        pa->header->count = 0;
    } else if (pa->header->magic != PERSISTANT_ARENA_MAGIC) {
        /* real corrupt/wrong file - do not silently treat as empty and
         * do not silently keep going with data this code doesn't
         * actually understand the layout of. */
        munmap(pa->base, total_bytes);
        close(pa->fd);
        return 0;
    }

    return 1;
}

void *persistant_arena_alloc(Persistant_Arena *pa, size_t size_bytes)
{
    void *result;

    if (pa->header->count + size_bytes > pa->header->capacity) {
        /* real, honest exhaustion - see this file's own doc comment on
         * why growth-by-remap is deliberately not attempted here. */
        return NULL;
    }

    result = pa->data + pa->header->count;
    pa->header->count += (u64)size_bytes;
    HOLYC_ENSURES(pa->header->count <= pa->header->capacity);
    return result;
}

size_t persistant_arena_offset(Persistant_Arena *pa, void *ptr)
{
    return (size_t)((u8 *)ptr - pa->data);
}

void *persistant_arena_at(Persistant_Arena *pa, size_t offset)
{
    PERSISTANT_ARENA_ASSERT(offset <= pa->header->capacity);
    return pa->data + offset;
}

void persistant_arena_sync(Persistant_Arena *pa)
{
    size_t total_bytes = sizeof(Persistant_Arena_Header) + (size_t)pa->header->capacity;
    msync(pa->base, total_bytes, MS_SYNC);
}

void persistant_arena_close(Persistant_Arena *pa)
{
    size_t total_bytes = sizeof(Persistant_Arena_Header) + (size_t)pa->header->capacity;
    persistant_arena_sync(pa);
    munmap(pa->base, total_bytes);
    close(pa->fd);
    pa->base = NULL;
    pa->header = NULL;
    pa->data = NULL;
    pa->fd = -1;
}

#endif /* PERSISTANT_ARENA_IMPLEMENTATION_ */
#endif /* PERSISTANT_ARENA_IMPLEMENTATION */
