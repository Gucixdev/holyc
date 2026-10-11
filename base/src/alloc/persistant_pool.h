#ifndef HOLYC_PERSISTANT_POOL_H_
#define HOLYC_PERSISTANT_POOL_H_

/* pool_alloc.c's own fixed-chunk free-list pool, backed by a real file
 * via mmap(MAP_SHARED, ...) instead of a caller-supplied in-memory
 * buffer - both the chunk CONTENT and which chunks are actually
 * allocated survive a process restart.
 *
 * The one real, non-obvious problem persistence adds that pool_alloc.c
 * never has to solve: pool_alloc.c's own Pool_Free_Node stores a real
 * `next` POINTER inside each free chunk, which is completely safe there
 * because that pool only ever lives in ONE process's ONE address space
 * for its entire life. A file-backed mapping has no such guarantee -
 * mmap() gives no base-address promise, so the SAME file reopened later
 * (even in the very same process) can legitimately land at a different
 * virtual address, silently turning every stored `next` pointer into
 * garbage. The real fix, used throughout this file: the free list is
 * chained by 1-based CHUNK INDEX (0 means "no next"), never by pointer -
 * an index means the exact same thing regardless of where the file
 * happens to be mapped this time, the same offset-not-pointer discipline
 * persistant_arena.h's own persistant_arena_offset/at exist for.
 *
 * Also unlike pool_alloc.c's own pool_init (which always calls
 * pool_free_all - a fresh pool is always "everything free", state that
 * only ever needs to exist once per process run): persistant_pool_open
 * only builds that initial free list ONCE, the first time a path is
 * created. Reopening an existing pool file trusts the REAL persisted
 * free_head - re-running pool_free_all's own "mark everything free"
 * logic on reopen would silently forget which chunks a previous session
 * had already allocated and never freed, which is exactly the state a
 * "persistent" pool exists to keep in the first place. */

#include "types/base.h"
#include "types/contracts.h"

/* See persistant_arena.h's own identical note: ftruncate/msync are
 * POSIX.1-2001, not strict ISO C89 - compile with
 * -D_POSIX_C_SOURCE=200112L (or -std=gnu89) under -std=c89. */

#ifndef PERSISTANT_POOL_ASSERT
#define PERSISTANT_POOL_ASSERT HOLYC_ASSERT
#endif

typedef struct {
    u64 magic;
    u64 chunk_size;   /* real, ALIGNED chunk size actually used - see persistant_pool_open */
    u64 chunk_count;
    u64 free_head;    /* 1-based chunk index of the first free chunk, 0 = none free */
} Persistant_Pool_Header;

#define PERSISTANT_POOL_MAGIC ((u64)0x484f4c594350504cull) /* "HOLYCPPL" as big-endian bytes, arbitrary but stable and distinct from PERSISTANT_ARENA_MAGIC */

typedef struct {
    int fd;
    void *base;
    Persistant_Pool_Header *header;
    u8 *chunks;   /* base + sizeof(Persistant_Pool_Header) - chunk 0 starts here */
} Persistant_Pool;

/* persistant_pool_open - creates `path` (chunk_count chunks of
 * chunk_size bytes, aligned up to sizeof(u64) the same way
 * pool_alloc.c's own pool_init does) if it doesn't exist yet, otherwise
 * reopens it and trusts the file's OWN persisted chunk_size/chunk_count/
 * free_head over whatever this call's arguments say (see this file's
 * own doc comment on why re-deriving free_head from scratch on reopen
 * would be wrong) - real per-call values are only real requirements the
 * FIRST time a path is created. Returns 1 on success, 0 on any real
 * failure (open/ftruncate/mmap failed, or an existing file's magic
 * doesn't match). */
int persistant_pool_open(Persistant_Pool *pp, const char *path, size_t chunk_size, size_t chunk_count);
void *persistant_pool_alloc(Persistant_Pool *pp);
void persistant_pool_dealloc(Persistant_Pool *pp, void *ptr);

void persistant_pool_sync(Persistant_Pool *pp);
void persistant_pool_close(Persistant_Pool *pp);

#endif /* HOLYC_PERSISTANT_POOL_H_ */

#ifdef PERSISTANT_POOL_IMPLEMENTATION
#ifndef PERSISTANT_POOL_IMPLEMENTATION_
#define PERSISTANT_POOL_IMPLEMENTATION_

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static size_t persistant_pool_align_up_(size_t n, size_t align)
{
    return (n + align - 1) & ~(align - 1); /* align must be a power of two, same as pool_align_up */
}

static u8 *persistant_pool_chunk_at_(Persistant_Pool *pp, u64 index_1based)
{
    return pp->chunks + (index_1based - 1) * pp->header->chunk_size;
}

