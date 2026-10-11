#ifndef HOLYC_EMMITER_H_
#define HOLYC_EMMITER_H_

/* A particle emitter: owns a FIXED-capacity, arena-allocated pool of
 * particle.h's Particle structs (no malloc/free per spawn - the whole
 * pool is allocated once, up front, via arena_alloc, matching this
 * project's no-malloc-in-library-code rule) and spawns into it with
 * randomized velocity/lifetime, drawn from src/math/random/rng.h's
 * PCG32 generator (`#include "rng.h"` is a real, functional dependency,
 * the same way particle.h genuinely depends on lerp.h rather than
 * reimplementing it).
 *
 * Free-slot tracking: a linear dead-slot scan over the fixed pool, not a
 * separate free-list-by-index. Chosen because particle.h's
 * particle_is_dead() is already the source of truth for "can this slot
 * be reused" (age >= max_lifetime, which a freshly zeroed - never
 * spawned - Particle satisfies for free, see particle.h's own comment
 * on this), so a free-list would just be a second, redundant structure
 * that could drift out of sync with the real per-particle state; a scan
 * over a capacity realistically in the hundreds/low-thousands is cheap
 * enough per frame that the extra bookkeeping isn't worth it. */

#include "base.h"
#include "mathx.h"
#include "arena_alloc.h"
#include "particle.h"
#include "rng.h"

typedef struct {
    Particle *particles;
    size_t capacity;
    vec3 position;
} Emmiter;

/* allocates the pool via arena_alloc (zeroed by the arena, so every slot
 * starts as a naturally-dead Particle - no init loop needed). */
void emmiter_init(Arena *a, Emmiter *e, size_t capacity, vec3 position);

/* linear-scans for a dead slot; if found, spawns a Particle into it at
 * the emitter's position, with lifetime randomized uniformly in
 * [min_lifetime, max_lifetime] and per-axis velocity randomized
 * uniformly between velocity_min/velocity_max (both via pcg32_f32),
 * returning 1. Returns 0 without touching anything if the pool is
 * already full. */
i32 emmiter_spawn(Emmiter *e, Rng *rng, f32 min_lifetime, f32 max_lifetime,
                   vec3 velocity_min, vec3 velocity_max,
                   vec3 color_start, vec3 color_end,
                   f32 size_start, f32 size_end);

/* calls particle_update on every slot in the pool - dead/never-spawned
 * slots absorb this harmlessly (their age just keeps climbing past an
 * already-satisfied is-dead check). */
void emmiter_update(Emmiter *e, vec3 acceleration, f32 dt);

#endif /* HOLYC_EMMITER_H_ */

#ifdef EMMITER_IMPLEMENTATION
#ifndef EMMITER_IMPLEMENTATION_
#define EMMITER_IMPLEMENTATION_

void emmiter_init(Arena *a, Emmiter *e, size_t capacity, vec3 position)
{
    e->particles = (Particle *)arena_alloc(a, capacity * sizeof(Particle));
    e->capacity = capacity;
    e->position = position;
}

i32 emmiter_spawn(Emmiter *e, Rng *rng, f32 min_lifetime, f32 max_lifetime,
                   vec3 velocity_min, vec3 velocity_max,
                   vec3 color_start, vec3 color_end,
                   f32 size_start, f32 size_end)
{
    size_t i;

    for (i = 0; i < e->capacity; i++) {
        if (particle_is_dead(&e->particles[i])) {
            vec3 velocity;
            f32 lifetime;

            velocity.x = velocity_min.x + pcg32_f32(rng) * (velocity_max.x - velocity_min.x);
            velocity.y = velocity_min.y + pcg32_f32(rng) * (velocity_max.y - velocity_min.y);
            velocity.z = velocity_min.z + pcg32_f32(rng) * (velocity_max.z - velocity_min.z);
            lifetime = min_lifetime + pcg32_f32(rng) * (max_lifetime - min_lifetime);

            e->particles[i] = particle_spawn(e->position, velocity, lifetime,
                                              color_start, color_end,
                                              size_start, size_end);
            return 1;
        }
    }

    return 0;
}

void emmiter_update(Emmiter *e, vec3 acceleration, f32 dt)
{
    size_t i;

    for (i = 0; i < e->capacity; i++) {
        particle_update(&e->particles[i], acceleration, dt);
    }
}

#endif /* EMMITER_IMPLEMENTATION_ */
#endif /* EMMITER_IMPLEMENTATION */
