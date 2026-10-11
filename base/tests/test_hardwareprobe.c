#include "test_common.h"
#define VECTOR_IMPLEMENTATION
#include "base.h"
#define HARDWAREPROBE_IMPLEMENTATION
#include "hardwareprobe.c"
#define ARENA_ALLOC_IMPLEMENTATION
#include "arena_alloc.h"
#define POOL_ALLOC_IMPLEMENTATION
#include "pool_alloc.c"

int main(void)
{
    Hardware_Info hw;

    print_cstr("test_hardwareprobe\n");

    hardwareprobe_detect(&hw);
    TEST_CHECK(hw.cpu_count >= 1, "cpu_count is always at least 1, never 0 or negative");
    TEST_CHECK(hw.has_avx2 == 0 || hw.has_avx2 == 1, "has_avx2 is a real boolean, not garbage");
    TEST_CHECK(hw.has_sse2 == 0 || hw.has_sse2 == 1, "has_sse2 is a real boolean, not garbage");

    {
        size_t cap = hardwareprobe_recommended_arena_capacity(&hw);
        TEST_CHECK(cap >= HARDWAREPROBE_ARENA_CAP_FLOOR, "arena capacity recommendation never goes below the real floor");
        TEST_CHECK(cap <= HARDWAREPROBE_ARENA_CAP_CEILING, "arena capacity recommendation never exceeds the real ceiling");
    }

    {
        /* a real, working region actually allocated at the recommended size - not just a number nobody exercises */
        size_t cap = hardwareprobe_recommended_arena_capacity(&hw);
        Arena_Region *r = new_region(cap);
        TEST_CHECK(r != NULL, "new_region() accepts the recommended capacity and succeeds");
        TEST_CHECK(r->capacity >= cap, "the region's real capacity covers at least what was requested (the default FLAT backend over-reserves address space on purpose, so it can legitimately be larger, never smaller)");
        free_region(r);
    }

    {
        Hardware_Info zero_ram;
        zero_ram.cpu_count = 1;
        zero_ram.total_ram_bytes = 0;
        zero_ram.has_avx2 = 0;
        zero_ram.has_sse2 = 0;
        TEST_CHECK(hardwareprobe_recommended_arena_capacity(&zero_ram) == HARDWAREPROBE_ARENA_CAP_FLOOR,
                   "undetectable RAM (0) falls back to the floor, not a crash or garbage value");
    }

    {
        size_t count = hardwareprobe_recommended_pool_chunk_count(&hw, sizeof(i32));
        u8 *buffer;
        Pool p;
        i32 *a, *b;

        TEST_CHECK(count >= HARDWAREPROBE_POOL_COUNT_FLOOR, "pool chunk count recommendation never goes below the real floor");
        TEST_CHECK(count <= HARDWAREPROBE_POOL_COUNT_CEILING, "pool chunk count recommendation never exceeds the real ceiling");

        /* a real, working pool actually sized from this recommendation */
        buffer = (u8 *)malloc(count * sizeof(i32));
        pool_init(&p, buffer, count * sizeof(i32), sizeof(i32));
        a = (i32 *)pool_alloc(&p);
        b = (i32 *)pool_alloc(&p);
        TEST_CHECK(a != NULL && b != NULL && a != b, "the hardware-sized pool actually hands out real, distinct chunks");
        free(buffer);
    }

    {
        size_t count = hardwareprobe_recommended_pool_chunk_count(&hw, 0);
        TEST_CHECK(count == HARDWAREPROBE_POOL_COUNT_FLOOR, "a zero chunk_size (division-by-zero risk) falls back to the floor instead of crashing");
    }

    TEST_CHECK(hardwareprobe_recommended_thread_count(&hw) == hw.cpu_count, "thread count recommendation matches detected cpu_count when positive");
    {
        Hardware_Info fake;
        fake.cpu_count = 0;
        TEST_CHECK(hardwareprobe_recommended_thread_count(&fake) == 1, "a detected cpu_count of 0 still recommends at least 1 thread");
    }

    print_flush();
    return test_failures_ != 0;
}
