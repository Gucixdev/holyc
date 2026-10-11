#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#define VECTOR_IMPLEMENTATION
#define LERP_IMPLEMENTATION
#define PARTICLE_IMPLEMENTATION
#define RNG_IMPLEMENTATION
#define EMMITER_IMPLEMENTATION
#include "arena_alloc.h"
#include "base.h"
#include "easing.h"
#include "particle.h"
#include "rng.h"
#include "emmiter.h"

int main(void)
{
    Arena a = {0};
    Rng rng;
    Emmiter e;
    size_t i;
    size_t cap = 4;

    print_cstr("test_emmiter\n");

    pcg32_seed(&rng, 42, 0);
    emmiter_init(&a, &e, cap, vec3_make(0.0f, 0.0f, 0.0f));

    /* pool starts fully available: fill it exactly to capacity */
    {
        i32 spawned_count = 0;
        for (i = 0; i < cap; i++) {
            i32 ok = emmiter_spawn(&e, &rng, 1.0f, 2.0f,
                                    vec3_make(-1.0f, -1.0f, -1.0f), vec3_make(1.0f, 1.0f, 1.0f),
                                    vec3_make(1.0f, 1.0f, 1.0f), vec3_make(0.0f, 0.0f, 0.0f), 1.0f, 1.0f);
            if (ok) spawned_count++;
        }
        TEST_CHECK(spawned_count == (i32)cap, "emmiter_spawn: fills the pool exactly to its capacity");
    }

    /* pool is now full - the next spawn must fail, not overflow */
    {
        i32 ok = emmiter_spawn(&e, &rng, 1.0f, 2.0f,
                                vec3_make(0.0f, 0.0f, 0.0f), vec3_make(0.0f, 0.0f, 0.0f),
                                vec3_make(0.0f, 0.0f, 0.0f), vec3_make(0.0f, 0.0f, 0.0f), 1.0f, 1.0f);
        TEST_CHECK(!ok, "emmiter_spawn: fails cleanly (returns 0) once the pool is full, does not overflow");
    }

    /* age every particle to death, then verify their slots are reusable */
    {
        i32 reused = 0;
        emmiter_update(&e, vec3_make(0.0f, 0.0f, 0.0f), 3.0f); /* dt=3 > max lifetime of 2 for all */
        for (i = 0; i < cap; i++) {
            i32 ok = emmiter_spawn(&e, &rng, 1.0f, 2.0f,
                                    vec3_make(0.0f, 0.0f, 0.0f), vec3_make(0.0f, 0.0f, 0.0f),
                                    vec3_make(0.0f, 0.0f, 0.0f), vec3_make(0.0f, 0.0f, 0.0f), 1.0f, 1.0f);
            if (ok) reused++;
        }
        TEST_CHECK(reused == (i32)cap, "emmiter: dead particle slots are all reused for new spawns after aging out");
    }

    arena_free(&a);
    print_flush();
    return test_failures_ != 0;
}
