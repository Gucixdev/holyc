#include "test_common.h"
#define VECTOR_IMPLEMENTATION
#define ARENA_ALLOC_IMPLEMENTATION
#define BROADPHASE_IMPLEMENTATION
#include "base.h"
#include "arena_alloc.h"
#include "broadphase.h"

static i32 has_pair_(Broadphase_Pair *pairs, size_t n, i64 a, i64 b)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if ((pairs[i].id_a == a && pairs[i].id_b == b) ||
            (pairs[i].id_a == b && pairs[i].id_b == a)) {
            return 1;
        }
    }
    return 0;
}

int main(void)
{
    Arena arena = {0};

    print_cstr("test_broadphase\n");

    /* 4 boxes along the x-axis: 1-2 overlap, 3-4 overlap, no other pair does */
    {
        Broadphase_Object objects[4];
        Broadphase_Pair pairs[16];
        size_t n_pairs;

        objects[0].id = 1; objects[0].min = vec3_make(0.0f, 0.0f, 0.0f); objects[0].max = vec3_make(2.0f, 1.0f, 1.0f);
        objects[1].id = 2; objects[1].min = vec3_make(1.0f, 0.0f, 0.0f); objects[1].max = vec3_make(3.0f, 1.0f, 1.0f);
        objects[2].id = 3; objects[2].min = vec3_make(5.0f, 0.0f, 0.0f); objects[2].max = vec3_make(7.0f, 1.0f, 1.0f);
        objects[3].id = 4; objects[3].min = vec3_make(6.0f, 0.0f, 0.0f); objects[3].max = vec3_make(8.0f, 1.0f, 1.0f);

        n_pairs = broadphase_sweep_and_prune(&arena, objects, 4, pairs, 16);

        TEST_CHECK(n_pairs == 2, "broadphase_sweep_and_prune finds exactly 2 overlapping pairs among 4 boxes");
        TEST_CHECK(has_pair_(pairs, n_pairs, 1, 2), "broadphase_sweep_and_prune finds the (1,2) overlap");
        TEST_CHECK(has_pair_(pairs, n_pairs, 3, 4), "broadphase_sweep_and_prune finds the (3,4) overlap");
        TEST_CHECK(!has_pair_(pairs, n_pairs, 2, 3), "broadphase_sweep_and_prune does NOT report the non-overlapping (2,3) pair");
    }

    /* objects overlapping in x but not in y are correctly excluded (a real
       sweep-and-prune bug class: sorting/pruning on x alone isn't enough,
       the final result must still be a genuine full-AABB overlap test) */
    {
        Broadphase_Object objects[2];
        Broadphase_Pair pairs[4];
        size_t n_pairs;

        objects[0].id = 10; objects[0].min = vec3_make(0.0f, 0.0f, 0.0f); objects[0].max = vec3_make(5.0f, 1.0f, 1.0f);
        objects[1].id = 20; objects[1].min = vec3_make(1.0f, 10.0f, 0.0f); objects[1].max = vec3_make(4.0f, 11.0f, 1.0f);

        n_pairs = broadphase_sweep_and_prune(&arena, objects, 2, pairs, 4);
        TEST_CHECK(n_pairs == 0, "broadphase_sweep_and_prune excludes boxes that overlap in x but not y, even though x-sweep alone would suggest a candidate pair");
    }

    /* no objects, no pairs, no crash */
    {
        Broadphase_Pair pairs[4];
        size_t n_pairs = broadphase_sweep_and_prune(&arena, NULL, 0, pairs, 4);
        TEST_CHECK(n_pairs == 0, "broadphase_sweep_and_prune with zero objects returns 0 pairs, not a crash");
    }

    /* the original objects array is untouched (not silently reordered) */
    {
        Broadphase_Object objects[3];
        Broadphase_Pair pairs[8];

        objects[0].id = 100; objects[0].min = vec3_make(9.0f, 0.0f, 0.0f); objects[0].max = vec3_make(9.0f, 0.0f, 0.0f);
        objects[1].id = 200; objects[1].min = vec3_make(0.0f, 0.0f, 0.0f); objects[1].max = vec3_make(0.0f, 0.0f, 0.0f);
        objects[2].id = 300; objects[2].min = vec3_make(5.0f, 0.0f, 0.0f); objects[2].max = vec3_make(5.0f, 0.0f, 0.0f);

        broadphase_sweep_and_prune(&arena, objects, 3, pairs, 8);
        TEST_CHECK(objects[0].id == 100 && objects[1].id == 200 && objects[2].id == 300,
                   "broadphase_sweep_and_prune does not reorder the caller's own objects array");
    }

    arena_free(&arena);
    print_flush();
    return test_failures_ != 0;
}