/* persistant_pool_build_free_list_ - links every chunk into the free
 * list in forward order (chunk 1 first) - real, deliberate departure
 * from pool_alloc.c's own pool_free_all, which links in REVERSE order
 * (walks the buffer forward, pushing each onto the head, so chunk 0
 * ends up LAST in the list). Forward order here isn't load-bearing for
 * correctness (either order is a valid free list), just easier to
 * reason about when inspecting a fresh file's header by hand. Only
 * ever called once, when a brand-new file is created - see this file's
 * own top comment on why reopening must NOT re-run this. */
static void persistant_pool_build_free_list_(Persistant_Pool *pp)
{
    u64 i;
    for (i = 1; i <= pp->header->chunk_count; i++) {
        u64 *next_slot = (u64 *)persistant_pool_chunk_at_(pp, i);
        *next_slot = (i < pp->header->chunk_count) ? (i + 1) : 0;
    }
    pp->header->free_head = 1;
}

int persistant_pool_open(Persistant_Pool *pp, const char *path, size_t chunk_size, size_t chunk_count)
{
    struct stat st;
    size_t aligned_chunk_size, total_bytes;
    int is_new;

    aligned_chunk_size = persistant_pool_align_up_(chunk_size, sizeof(u64));
    PERSISTANT_POOL_ASSERT(aligned_chunk_size >= sizeof(u64)); /* every chunk must fit a real free-list index */

    pp->fd = open(path, O_RDWR | O_CREAT, 0644);
    if (pp->fd < 0) return 0;

    if (fstat(pp->fd, &st) != 0) {
        close(pp->fd);
        return 0;
    }

    is_new = (st.st_size == 0);
    total_bytes = is_new
        ? sizeof(Persistant_Pool_Header) + aligned_chunk_size * chunk_count
        : (size_t)st.st_size;

    if (is_new) {
        if (ftruncate(pp->fd, (off_t)total_bytes) != 0) {
            close(pp->fd);
            return 0;
        }
    }

    pp->base = mmap(NULL, total_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, pp->fd, 0);
    if (pp->base == MAP_FAILED) {
        close(pp->fd);
        return 0;
    }

    pp->header = (Persistant_Pool_Header *)pp->base;
    pp->chunks = (u8 *)pp->base + sizeof(Persistant_Pool_Header);

    if (is_new) {
        pp->header->magic = PERSISTANT_POOL_MAGIC;
        pp->header->chunk_size = (u64)aligned_chunk_size;
        pp->header->chunk_count = (u64)chunk_count;
        persistant_pool_build_free_list_(pp);
    } else if (pp->header->magic != PERSISTANT_POOL_MAGIC) {
        munmap(pp->base, total_bytes);
        close(pp->fd);
        return 0;
    }

    return 1;
}

void *persistant_pool_alloc(Persistant_Pool *pp)
{
    u64 index;
    u8 *chunk;

    index = pp->header->free_head;
    if (index == 0) return NULL; /* real, honest "pool out of chunks" - same as pool_alloc's own POOL_ASSERT case, but never fatal here: a persisted pool being full is a real, expected steady state, not a programming error */

    chunk = persistant_pool_chunk_at_(pp, index);
    pp->header->free_head = *(u64 *)chunk; /* the stored next-index becomes the new head */

    HOLYC_ENSURES(chunk >= pp->chunks && chunk + pp->header->chunk_size <= pp->chunks + pp->header->chunk_count * pp->header->chunk_size);
    return chunk;
}

void persistant_pool_dealloc(Persistant_Pool *pp, void *ptr)
{
    u64 index;
    u8 *chunk = (u8 *)ptr;

    if (!ptr) return;

    /* real chunk index, computed from a real offset - never store `ptr`
     * itself, see this file's own top comment. */
    index = (u64)((chunk - pp->chunks) / pp->header->chunk_size) + 1;
    *(u64 *)chunk = pp->header->free_head;
    pp->header->free_head = index;
}

void persistant_pool_sync(Persistant_Pool *pp)
{
    size_t total_bytes = sizeof(Persistant_Pool_Header) + (size_t)pp->header->chunk_size * (size_t)pp->header->chunk_count;
    msync(pp->base, total_bytes, MS_SYNC);
}

void persistant_pool_close(Persistant_Pool *pp)
{
    size_t total_bytes = sizeof(Persistant_Pool_Header) + (size_t)pp->header->chunk_size * (size_t)pp->header->chunk_count;
    persistant_pool_sync(pp);
    munmap(pp->base, total_bytes);
    close(pp->fd);
    pp->base = NULL;
    pp->header = NULL;
    pp->chunks = NULL;
    pp->fd = -1;
}

#endif /* PERSISTANT_POOL_IMPLEMENTATION_ */
#endif /* PERSISTANT_POOL_IMPLEMENTATION */
