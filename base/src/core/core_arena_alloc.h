#ifndef HOLYC_ARENA_ALLOC_H_
#define HOLYC_ARENA_ALLOC_H_

/* Port of tsoding's arena.h (https://github.com/tsoding/arena) to plain C89:
 *   - no <stdint.h> (own base_type.h u64 instead of uintptr_t)
 *   - no flexible array member (C99) -> offsetof() struct-hack instead
 *   - no mixed declarations / inline for-loop decls
 *   - no C++ interop cruft (this is not going to touch a C++ compiler)
 *   - WIN32/WASM backends dropped for now, add back when actually needed
 *   - no heap: the libc malloc backend is gone; LINUX_MMAP and FLAT (the default) take memory from
 *     the kernel, and arena_snapshot/arena_rewind around a scope free everything allocated in it
 */

#define HOLYC_ARENA_ALLOC_IN_PROGRESS_ /* core_base.h must not re-enter this file */
#include "core/core_base.h"
#include "core/core_contracts.h"
#undef HOLYC_ARENA_ALLOC_IN_PROGRESS_
#include <stdarg.h> /* va_list is part of arena_vsprintf's declaration below, not implementation-only */
#include <string.h> /* memcpy - arena_memcpy's underlying primitive */

#ifndef ARENA_ASSERT
#define ARENA_ASSERT HOLYC_ASSERT
#endif

#define ARENA_BACKEND_LINUX_MMAP  1
#define ARENA_BACKEND_FLAT        2

#ifndef ARENA_BACKEND
#define ARENA_BACKEND ARENA_BACKEND_FLAT
#endif

typedef struct Arena_Region Arena_Region;

struct Arena_Region {
    Arena_Region *next;
    size_t count;
    size_t capacity;
    size_t high; /* with count: max(high, count) granules were ever handed out here; data above that is still
                  * zero from mmap. Kept up to date when count drops (rewind, reset), so arena_alloc never
                  * touches it. */
    u64 data[1]; /* struct-hack: real allocation is offsetof(Arena_Region, data) + capacity*sizeof(u64) */
};

struct Arena { /* `typedef struct Arena Arena;` is in core_base.h (Arena_String needs it) */
    Arena_Region *begin, *end;
};

typedef struct {
    Arena_Region *region;
    size_t count;
} Arena_Mark;

#ifndef ARENA_REGION_DEFAULT_CAPACITY
#define ARENA_REGION_DEFAULT_CAPACITY ((size_t)8 * 1024) /* size_t, not int, so any future edit to this macro can't silently overflow int before widening */
#endif

Arena_Region *new_region(size_t capacity);
void free_region(Arena_Region *r);

void *arena_alloc(Arena *a, size_t size_bytes);

/* alignof(type) with no <stdalign.h> (C11): the classic struct-offset
 * trick. A compiler must pad `d` up to its natural alignment right after
 * the single `char`, so that padding amount IS alignof(type). Seen while
 * surveying vendored reference libraries (arena-allocator-main, Carter
 * Dugan, Apache-2.0) - worth lifting on its own even though the allocator
 * it came from isn't (no growth, no bounds checking - silently overruns
 * past whatever size you gave it, which this project's own arena already
 * does better without). */
#define ARENA_ALIGNOF(type) offsetof(struct { char c_; type d_; }, d_)

/* returns a block of size_bytes whose address is a multiple of `align`
 * (align must be a power of 2 - true of every real alignment requirement,
 * including ARENA_ALIGNOF's own output). Wastes up to align-1 bytes doing
 * it - the arena has no way to hand back the slack since it never frees
 * individual allocations anyway, same slack-for-simplicity tradeoff as
 * arena_da_append's capacity doubling. Exists for SIMD-width-aligned
 * buffers (aligned loads are cheaper than unaligned ones, e.g.
 * vecindex.h's AVX2 kernel currently uses _mm256_loadu_ps specifically
 * because nothing guaranteed 32-byte alignment before this existed). */
