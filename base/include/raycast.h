#ifndef HOLYC_RAYCAST_H_
#define HOLYC_RAYCAST_H_

/* Ray-vs-shape intersection tests - three real, precise, well-known
 * algorithms, not approximations: the AABB "slab" test, the ray-sphere
 * quadratic, and Moller-Trumbore for ray-triangle. A ray is an origin
 * point plus a direction vector that is NOT required to be normalized
 * (the hit distance `t` is then measured in units of |direction|, same
 * convention every one of these reference algorithms uses natively -
 * a caller wanting hit distance in world units should normalize the
 * direction before calling, not something these functions do silently
 * on the caller's behalf). */

#include "base.h"
#include "mathx.h"

/* AABB "slab" method: intersect the ray against each axis-aligned pair
 * of planes and shrink [tmin,tmax] to their overlap. Returns 1 and
 * sets *out_t to the entry distance (0 if the origin is already
 * inside the box) on a hit, 0 otherwise. */
i32 ray_aabb(vec3 origin, vec3 dir, vec3 box_min, vec3 box_max, f32 *out_t);

/* solves the ray-sphere quadratic |origin + t*dir - center|^2 = r^2.
 * Returns 1 and sets *out_t to the nearest non-negative root (the near
 * surface, or the far surface if the origin is inside the sphere) on
 * a hit, 0 if both roots are negative (sphere entirely behind the
 * ray) or the discriminant is negative (ray misses entirely). */
i32 ray_sphere(vec3 origin, vec3 dir, vec3 center, f32 radius, f32 *out_t);

/* Moller-Trumbore ray-triangle intersection - the standard, precise
 * algorithm (not the naive plane-intersect-then-barycentric-check
 * approach, which needs an extra plane-normal computation this one
 * avoids). Returns 1 and sets *out_t to the hit distance on a genuine
 * hit (inside the triangle, in front of the ray origin), 0 otherwise
 * (including the ray lying in the triangle's own plane - a real,
 * separate degenerate case, not folded into a generic "miss"). */
i32 ray_triangle(vec3 origin, vec3 dir, vec3 v0, vec3 v1, vec3 v2, f32 *out_t);

#endif /* HOLYC_RAYCAST_H_ */

#ifdef RAYCAST_IMPLEMENTATION
#ifndef RAYCAST_IMPLEMENTATION_
#define RAYCAST_IMPLEMENTATION_

/* Newton-Raphson sqrt, replacing math.h's - own local copy (see
 * src/base.h's own vector_sqrt_/quat_sqrt_/matrix_sqrt_ for the
 * precedent and algorithm) - part of this project's "0 glibc" push. */
static f64 raycast_sqrt_(f64 x)
{
    f64 guess;
    i32 i;
    if (x <= 0.0) return 0.0;
    guess = (x > 1.0) ? x : 1.0;
    for (i = 0; i < 40; i++) {
        f64 next = 0.5 * (guess + x / guess);
        if (next == guess) break;
        guess = next;
    }
    return guess;
}

#ifndef RAYCAST_EPSILON_
#define RAYCAST_EPSILON_ 0.0000001f
#endif

i32 ray_aabb(vec3 origin, vec3 dir, vec3 box_min, vec3 box_max, f32 *out_t)
{
    f32 tmin = -1.0e30f;
    f32 tmax = 1.0e30f;

    /* one unrolled block per axis (always exactly 3, same "no loop over
       a fixed, compile-time-known bound" convention matrix.h uses) -
       each axis either narrows [tmin,tmax] via the two slab planes, or
       (when the ray is parallel to that axis) fails immediately if the
       origin already lies outside the slab on that axis. */
    if (dir.x != 0.0f) {
        f32 t1 = (box_min.x - origin.x) / dir.x;
        f32 t2 = (box_max.x - origin.x) / dir.x;
        f32 near = t1 < t2 ? t1 : t2;
        f32 far = t1 < t2 ? t2 : t1;
        if (near > tmin) tmin = near;
        if (far < tmax) tmax = far;
    } else if (origin.x < box_min.x || origin.x > box_max.x) {
        return 0;
    }

    if (dir.y != 0.0f) {
        f32 t1 = (box_min.y - origin.y) / dir.y;
        f32 t2 = (box_max.y - origin.y) / dir.y;
        f32 near = t1 < t2 ? t1 : t2;
        f32 far = t1 < t2 ? t2 : t1;
        if (near > tmin) tmin = near;
        if (far < tmax) tmax = far;
    } else if (origin.y < box_min.y || origin.y > box_max.y) {
        return 0;
    }

    if (dir.z != 0.0f) {
        f32 t1 = (box_min.z - origin.z) / dir.z;
        f32 t2 = (box_max.z - origin.z) / dir.z;
        f32 near = t1 < t2 ? t1 : t2;
        f32 far = t1 < t2 ? t2 : t1;
        if (near > tmin) tmin = near;
        if (far < tmax) tmax = far;
    } else if (origin.z < box_min.z || origin.z > box_max.z) {
        return 0;
    }

    if (tmax < tmin || tmax < 0.0f) return 0;

    *out_t = tmin >= 0.0f ? tmin : 0.0f;
    return 1;
}

i32 ray_sphere(vec3 origin, vec3 dir, vec3 center, f32 radius, f32 *out_t)
{
    vec3 oc = vec3_sub(origin, center);
    f32 a = vec3_dot(dir, dir);
    f32 b = 2.0f * vec3_dot(oc, dir);
    f32 c = vec3_dot(oc, oc) - radius * radius;
    f32 disc = b * b - 4.0f * a * c;
    f32 sqrt_disc, t0, t1;

    if (disc < 0.0f) return 0;

    sqrt_disc = (f32)raycast_sqrt_((f64)disc);
    t0 = (-b - sqrt_disc) / (2.0f * a);
    t1 = (-b + sqrt_disc) / (2.0f * a);

    if (t0 >= 0.0f) {
        *out_t = t0;
        return 1;
    }
    if (t1 >= 0.0f) {
        *out_t = t1;
        return 1;
    }
    return 0;
}

i32 ray_triangle(vec3 origin, vec3 dir, vec3 v0, vec3 v1, vec3 v2, f32 *out_t)
{
    vec3 edge1 = vec3_sub(v1, v0);
    vec3 edge2 = vec3_sub(v2, v0);
    vec3 h = vec3_cross(dir, edge2);
    f32 a = vec3_dot(edge1, h);
    f32 f, u, v, t;
    vec3 s, q;

    if (a > -RAYCAST_EPSILON_ && a < RAYCAST_EPSILON_) return 0; /* ray parallel to the triangle's plane */

    f = 1.0f / a;
    s = vec3_sub(origin, v0);
    u = f * vec3_dot(s, h);
    if (u < 0.0f || u > 1.0f) return 0;

    q = vec3_cross(s, edge1);
    v = f * vec3_dot(dir, q);
    if (v < 0.0f || u + v > 1.0f) return 0;

    t = f * vec3_dot(edge2, q);
    if (t <= RAYCAST_EPSILON_) return 0; /* behind the ray origin, or right at it */

    *out_t = t;
    return 1;
}

#endif /* RAYCAST_IMPLEMENTATION_ */
#endif /* RAYCAST_IMPLEMENTATION */
