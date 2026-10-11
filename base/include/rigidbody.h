#ifndef HOLYC_RIGIDBODY_H_
#define HOLYC_RIGIDBODY_H_

/* A minimal rigid body - POINT MASS ONLY, no rotational/angular
 * dynamics. Stated plainly, not silently dropped: a full rigid body
 * (angular velocity, torque, an inertia tensor, orientation coupling
 * position and rotation together) is a real, separate, much bigger
 * undertaking - this file is deliberately scoped to translation only,
 * a genuinely useful building block on its own (projectiles, simple
 * AI-controlled movers, camera physics) without pretending to be a
 * full physics engine.
 *
 * Integrated via semi-implicit (symplectic) Euler, not explicit Euler -
 * a deliberate choice, not laziness: semi-implicit Euler updates
 * velocity FIRST (from the applied force), then uses that ALREADY-
 * UPDATED velocity to move position, which is unconditionally more
 * stable than explicit Euler (which uses the OLD velocity for both) for
 * the same reason symplectic integrators are the standard choice in
 * real-time physics - it approximately conserves energy over many
 * steps instead of systematically gaining it. Verlet integration
 * (verlet.h) is this project's OTHER integration scheme, used instead
 * wherever iterative distance constraints are involved (constrains.h) -
 * a point mass with no constraints doesn't need Verlet's implicit-
 * velocity trick, so semi-implicit Euler (simpler, explicit velocity
 * field a caller can read directly) is the better fit here. */

#include "base.h"
#include "mathx.h"

typedef struct {
    vec3 position;
    vec3 velocity;
    f32 inv_mass; /* 0 = immovable */
} Rigidbody;

/* mass == 0 is treated as "immovable" (inv_mass set to 0), matching
 * verlet.h's own inv_mass=0 pinning convention - not a divide-by-zero
 * footgun, a deliberate, checked special case. */
Rigidbody rigidbody_make(vec3 position, f32 mass);

/* F = m*a -> a = F * inv_mass; accumulates directly into velocity
 * (velocity += a * dt) - does NOT touch position, see
 * rigidbody_integrate for that. Call once per force source per step
 * before integrating, so multiple simultaneous forces (gravity + drag
 * + thrust, say) can each call this once and sum naturally. */
void rigidbody_apply_force(Rigidbody *b, vec3 force, f32 dt);

/* position += velocity * dt - uses whatever velocity already holds
 * (after any rigidbody_apply_force calls this step), the second half
 * of semi-implicit Euler. */
void rigidbody_integrate(Rigidbody *b, f32 dt);

#endif /* HOLYC_RIGIDBODY_H_ */

#ifdef RIGIDBODY_IMPLEMENTATION
#ifndef RIGIDBODY_IMPLEMENTATION_
#define RIGIDBODY_IMPLEMENTATION_

Rigidbody rigidbody_make(vec3 position, f32 mass)
{
    Rigidbody b;
    b.position = position;
    b.velocity = vec3_make(0.0f, 0.0f, 0.0f);
    b.inv_mass = (mass == 0.0f) ? 0.0f : (1.0f / mass);
    return b;
}

void rigidbody_apply_force(Rigidbody *b, vec3 force, f32 dt)
{
    vec3 accel = vec3_scale(force, b->inv_mass);
    b->velocity = vec3_add(b->velocity, vec3_scale(accel, dt));
}

void rigidbody_integrate(Rigidbody *b, f32 dt)
{
    b->position = vec3_add(b->position, vec3_scale(b->velocity, dt));
}

#endif /* RIGIDBODY_IMPLEMENTATION_ */
#endif /* RIGIDBODY_IMPLEMENTATION */