void *arena_alloc_aligned(Arena *a, size_t size_bytes, size_t align);

void *arena_realloc(Arena *a, void *oldptr, size_t oldsz, size_t newsz);
size_t arena_strlen(const char *s);
void *arena_memcpy(void *dest, const void *src, size_t n);
char *arena_strdup(Arena *a, const char *cstr);
void *arena_memdup(Arena *a, void *data, size_t size);

#ifndef ARENA_NOSTDIO
char *arena_sprintf(Arena *a, const char *format, ...);
char *arena_vsprintf(Arena *a, const char *format, va_list args);
#endif

Arena_Mark arena_snapshot(Arena *a);
void arena_reset(Arena *a);
void arena_rewind(Arena *a, Arena_Mark m);
void arena_free(Arena *a);
void arena_trim(Arena *a);

void *arena_calloc(Arena *a, size_t size_bytes);       /* zeroed; memory never handed out is not written */
void arena_decommit(Arena *a);                         /* pages above the current position back to the kernel */
size_t arena_used(const Arena *a);                     /* bytes handed out, 8-byte granules */
size_t arena_reserved(const Arena *a);                 /* bytes held in regions */


#ifndef ARENA_DA_INIT_CAP
#define ARENA_DA_INIT_CAP 256
#endif

/* void* -> T* needs no cast in C (unlike C++), so no cast_ptr wrapper here */

#define arena_da_append(a, da, item)                                                         \
    do {                                                                                      \
        if ((da)->count >= (da)->capacity) {                                                  \
            size_t new_capacity = (da)->capacity == 0 ? ARENA_DA_INIT_CAP : (da)->capacity*2; \
            (da)->items = arena_realloc(                                                      \
                (a), (da)->items,                                                             \
                (da)->capacity*sizeof(*(da)->items),                                          \
                new_capacity*sizeof(*(da)->items));                                           \
            (da)->capacity = new_capacity;                                                    \
        }                                                                                      \
        (da)->items[(da)->count++] = (item);                                                  \
    } while (0)

#define arena_da_append_many(a, da, new_items, new_items_count)                                       \
    do {                                                                                              \
        if ((da)->count + (new_items_count) > (da)->capacity) {                                       \
            size_t new_capacity = (da)->capacity;                                                     \
            if (new_capacity == 0) new_capacity = ARENA_DA_INIT_CAP;                                  \
            while ((da)->count + (new_items_count) > new_capacity) new_capacity *= 2;                 \
            (da)->items = arena_realloc(                                                              \
                (a), (da)->items,                                                                     \
                (da)->capacity*sizeof(*(da)->items),                                                  \
                new_capacity*sizeof(*(da)->items));                                                   \
            (da)->capacity = new_capacity;                                                            \
        }                                                                                              \
        arena_memcpy((da)->items + (da)->count, (new_items), (new_items_count)*sizeof(*(da)->items)); \
        (da)->count += (new_items_count);                                                             \
    } while (0)

#define arena_sb_append_buf arena_da_append_many

#define arena_sb_append_cstr(a, sb, cstr)      \
    do {                                       \
        const char *s_ = (cstr);               \
        size_t n_ = arena_strlen(s_);          \
        arena_da_append_many(a, sb, s_, n_);   \
    } while (0)

#define arena_sb_append_null(a, sb) arena_da_append(a, sb, 0)

#endif /* HOLYC_ARENA_ALLOC_H_ */

#ifdef ARENA_ALLOC_IMPLEMENTATION
#ifndef ARENA_ALLOC_IMPLEMENTATION_
#define ARENA_ALLOC_IMPLEMENTATION_

#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h> /* sysconf */


#if ARENA_BACKEND == ARENA_BACKEND_LINUX_MMAP


