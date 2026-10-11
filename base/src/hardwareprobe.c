#ifndef HOLYC_HARDWAREPROBE_C_
#define HOLYC_HARDWAREPROBE_C_

/* Real hardware capability probe - detects CPU core count, physical
 * RAM, and SIMD support at runtime, then turns that into sane sizing
 * recommendations for arena_alloc.h/pool_alloc.c callers ("sprawdza
 * hardware i dostosowuje areny/poole do niego" - checks hardware and
 * adjusts arenas/pools to it, per this file's own original braindump
 * note). Deliberately does NOT touch arena_alloc.h/pool_alloc.c
 * themselves - both new_region() and pool_init() already take an
 * explicit size parameter per call, so this file's whole job is
 * recommending a real NUMBER a caller can pass in, not adding new
 * global state or a hidden default override to either allocator.
 *
 * SIMD detection is its own small, direct __builtin_cpu_supports
 * check here rather than depending on base.h's own avx_math_available/
 * sse_math_available (real, same-technique functions, see base.h's
 * AVX_MATH_IMPLEMENTATION/SSE_MATH_IMPLEMENTATION) - those only exist
 * when a caller defines those specific IMPLEMENTATION guards before
 * including base.h, an extra required-coupling this file avoids by
 * duplicating one real, tiny check instead. This is exactly the
 * "fallback if the CPU doesn't support it" story - has_avx2/has_sse2 tell a caller whether it's safe to
 * reach for base.h's own SIMD functions at all, real runtime
 * detection, not a compile-time assumption. */

#include "base.h"
#include <unistd.h>

typedef struct {
    i32 cpu_count;
    i64 total_ram_bytes; /* 0 if undetectable (sysconf failed) - real callers should treat that as "assume nothing" */
    i32 has_avx2;
    i32 has_sse2;
} Hardware_Info;

void hardwareprobe_detect(Hardware_Info *hw);

/* recommended new_region() capacity, in u64 elements (matching
 * Arena_Region.capacity's own real units, see arena_alloc.h) - scales
 * with available RAM, real bounded on both ends: never below
 * arena_alloc.h's own ARENA_REGION_DEFAULT_CAPACITY floor (8K u64 =
 * 64KB - going smaller just means more, smaller region allocations,
 * no real benefit), never above a real 8MB-of-u64s ceiling (so one
 * region can't dominate memory even on a huge-RAM machine - more
 * regions chaining is fine, arena_alloc.h is built for that). */
size_t hardwareprobe_recommended_arena_capacity(const Hardware_Info *hw);

/* recommended pool_init() chunk COUNT for a fixed chunk_size (the
 * pool's own buffer_size is chunk_count * chunk_size, still the
 * caller's own multiplication - this file only recommends the count,
 * since "how many objects" is what actually depends on hardware,
 * not the object's own byte size). Same floor/ceiling shape as the
 * arena recommendation above, scaled independently since a pool's
 * per-chunk_size differs per real caller. */
size_t hardwareprobe_recommended_pool_chunk_count(const Hardware_Info *hw, size_t chunk_size);

/* recommended worker/thread-pool count - real cpu_count, clamped to
 * at least 1 (a detection failure or a single-core container must
 * still get a valid, usable answer, not 0). */
i32 hardwareprobe_recommended_thread_count(const Hardware_Info *hw);

#endif /* HOLYC_HARDWAREPROBE_C_ */

#ifdef HARDWAREPROBE_IMPLEMENTATION
#ifndef HARDWAREPROBE_IMPLEMENTATION_
#define HARDWAREPROBE_IMPLEMENTATION_

#define HARDWAREPROBE_ARENA_CAP_FLOOR ((size_t)8 * 1024)        /* matches arena_alloc.h's own ARENA_REGION_DEFAULT_CAPACITY */
#define HARDWAREPROBE_ARENA_CAP_CEILING ((size_t)1024 * 1024)   /* 8MB worth of u64s */
#define HARDWAREPROBE_POOL_COUNT_FLOOR ((size_t)64)
#define HARDWAREPROBE_POOL_COUNT_CEILING ((size_t)65536)

void hardwareprobe_detect(Hardware_Info *hw)
{
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGE_SIZE);

    hw->cpu_count = cores > 0 ? (i32)cores : 1;
    hw->total_ram_bytes = (pages > 0 && page_size > 0) ? (i64)pages * (i64)page_size : 0;

#if defined(__x86_64__) || defined(__i386__)
    hw->has_avx2 = __builtin_cpu_supports("avx2") ? 1 : 0;
    hw->has_sse2 = __builtin_cpu_supports("sse2") ? 1 : 0;
#else
    hw->has_avx2 = 0;
    hw->has_sse2 = 0;
#endif
}

size_t hardwareprobe_recommended_arena_capacity(const Hardware_Info *hw)
{
    size_t scaled;

    if (hw->total_ram_bytes <= 0) return HARDWAREPROBE_ARENA_CAP_FLOOR;

    /* real, simple rule: one region grows to roughly 1/10000th of
     * total RAM before a caller needs a new one - keeps region count
     * reasonable without any single region claiming a large fraction
     * of available memory. */
    scaled = (size_t)(hw->total_ram_bytes / 10000) / sizeof(u64);
    if (scaled < HARDWAREPROBE_ARENA_CAP_FLOOR) return HARDWAREPROBE_ARENA_CAP_FLOOR;
    if (scaled > HARDWAREPROBE_ARENA_CAP_CEILING) return HARDWAREPROBE_ARENA_CAP_CEILING;
    return scaled;
}

size_t hardwareprobe_recommended_pool_chunk_count(const Hardware_Info *hw, size_t chunk_size)
{
    size_t scaled;

    if (hw->total_ram_bytes <= 0 || chunk_size == 0) return HARDWAREPROBE_POOL_COUNT_FLOOR;

    /* same real budget as the arena recommendation (1/10000th of
     * RAM), divided by this pool's own real chunk_size instead of a
     * fixed element size, since a pool's objects can be any size. */
    scaled = (size_t)(hw->total_ram_bytes / 10000) / chunk_size;
    if (scaled < HARDWAREPROBE_POOL_COUNT_FLOOR) return HARDWAREPROBE_POOL_COUNT_FLOOR;
    if (scaled > HARDWAREPROBE_POOL_COUNT_CEILING) return HARDWAREPROBE_POOL_COUNT_CEILING;
    return scaled;
}

i32 hardwareprobe_recommended_thread_count(const Hardware_Info *hw)
{
    return hw->cpu_count > 0 ? hw->cpu_count : 1;
}

#endif /* HARDWAREPROBE_IMPLEMENTATION_ */
#endif /* HARDWAREPROBE_IMPLEMENTATION */
