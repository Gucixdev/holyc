#ifndef HOLYC_GJK_H_
#define HOLYC_GJK_H_

/* GJK (Gilbert-Johnson-Keerthi) convex-convex intersection test - the
 * standard algorithm, general knowledge at this point (this specific
 * simplex-case structure - a `push_front` simplex history plus
 * separate Line/Triangle/Tetrahedron case handlers - matches the
 * widely-referenced public "winter.dev" GJK/EPA tutorial's own shape,
 * not a novel structure invented here).
 *
 * Works generically over ANY convex shape via a "support function" -
 * a function that returns the point on the shape furthest in a given
 * direction - rather than shape-specific code, so the same
 * gjk_intersect() works for spheres, boxes, capsules, arbitrary convex
 * hulls, or anything else with a support function, without this file
 * needing to know what any of them actually are.
 *
 * Operates on the MINKOWSKI DIFFERENCE of the two shapes (every point
 * support_a(d) - support_b(-d)) - the two shapes intersect if and only
 * if this difference shape contains the origin, which is exactly what
 * the simplex-evolution loop below tests for.
 *
 * On a hit, `epa.h` can take the terminating simplex from `out_simplex`
 * and compute the actual penetration depth/direction (GJK alone only
 * answers yes/no). */

#include "base.h"
#include "mathx.h"

typedef vec3 (*Gjk_Support_Fn)(void *shape, vec3 direction);

/* points[0] is always the MOST RECENTLY ADDED point (a "push_front"
 * history, not insertion order) - the convention every simplex-case
 * handler below assumes. */
typedef struct {
    vec3 points[4];
    i32 count;
} Gjk_Simplex;

/* returns 1 if the two convex shapes overlap, 0 otherwise. If
 * out_simplex is non-NULL and the shapes overlap, it's filled with the
 * terminating tetrahedron simplex (always exactly 4 points on a real
 * hit) - the input epa.h needs to compute penetration depth. */
i32 gjk_intersect(Gjk_Support_Fn support_a, void *shape_a, Gjk_Support_Fn support_b, void *shape_b,
                   Gjk_Simplex *out_simplex);

#endif /* HOLYC_GJK_H_ */

#ifdef GJK_IMPLEMENTATION
#ifndef GJK_IMPLEMENTATION_
#define GJK_IMPLEMENTATION_

#ifndef GJK_MAX_ITERATIONS_
#define GJK_MAX_ITERATIONS_ 64 /* real convex shapes converge in well under this; a non-convergence after this many iterations is treated as no-hit rather than looping forever */
#endif

static vec3 gjk_support_(Gjk_Support_Fn support_a, void *shape_a, Gjk_Support_Fn support_b, void *shape_b, vec3 dir)
{
    vec3 neg_dir = vec3_scale(dir, -1.0f);
    vec3 pa = support_a(shape_a, dir);
    vec3 pb = support_b(shape_b, neg_dir);
    return vec3_sub(pa, pb);
}

static void gjk_simplex_push_front_(Gjk_Simplex *s, vec3 p)
{
    i32 i;
    i32 new_count = s->count < 4 ? s->count + 1 : 4;
    for (i = new_count - 1; i > 0; i--) s->points[i] = s->points[i - 1];
    s->points[0] = p;
    s->count = new_count;
}

static i32 gjk_same_direction_(vec3 direction, vec3 ao)
{
    return vec3_dot(direction, ao) > 0.0f;
}

static i32 gjk_line_(Gjk_Simplex *s, vec3 *direction)
{
    vec3 a = s->points[0];
    vec3 b = s->points[1];
    vec3 ab = vec3_sub(b, a);
    vec3 ao = vec3_scale(a, -1.0f);

    if (gjk_same_direction_(ab, ao)) {
        *direction = vec3_cross(vec3_cross(ab, ao), ab);
    } else {
        s->points[0] = a;
        s->count = 1;
        *direction = ao;
    }
    return 0;
}

