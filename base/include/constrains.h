#ifndef HOLYC_CONSTRAINS_H_
#define HOLYC_CONSTRAINS_H_

/* Distance constraints for verlet.h's Verlet_Point - the standard
 * companion to Verlet integration: individually-stable free points
 * become rope/cloth/soft-body simulations once pairs of them are held
 * a fixed distance apart and the constraint is iteratively re-satisfied
 * every step (the more iterations per step, the stiffer/more accurate
 * the result - a real, deliberate accuracy/cost tradeoff left to the
 * caller, not baked in here as a fixed count).
 *
 * `#include "verlet.h"` is a real, functional dependency (not
 * accidental coupling) - this file exists specifically to operate on
 * Verlet_Point, the same way constrains.h and verlet.h are meant to be
 * used together in any real rope/cloth setup. */

#include "base.h"
#include "mathx.h"
#include "verlet.h"

typedef struct {
    Verlet_Point *a;
    Verlet_Point *b;
    f32 rest_length;
} Verlet_Distance_Constraint;

Verlet_Distance_Constraint verlet_distance_constraint_make(Verlet_Point *a, Verlet_Point *b, f32 rest_length);

/* one relaxation pass: pushes a/b along their connecting line so the
 * distance between them moves toward rest_length, splitting the
 * correction by INVERSE mass (a heavier point, smaller inv_mass, moves
 * less) - the standard mass-weighted Verlet constraint technique. Safe
 * to call when both points are pinned (inv_mass both 0) or when the
 * points already coincide (zero-length delta) - both are real,
 * checked-for edge cases, not just assumed away. */
void constraint_satisfy(Verlet_Distance_Constraint *c);

#endif /* HOLYC_CONSTRAINS_H_ */

#ifdef CONSTRAINS_IMPLEMENTATION
#ifndef CONSTRAINS_IMPLEMENTATION_
#define CONSTRAINS_IMPLEMENTATION_

Verlet_Distance_Constraint verlet_distance_constraint_make(Verlet_Point *a, Verlet_Point *b, f32 rest_length)
{
    Verlet_Distance_Constraint c;
    c.a = a;
    c.b = b;
    c.rest_length = rest_length;
    return c;
}

void constraint_satisfy(Verlet_Distance_Constraint *c)
{
    vec3 delta;
    f32 dist, diff, total_inv_mass, a_share, b_share;
    vec3 dir;

    total_inv_mass = c->a->inv_mass + c->b->inv_mass;
    if (total_inv_mass == 0.0f) return; /* both pinned - nothing can move */

    delta = vec3_sub(c->b->pos, c->a->pos);
    dist = vec3_len(delta);
    if (dist == 0.0f) return; /* coincident points - no well-defined direction to push along */

    dir = vec3_scale(delta, 1.0f / dist);
    diff = dist - c->rest_length;

    a_share = c->a->inv_mass / total_inv_mass;
    b_share = c->b->inv_mass / total_inv_mass;

    c->a->pos = vec3_add(c->a->pos, vec3_scale(dir, diff * a_share));
    c->b->pos = vec3_sub(c->b->pos, vec3_scale(dir, diff * b_share));
}

#endif /* CONSTRAINS_IMPLEMENTATION_ */
#endif /* CONSTRAINS_IMPLEMENTATION */