Arena_Region *new_region(size_t capacity)
{
    Arena_Region *r;
    size_t size_bytes = offsetof(Arena_Region, data) + sizeof(u64) * capacity;
    r = (Arena_Region *)mmap(NULL, size_bytes, PROT_READ | PROT_WRITE,
                        MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    ARENA_ASSERT(r != MAP_FAILED);
    r->next = NULL;
    r->count = 0;
    r->high = 0;
    r->capacity = capacity;
    return r;
}

void free_region(Arena_Region *r)
{
    size_t size_bytes = offsetof(Arena_Region, data) + sizeof(u64) * r->capacity;
    int ret = munmap(r, size_bytes);
    ARENA_ASSERT(ret == 0);
}

#elif ARENA_BACKEND == ARENA_BACKEND_FLAT

/* "pamiec jako jedna wielka tablica totalnie w stylu terrego" - the region
 * chasing (fixed vs geometric, how big should the next malloc be, does
 * this size class warm glibc's free-list) was never really the interesting
 * question; it only existed because every new_region() call handed memory
 * management back to glibc. This backend hands it back to the kernel
 * instead, once: reserve one huge chunk of VIRTUAL address space up front
 * (MAP_NORESERVE - Linux backs it with real RAM/swap lazily, page by page,
 * only where something actually gets written), then never call mmap()
 * again in the overwhelmingly common case. arena_alloc()'s existing
 * region-growth loop is completely unchanged - it just so happens the
 * first "region" is already bigger than any realistic arena will ever
 * need, so growth code exists only as a safety net, not a hot path. No
 * glibc heap, no free-list warm-up effects, no size-class bookkeeping -
 * closer to the flat-array ideal than a linked list of glibc-managed
 * chunks could ever be. */


#ifndef ARENA_FLAT_RESERVE_BYTES
#define ARENA_FLAT_RESERVE_BYTES ((size_t)1 << 30) /* 1GB of address space, reserved not committed */
#endif

#ifndef ARENA_HUGE_BYTES
#define ARENA_HUGE_BYTES ((size_t)2 << 20) /* an x86-64 transparent huge page */
#endif

Arena_Region *new_region(size_t capacity)
{
    Arena_Region *r;
    u8 *m;
    size_t requested_bytes = offsetof(Arena_Region, data) + sizeof(u64) * capacity;
    size_t size_bytes = requested_bytes > ARENA_FLAT_RESERVE_BYTES ? requested_bytes : ARENA_FLAT_RESERVE_BYTES;

    /* Reserve 2 MiB more, start the region on a 2 MiB boundary and hand the slack back, so that past its
     * first 2 MiB the region can sit on transparent huge pages (MADV_HUGEPAGE): one page fault and one TLB
     * entry per 2 MiB instead of 512. Writing 512 MiB for the first time took 69-79 ms that way against
     * 190-300 ms on 4 KiB pages (2 vCPU Xeon, kernel 6.18, THP in madvise mode, 2026-11-06). The first
     * 2 MiB stay on 4 KiB pages, so an arena that stays small never holds a 2 MiB page. */
    size_bytes = (size_bytes + ARENA_HUGE_BYTES - 1) & ~(ARENA_HUGE_BYTES - 1);
    m = (u8 *)mmap(NULL, size_bytes + ARENA_HUGE_BYTES, PROT_READ | PROT_WRITE,
                   MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
    if (m != (u8 *)MAP_FAILED) {
        u8 *p = (u8 *)(((size_t)m + ARENA_HUGE_BYTES - 1) & ~(ARENA_HUGE_BYTES - 1));
        if (p != m) munmap(m, (size_t)(p - m));
        if (p != m + ARENA_HUGE_BYTES) munmap(p + size_bytes, (size_t)(m + ARENA_HUGE_BYTES - p));
#ifdef MADV_HUGEPAGE
        madvise(p + ARENA_HUGE_BYTES, size_bytes - ARENA_HUGE_BYTES, MADV_HUGEPAGE);
#endif
        r = (Arena_Region *)p;
    } else {
        /* the over-reservation itself failed - not a real out-of-memory
         * condition (MAP_NORESERVE reservations are cheap and this
         * machine's overcommit=0 heuristic accepts them fine, but a
         * container with a tight memory cgroup or a system running
         * overcommit_memory=2 can legitimately refuse a 1GB ask). Retry
         * for exactly what this call actually needs before giving up -
         * ARENA_ASSERT below is a real hard failure only if even that
         * fails, not silently continuing with a MAP_FAILED pointer under
         * NDEBUG (assert() compiles out in release builds - dereferencing
         * r->next below on (void*)-1 would otherwise be a use-after-"success"
         * crash waiting for whichever machine has a tighter memory limit). */
        size_bytes = requested_bytes;
        r = (Arena_Region *)mmap(NULL, size_bytes, PROT_READ | PROT_WRITE,
                            MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
    }
    ARENA_ASSERT(r != MAP_FAILED);
    r->next = NULL;
    r->count = 0;
    r->high = 0;
    /* real capacity is the whole reservation, not just what this call
     * asked for - that's the entire point, over-reserve once */
    r->capacity = (size_bytes - offsetof(Arena_Region, data)) / sizeof(u64);
    return r;
}

void free_region(Arena_Region *r)
{
    size_t size_bytes = offsetof(Arena_Region, data) + sizeof(u64) * r->capacity;
    int ret = munmap(r, size_bytes);
    ARENA_ASSERT(ret == 0);
}

#else
#error "Unknown ARENA_BACKEND"
#endif

/* arena_alloc's slow path: the first region, a later region after a rewind, or a new one */
static void *arena_alloc_slow_(Arena *a, size_t size)
{
    void *result;

    if (size > ((size_t)-1 >> 4)) return NULL; /* no region holds it, and its byte size would wrap */

    if (a->end == NULL) {
        size_t capacity = ARENA_REGION_DEFAULT_CAPACITY;
        ARENA_ASSERT(a->begin == NULL);
        /* branchless max(capacity, size): selects size when capacity <
         * size, else keeps capacity - same XOR-mask select idiom used
         * in string_view.h's sv_chop_left/right clamp. */
        capacity ^= (capacity ^ size) & (size_t)-(size_t)(capacity < size);
        a->end = new_region(capacity);
        a->begin = a->end;
    }

    while (a->end->count + size > a->end->capacity && a->end->next != NULL) {
        a->end = a->end->next;
    }

    if (a->end->count + size > a->end->capacity) {
        /* Tried geometric growth here (double the previous region's
         * capacity) reasoning "fewer malloc() calls must be faster" -
         * measured with a benchmark that allocated but never wrote to the
         * memory, which showed a 2.7x win (8192 regions -> 14 for a 512MB
         * arena). That benchmark was misleading: pages that are never
         * touched may never actually get committed, so it wasn't measuring
         * real usage. Redone with every allocation actually written to
         * (memset), 5 trials: flat ARENA_REGION_DEFAULT_CAPACITY every time
         * was consistently ~40% FASTER than geometric growth (70-76ms vs
         * 120-124ms for the same 512MB workload), and a capped variant
         * (grow up to 1MB then stay flat) was worse than both (175-179ms).
         * The likely cause: ARENA_REGION_DEFAULT_CAPACITY (64KB) sits under
         * glibc's default mmap threshold (128KB) and stays on the fast
         * brk-extended heap; both geometric and capped-at-1MB cross that
         * threshold and pay a real per-region mmap() cost with different
         * page-fault behavior. So: still flat, on purpose, re-verified
         * rather than "obviously" switched to geometric. */
        size_t capacity = ARENA_REGION_DEFAULT_CAPACITY;
        ARENA_ASSERT(a->end->next == NULL);
        /* branchless max(capacity, size) - see arena_alloc's identical clamp */
        capacity ^= (capacity ^ size) & (size_t)-(size_t)(capacity < size);
        a->end->next = new_region(capacity);
        a->end = a->end->next;
    }

    result = &a->end->data[a->end->count];
    a->end->count += size;
    /* the exact "arena nie wysadza" property: this allocation never
     * pushed the active region's count past its own capacity - true
     * unconditionally here (not "usually"), since every path above
     * either found/made a region with enough room before ever
     * touching count. */
    HOLYC_ENSURES(a->end->count <= a->end->capacity);
    return result;
}

void *arena_alloc(Arena *a, size_t size_bytes)
{
    /* granules, rounded up without (size_bytes + 7) wrapping around near SIZE_MAX */
    size_t size = size_bytes / sizeof(u64) + (size_bytes % sizeof(u64) != 0);
    Arena_Region *r = a->end;
    if (r != NULL && size <= r->capacity - r->count) { /* room in the current region: a bump, nothing else */
        void *result = &r->data[r->count];
        r->count += size;
        return result;
    }
    return arena_alloc_slow_(a, size);
}

void *arena_alloc_aligned(Arena *a, size_t size_bytes, size_t align)
{
    size_t addr, aligned_addr;
    void *raw = arena_alloc(a, size_bytes + align - 1);
    addr = (size_t)raw;
    aligned_addr = (addr + align - 1) & ~(align - 1); /* align must be a power of 2 */
    return (void *)aligned_addr;
}

void *arena_realloc(Arena *a, void *oldptr, size_t oldsz, size_t newsz)
{
    Arena_Region *r = a->end;
    size_t og, ng;
    void *newptr;

    if (newsz <= oldsz) return oldptr;
    og = oldsz / sizeof(u64) + (oldsz % sizeof(u64) != 0);
    ng = newsz / sizeof(u64) + (newsz % sizeof(u64) != 0);
    /* The block is the region's last one and the region has room: it grows where it is, nothing is
     * copied. A growable array nothing else allocates after (arena_da_append) never moves. */
    if (oldptr != NULL && r != NULL && (u64 *)oldptr + og == &r->data[r->count] && ng - og <= r->capacity - r->count) {
        r->count += ng - og;
        return oldptr;
    }
    newptr = arena_alloc(a, newsz);
    if (newptr != NULL && oldsz > 0) arena_memcpy(newptr, oldptr, oldsz);
    return newptr;
}

size_t arena_strlen(const char *s)
{
    size_t n = 0;
    while (*s++) n++;
    return n;
}

void *arena_memcpy(void *dest, const void *src, size_t n)
{
    /* Measured, not assumed: a hand-rolled byte loop here benchmarks at
     * ~3-4 GB/s flat regardless of size on this machine, while glibc's
     * memcpy (runtime-dispatched to AVX2/AVX-512 + non-temporal stores
     * depending on size) hits 30-100+ GB/s - a 2x to 26x gap depending on
     * copy size. A hand-rolled word-at-a-time (u64) loop closes some of
     * that gap but still loses to libc by 2-4x. arena_memcpy is only a
     * name/API of ours - what runs underneath doesn't have to be reinvented
     * badly when the platform's own implementation measurably wins, same
     * pragmatic call already made for vsnprintf/mmap/sqrt elsewhere in this
     * file. This is on the hot path of every arena_realloc call, which is
     * every growable array in the codebase (arena_da_append, vecindex's
     * codes[] growth, ...). */
    return memcpy(dest, src, n);
}

char *arena_strdup(Arena *a, const char *cstr)
{
    size_t n = arena_strlen(cstr);
    char *dup = (char *)arena_alloc(a, n + 1);
    arena_memcpy(dup, cstr, n);
    dup[n] = '\0';
    return dup;
}

void *arena_memdup(Arena *a, void *data, size_t size)
{
    return arena_memcpy(arena_alloc(a, size), data, size);
}

#ifndef ARENA_NOSTDIO

/* va_copy is a C99 macro; under strict -std=c89 glibc's <stdarg.h> hides it,
 * so fall back to the gcc/clang builtin that is there regardless of -std=. */
#ifndef va_copy
#define va_copy(dst, src) __builtin_va_copy((dst), (src))
#endif

/* vsnprintf is not strict ISO C89 (it is C99/POSIX), but every libc that
 * matters here (glibc) has provided it forever regardless of -std= mode.
 * Compile with -D_POSIX_C_SOURCE=200112L (or -std=gnu89) to expose it under
 * -std=c89. "shut up and write code" beats pedantry on this one. */
char *arena_vsprintf(Arena *a, const char *format, va_list args)
{
    va_list args_copy;
    int n;
    char *result;

    va_copy(args_copy, args);
    n = vsnprintf(NULL, 0, format, args_copy);
    va_end(args_copy);

    ARENA_ASSERT(n >= 0);
    result = (char *)arena_alloc(a, (size_t)n + 1);
    vsnprintf(result, (size_t)n + 1, format, args);

    return result;
}

char *arena_sprintf(Arena *a, const char *format, ...)
{
    va_list args;
    char *result;

    va_start(args, format);
    result = arena_vsprintf(a, format, args);
    va_end(args);

    return result;
}
#endif /* ARENA_NOSTDIO */

Arena_Mark arena_snapshot(Arena *a)
{
    Arena_Mark m;
    ARENA_ASSERT(a->end != NULL || a->begin == NULL);
    m.region = a->end;
    m.count = (a->end == NULL) ? 0 : a->end->count; /* both branches only differed in this one field */
    return m;
}

/* count is about to drop: keep the most it has been, so arena_calloc knows what was handed out */
static void arena_keep_high_(Arena_Region *r)
{
    if (r->count > r->high) r->high = r->count;
}

void arena_reset(Arena *a)
{
    Arena_Region *r;
    for (r = a->begin; r != NULL; r = r->next) {
        arena_keep_high_(r);
        r->count = 0;
    }
    a->end = a->begin;
}

void arena_rewind(Arena *a, Arena_Mark m)
{
    Arena_Region *r;

    if (m.region == NULL) {
        arena_reset(a);
        return;
    }

    arena_keep_high_(m.region);
    m.region->count = m.count;
    for (r = m.region->next; r != NULL; r = r->next) {
        arena_keep_high_(r);
        r->count = 0;
    }
    a->end = m.region;
}

void arena_free(Arena *a)
{
    Arena_Region *r = a->begin;
    while (r) {
        Arena_Region *r0 = r;
        r = r->next;
        free_region(r0);
    }
    a->begin = NULL;
    a->end = NULL;
}

void arena_trim(Arena *a)
{
    Arena_Region *r = a->end->next;
    while (r) {
        Arena_Region *r0 = r;
        r = r->next;
        free_region(r0);
    }
    a->end->next = NULL;
}


void *arena_calloc(Arena *a, size_t size_bytes)
{
    u8 *p = (u8 *)arena_alloc(a, size_bytes);
    if (p != NULL) {
        /* p is in a->end. From max(high, count before this call) granules on, nothing was ever handed out:
         * still zero from mmap, so not written (or committed) here. Only the part of the block below the
         * high-water mark is cleared. */
        Arena_Region *r = a->end;
        size_t off = (size_t)(p - (u8 *)r->data), dirty = r->high * sizeof(u64);
        size_t n = (size_bytes / sizeof(u64) + (size_bytes % sizeof(u64) != 0)) * sizeof(u64);
        if (dirty > off) memset(p, 0, dirty - off < n ? dirty - off : n);
    }
    return p;
}

/* Pages wholly above each region's position go back to the kernel (MADV_DONTNEED): after a big scope is
 * rewound, resident memory drops to what is still in use. The pages read as zero again (high drops with
 * them) and are committed again when written. */
void arena_decommit(Arena *a)
{
    Arena_Region *r;
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    for (r = a->begin; r != NULL; r = r->next) {
        u8 *data = (u8 *)r->data;
        u8 *top = data + (r->high > r->count ? r->high : r->count) * sizeof(u64);
        u8 *from = (u8 *)(((size_t)(data + r->count * sizeof(u64)) + page - 1) & ~(page - 1));
        if (from < top && madvise(from, (size_t)(top - from), MADV_DONTNEED) == 0)
            r->high = (size_t)(from - data) / sizeof(u64);
    }
}

size_t arena_used(const Arena *a)
{
    const Arena_Region *r;
    size_t n = 0;
    for (r = a->begin; r != NULL; r = r->next) n += r->count * sizeof(u64);
    return n;
}

size_t arena_reserved(const Arena *a)
{
    const Arena_Region *r;
    size_t n = 0;
    for (r = a->begin; r != NULL; r = r->next) n += r->capacity * sizeof(u64);
    return n;
}


#endif /* ARENA_ALLOC_IMPLEMENTATION_ */
#endif /* ARENA_ALLOC_IMPLEMENTATION */

/* ---- Arena_String bodies (declared in core_base.h) ---- */
#ifdef ARENA_STRING_IMPLEMENTATION
#ifndef ARENA_STRING_IMPLEMENTATION_
#define ARENA_STRING_IMPLEMENTATION_


void arena_string_init(Arena_String *s, Arena *a)
{
    s->a = a;
    s->items = 0;
    s->count = 0;
    s->capacity = 0;
}

void arena_string_clear(Arena_String *s)
{
    s->count = 0;
}

void arena_string_append_char(Arena_String *s, char c)
{
    arena_da_append(s->a, s, c);
}

void arena_string_append_buf(Arena_String *s, const char *buf, size_t n)
{
    arena_da_append_many(s->a, s, buf, n);
}

void arena_string_append_cstr(Arena_String *s, const char *cstr)
{
    arena_string_append_buf(s, cstr, arena_strlen(cstr));
}

/* same two-digits-per-division trick print.h's print_u64_base(v,10) uses -
 * reimplemented here rather than reused, since print.h's version writes
 * straight to its own internal write(2) buffer with no way to redirect
 * output into an arbitrary caller buffer. */
static const char arena_string_digit_pairs_[] =
    "00010203040506070809"
    "10111213141516171819"
    "20212223242526272829"
    "30313233343536373839"
    "40414243444546474849"
    "50515253545556575859"
    "60616263646566676869"
    "70717273747576777879"
    "80818283848586878889"
    "90919293949596979899";

void arena_string_append_u64(Arena_String *s, u64 v)
{
    char tmp[24];
    size_t n = 0;

    if (v == 0) {
        arena_string_append_char(s, '0');
        return;
    }

    while (v >= 100) {
        u32 pair = (u32)(v % 100) * 2;
        v /= 100;
        tmp[n++] = arena_string_digit_pairs_[pair + 1];
        tmp[n++] = arena_string_digit_pairs_[pair];
    }
    if (v >= 10) {
        u32 pair = (u32)v * 2;
        tmp[n++] = arena_string_digit_pairs_[pair + 1];
        tmp[n++] = arena_string_digit_pairs_[pair];
    } else {
        tmp[n++] = (char)('0' + v);
    }

    while (n > 0) arena_string_append_char(s, tmp[--n]);
}

void arena_string_append_i64(Arena_String *s, i64 v)
{
    u64 uv;
    if (v < 0) {
        arena_string_append_char(s, '-');
        uv = (u64)(-(v + 1)) + 1; /* safe negation, avoids overflow at i64 min - same idiom as print.h */
    } else {
        uv = (u64)v;
    }
    arena_string_append_u64(s, uv);
}

const char *arena_string_cstr(Arena_String *s)
{
    size_t saved_count = s->count;
    arena_da_append(s->a, s, '\0');
    s->count = saved_count; /* NUL written but not counted, same convention arena_sb_append_null documents */
    return s->items;
}

#endif /* ARENA_STRING_IMPLEMENTATION_ */
#endif /* ARENA_STRING_IMPLEMENTATION */