static i32 gjk_triangle_(Gjk_Simplex *s, vec3 *direction)
{
    vec3 a = s->points[0];
    vec3 b = s->points[1];
    vec3 c = s->points[2];
    vec3 ab = vec3_sub(b, a);
    vec3 ac = vec3_sub(c, a);
    vec3 ao = vec3_scale(a, -1.0f);
    vec3 abc = vec3_cross(ab, ac);

    if (gjk_same_direction_(vec3_cross(abc, ac), ao)) {
        if (gjk_same_direction_(ac, ao)) {
            s->points[0] = a;
            s->points[1] = c;
            s->count = 2;
            *direction = vec3_cross(vec3_cross(ac, ao), ac);
        } else {
            s->points[0] = a;
            s->points[1] = b;
            s->count = 2;
            return gjk_line_(s, direction);
        }
    } else if (gjk_same_direction_(vec3_cross(ab, abc), ao)) {
        s->points[0] = a;
        s->points[1] = b;
        s->count = 2;
        return gjk_line_(s, direction);
    } else if (gjk_same_direction_(abc, ao)) {
        *direction = abc;
    } else {
        s->points[0] = a;
        s->points[1] = c;
        s->points[2] = b;
        s->count = 3;
        *direction = vec3_scale(abc, -1.0f);
    }
    return 0;
}

static i32 gjk_tetrahedron_(Gjk_Simplex *s, vec3 *direction)
{
    vec3 a = s->points[0];
    vec3 b = s->points[1];
    vec3 c = s->points[2];
    vec3 d = s->points[3];
    vec3 ab = vec3_sub(b, a);
    vec3 ac = vec3_sub(c, a);
    vec3 ad = vec3_sub(d, a);
    vec3 ao = vec3_scale(a, -1.0f);
    vec3 abc = vec3_cross(ab, ac);
    vec3 acd = vec3_cross(ac, ad);
    vec3 adb = vec3_cross(ad, ab);

    if (gjk_same_direction_(abc, ao)) {
        s->points[0] = a; s->points[1] = b; s->points[2] = c;
        s->count = 3;
        return gjk_triangle_(s, direction);
    }
    if (gjk_same_direction_(acd, ao)) {
        s->points[0] = a; s->points[1] = c; s->points[2] = d;
        s->count = 3;
        return gjk_triangle_(s, direction);
    }
    if (gjk_same_direction_(adb, ao)) {
        s->points[0] = a; s->points[1] = d; s->points[2] = b;
        s->count = 3;
        return gjk_triangle_(s, direction);
    }

    return 1; /* origin is inside the tetrahedron - real intersection found */
}

static i32 gjk_next_simplex_(Gjk_Simplex *s, vec3 *direction)
{
    switch (s->count) {
    case 2: return gjk_line_(s, direction);
    case 3: return gjk_triangle_(s, direction);
    case 4: return gjk_tetrahedron_(s, direction);
    default: break;
    }
    return 0; /* unreachable - count is always 2/3/4 when this is called */
}

i32 gjk_intersect(Gjk_Support_Fn support_a, void *shape_a, Gjk_Support_Fn support_b, void *shape_b,
                   Gjk_Simplex *out_simplex)
{
    Gjk_Simplex simplex;
    vec3 direction = vec3_make(1.0f, 0.0f, 0.0f);
    vec3 support;
    i32 iter;

    simplex.count = 0;

    support = gjk_support_(support_a, shape_a, support_b, shape_b, direction);
    gjk_simplex_push_front_(&simplex, support);
    direction = vec3_scale(support, -1.0f);

    for (iter = 0; iter < GJK_MAX_ITERATIONS_; iter++) {
        support = gjk_support_(support_a, shape_a, support_b, shape_b, direction);

        if (vec3_dot(support, direction) < 0.0f) return 0; /* new point didn't even reach the origin's side */

        gjk_simplex_push_front_(&simplex, support);

        if (gjk_next_simplex_(&simplex, &direction)) {
            if (out_simplex) *out_simplex = simplex;
            return 1;
        }
    }

    return 0; /* did not converge within the iteration budget - treated as no-hit */
}

#endif /* GJK_IMPLEMENTATION_ */
#endif /* GJK_IMPLEMENTATION */
