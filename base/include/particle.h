#ifndef HOLYC_PARTICLE_H_
#define HOLYC_PARTICLE_H_

/* A single particle for simple VFX-style effects (sparks, smoke, debris) -
 * position/velocity integrated via plain semi-implicit Euler (see
 * rigidbody.h for the same choice and its reasoning; a particle has no
 * constraints and doesn't need Verlet's implicit-velocity tracking), plus
 * an age/max_lifetime pair that both marks when the particle dies and
 * drives a lerp of its color and size from a start value to an end value
 * over its life.
 *
 * The lerp itself is NOT reimplemented here - lerpf/vec3_lerp already
 * exist in lerp.h and are used directly (`#include "easing.h"` below is a
 * real, functional dependency), the same way constrains.h genuinely
 * depends on verlet.h rather than duplicating its logic. */

#include "base.h"
#include "mathx.h"
#include "easing.h"

typedef struct {
    vec3 position;
    vec3 velocity;
    f32 age;
    f32 max_lifetime;
    vec3 color_start;
    vec3 color_end;
    f32 size_start;
    f32 size_end;
} Particle;

/* age starts at 0. */
Particle particle_spawn(vec3 position, vec3 velocity, f32 max_lifetime,
                         vec3 color_start, vec3 color_end,
                         f32 size_start, f32 size_end);

/* semi-implicit Euler: velocity += acceleration*dt, then position +=
 * velocity*dt (gravity/drag are just whatever acceleration the caller
 * passes in - not baked in here); age += dt. */
void particle_update(Particle *p, vec3 acceleration, f32 dt);

/* age >= max_lifetime. Note: a zeroed (arena-allocated, never-spawned)
 * Particle has age==0 and max_lifetime==0, so this is true for it too -
 * emmiter.h relies on exactly this to find free pool slots without a
 * separate alive flag. */
i32 particle_is_dead(const Particle *p);

/* vec3_lerp(color_start, color_end, age/max_lifetime), from lerp.h. */
vec3 particle_current_color(const Particle *p);

/* lerpf(size_start, size_end, age/max_lifetime), from lerp.h. */
f32 particle_current_size(const Particle *p);

#endif /* HOLYC_PARTICLE_H_ */

#ifdef PARTICLE_IMPLEMENTATION
#ifndef PARTICLE_IMPLEMENTATION_
#define PARTICLE_IMPLEMENTATION_

Particle particle_spawn(vec3 position, vec3 velocity, f32 max_lifetime,
                         vec3 color_start, vec3 color_end,
                         f32 size_start, f32 size_end)
{
    Particle p;
    p.position = position;
    p.velocity = velocity;
    p.age = 0.0f;
    p.max_lifetime = max_lifetime;
    p.color_start = color_start;
    p.color_end = color_end;
    p.size_start = size_start;
    p.size_end = size_end;
    return p;
}

void particle_update(Particle *p, vec3 acceleration, f32 dt)
{
    p->velocity = vec3_add(p->velocity, vec3_scale(acceleration, dt));
    p->position = vec3_add(p->position, vec3_scale(p->velocity, dt));
    p->age += dt;
}

i32 particle_is_dead(const Particle *p)
{
    return p->age >= p->max_lifetime;
}

vec3 particle_current_color(const Particle *p)
{
    f32 t = (p->max_lifetime == 0.0f) ? 1.0f : (p->age / p->max_lifetime);
    return vec3_lerp(p->color_start, p->color_end, t);
}

f32 particle_current_size(const Particle *p)
{
    f32 t = (p->max_lifetime == 0.0f) ? 1.0f : (p->age / p->max_lifetime);
    return lerpf(p->size_start, p->size_end, t);
}

#endif /* PARTICLE_IMPLEMENTATION_ */
#endif /* PARTICLE_IMPLEMENTATION */
