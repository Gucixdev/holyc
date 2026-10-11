#ifndef HOLYC_VERLET_H_
#define HOLYC_VERLET_H_

/* Verlet integration - tracks a point's CURRENT and PREVIOUS position
 * instead of position+velocity; the difference (pos - prev_pos) IS the
 * implicit velocity, so there's no separate velocity field to keep in
 * sync. Picked over explicit/semi-implicit Euler (see rigidbody.h,
 * which uses semi-implicit Euler instead, for a point-mass body with
 * no constraints) specifically because Verlet integration is
 * unconditionally stable under iterative distance-constraint solving
 * (see constrains.h) - the standard reason it's the textbook technique
 * for rope/cloth/soft-body simulation, where explicit Euler would
 * accumulate energy and blow up under repeated constraint correction.
 *
 * inv_mass (inverse mass, 0 = immovable/pinned) rather than mass - the
 * standard trick that avoids a separate boolean AND a divide-by-mass at
 * every integration step: multiplying by inv_mass=0 is the pin, no
 * branch needed at the integration call site itself (verlet_integrate
 * below does still need one guard - a pinned point must not update
 * prev_pos either, or it would silently un-pin itself the next step
 * by seeing its own fixed position as if it were newly-arrived-at). */

#include "base.h"
#include "mathx.h"

typedef struct {
    vec3 pos;
    vec3 prev_pos;
    f32 inv_mass; /* 0 = immovable */
} Verlet_Point;

Verlet_Point verlet_point_make(vec3 pos, f32 inv_mass);

/* one Verlet integration step: pos_new = pos + (pos - prev_pos)*damping
 * + acceleration*dt*dt. damping in (0,1] models energy loss (air
 * resistance / internal friction) each step; 1.0 = none. A pinned
 * point (inv_mass == 0) is left completely untouched. */
void verlet_integrate(Verlet_Point *p, vec3 acceleration, f32 dt, f32 damping);

#endif /* HOLYC_VERLET_H_ */

#ifdef VERLET_IMPLEMENTATION
#ifndef VERLET_IMPLEMENTATION_
#define VERLET_IMPLEMENTATION_

Verlet_Point verlet_point_make(vec3 pos, f32 inv_mass)
{
    Verlet_Point p;
    p.pos = pos;
    p.prev_pos = pos;
    p.inv_mass = inv_mass;
    return p;
}

void verlet_integrate(Verlet_Point *p, vec3 acceleration, f32 dt, f32 damping)
{
    vec3 velocity_term, accel_term, new_pos;
    vec3 old_pos;

    if (p->inv_mass == 0.0f) return; /* pinned - do not move, do not touch prev_pos either */

    old_pos = p->pos;
    velocity_term = vec3_scale(vec3_sub(p->pos, p->prev_pos), damping);
    accel_term = vec3_scale(acceleration, dt * dt);
    new_pos = vec3_add(vec3_add(p->pos, velocity_term), accel_term);

    p->pos = new_pos;
    p->prev_pos = old_pos;
}

#endif /* VERLET_IMPLEMENTATION_ */
#endif /* VERLET_IMPLEMENTATION */
