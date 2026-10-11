#ifndef HOLYC_GEOMETRY_H_
#define HOLYC_GEOMETRY_H_

/* Bounded 3D shape primitives, one file, per the single-file convention - previously src/gamedev/physic/shapes/{capsule,
 * plane,sphere,sphere_collision,triangle}.h, merged back here. Each shape
 * still stands alone under its OWN IMPLEMENTATION guard macro (CAPSULE_/
 * PLANE_/SPHERE_IMPLEMENTATION, unchanged from the split files) - a
 * caller/test wanting only one shape still only pulls in that one's code,
 * this file just puts them in one place on disk. sphere_collision.h was
 * an empty not-yet-written stub, folded in here with no content to carry
 * over. */

#include "base.h"
#include "mathx.h"

/* ---- sphere ----
 * Bounding sphere - center + radius, nothing else. sphere_from_points
 * uses the naive (centroid, then radius = max distance to any point)
 * construction, NOT a tight-bounding-sphere algorithm - a documented
 * gap, not silently missing: Ritter's algorithm (a fast approximate
 * bound) or Welzl's algorithm (the real minimal-enclosing-sphere, via
 * randomized incremental construction) both produce meaningfully
 * tighter spheres for non-symmetric point sets, at real extra
 * implementation cost this file doesn't spend yet. */

typedef struct {
    vec3 center;
    f32 radius;
} Sphere;

/* naive bound: centroid of all points, radius = distance to the
 * farthest one. Correct (every point IS within radius of center) but
 * not tight for non-symmetric point sets - see file header. */
Sphere sphere_from_points(const vec3 *points, size_t count);

i32 sphere_contains_point(Sphere s, vec3 p);
i32 sphere_intersects_sphere(Sphere a, Sphere b);

/* ---- plane ----
 * 3D plane, standard normal+distance form: a unit normal plus the
 * signed distance from the origin along that normal, so a point p lies
 * on the plane exactly when dot(normal, p) + d == 0 - the compact form
 * most collision/culling code actually uses (vs. storing a normal plus
 * an arbitrary point on the plane, which needs a dot product to get to
 * this form anyway before it's useful for distance tests). */

typedef struct {
    vec3 normal; /* unit length */
    f32 d;       /* dot(normal, any point on the plane) + d == 0 */
} Plane;

typedef enum {
    PLANE_BACK  = -1,
    PLANE_ON    =  0,
    PLANE_FRONT =  1
} Plane_Side;

#ifndef PLANE_EPSILON
#define PLANE_EPSILON 0.0001f
#endif

Plane plane_from_points(vec3 a, vec3 b, vec3 c);
Plane plane_from_normal_point(vec3 normal, vec3 point);
f32 plane_signed_distance(Plane p, vec3 point);
Plane_Side plane_classify(Plane p, vec3 point);

/* ---- triangle ----
 * 3D triangle - 3 vec3 vertices, plus the standard derived quantities
 * (normal, area, centroid, barycentric coordinates). Barycentric
 * coordinates are computed via the standard dot-product formulation
 * (project the point onto the triangle's own edge basis), not the
 * cross-product/area-ratio formulation - fewer sqrt/cross calls for
 * the same result, the common choice in real-time collision code. */

typedef struct {
    vec3 a, b, c;
} Triangle;

vec3 triangle_normal(Triangle t);   /* unit length, right-hand rule from a->b->c winding */
f32 triangle_area(Triangle t);
vec3 triangle_centroid(Triangle t);

/* barycentric coordinates (u,v,w) of point p with respect to t, such
 * that p == u*t.a + v*t.b + w*t.c and u+v+w == 1 for any p in the
 * triangle's own plane (not clamped to the triangle itself - a point
 * outside the triangle still gets valid barycentric coords, just with
 * one or more negative). */
void triangle_barycentric(Triangle t, vec3 p, f32 *out_u, f32 *out_v, f32 *out_w);

/* 1 if p (assumed already in the triangle's own plane) lies within the
 * triangle, 0 otherwise - via barycentric coords all being >= 0. */
i32 triangle_contains_point(Triangle t, vec3 p);

/* ---- capsule ----
 * Capsule - a "swept sphere": a line segment (a,b) plus a radius, the
 * shape traced by sweeping a sphere of that radius along the segment.
 * Every function reduces to a closest-point-on-segment problem, then a
 * radius comparison - the standard technique (Ericson, Real-Time
 * Collision Detection, ch. 5) rather than anything invented here. */

typedef struct {
    vec3 a, b;
    f32 radius;
} Capsule;

vec3 capsule_closest_point_on_segment(Capsule c, vec3 p);
i32 capsule_contains_point(Capsule c, vec3 p);
i32 capsule_intersects_capsule(Capsule a, Capsule b);

#endif /* HOLYC_GEOMETRY_H_ */

#ifdef SPHERE_IMPLEMENTATION
#ifndef SPHERE_IMPLEMENTATION_
#define SPHERE_IMPLEMENTATION_

/* Newton-Raphson sqrt, replacing math.h's - own local copy (this
 * project's standing "no shared cross-file static" rule, see
 * src/base.h's own vector_sqrt_/quat_sqrt_/matrix_sqrt_ split for the
 * precedent and the algorithm this mirrors) - part of the "0 glibc"
 * push (no-glibc rule). */
static f64 sphere_sqrt_(f64 x)
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

Sphere sphere_from_points(const vec3 *points, size_t count)
{
    Sphere s;
    vec3 sum;
    size_t i;
    f32 max_dist_sq;

    sum = vec3_make(0.0f, 0.0f, 0.0f);
    if (count == 0) {
        s.center = sum;
        s.radius = 0.0f;
        return s;
    }

    for (i = 0; i < count; i++) sum = vec3_add(sum, points[i]);
    s.center = vec3_scale(sum, 1.0f / (f32)count);

    max_dist_sq = 0.0f;
    for (i = 0; i < count; i++) {
        vec3 diff = vec3_sub(points[i], s.center);
        f32 dist_sq = vec3_dot(diff, diff);
        if (dist_sq > max_dist_sq) max_dist_sq = dist_sq;
    }
    s.radius = (f32)sphere_sqrt_((f64)max_dist_sq);

    return s;
}

i32 sphere_contains_point(Sphere s, vec3 p)
{
    vec3 diff = vec3_sub(p, s.center);
    f32 dist_sq = vec3_dot(diff, diff);
    return (dist_sq <= s.radius * s.radius) ? 1 : 0;
}

i32 sphere_intersects_sphere(Sphere a, Sphere b)
{
    vec3 diff = vec3_sub(b.center, a.center);
    f32 dist_sq = vec3_dot(diff, diff);
    f32 radius_sum = a.radius + b.radius;
    return (dist_sq <= radius_sum * radius_sum) ? 1 : 0;
}

#endif /* SPHERE_IMPLEMENTATION_ */
#endif /* SPHERE_IMPLEMENTATION */

#ifdef PLANE_IMPLEMENTATION
#ifndef PLANE_IMPLEMENTATION_
#define PLANE_IMPLEMENTATION_

Plane plane_from_points(vec3 a, vec3 b, vec3 c)
{
    vec3 ab = vec3_sub(b, a);
    vec3 ac = vec3_sub(c, a);
    vec3 n = vec3_cross(ab, ac);
    f32 len = vec3_len(n);
    Plane result;

    if (len > 0.0f) {
        n = vec3_scale(n, 1.0f / len);
    }
    result.normal = n;
    result.d = -vec3_dot(n, a);
    return result;
}

Plane plane_from_normal_point(vec3 normal, vec3 point)
{
    f32 len = vec3_len(normal);
    Plane result;

    if (len > 0.0f) {
        normal = vec3_scale(normal, 1.0f / len);
    }
    result.normal = normal;
    result.d = -vec3_dot(normal, point);
    return result;
}

f32 plane_signed_distance(Plane p, vec3 point)
{
    return vec3_dot(p.normal, point) + p.d;
}

Plane_Side plane_classify(Plane p, vec3 point)
{
    f32 dist = plane_signed_distance(p, point);
    if (dist > PLANE_EPSILON) return PLANE_FRONT;
    if (dist < -PLANE_EPSILON) return PLANE_BACK;
    return PLANE_ON;
}

#endif /* PLANE_IMPLEMENTATION_ */
#endif /* PLANE_IMPLEMENTATION */

#ifdef TRIANGLE_IMPLEMENTATION
#ifndef TRIANGLE_IMPLEMENTATION_
#define TRIANGLE_IMPLEMENTATION_

vec3 triangle_normal(Triangle t)
{
    vec3 ab = vec3_sub(t.b, t.a);
    vec3 ac = vec3_sub(t.c, t.a);
    vec3 n = vec3_cross(ab, ac);
    f32 len = vec3_len(n);
    if (len > 0.0f) return vec3_scale(n, 1.0f / len);
    return n;
}

f32 triangle_area(Triangle t)
{
    vec3 ab = vec3_sub(t.b, t.a);
    vec3 ac = vec3_sub(t.c, t.a);
    vec3 cr = vec3_cross(ab, ac);
    return vec3_len(cr) * 0.5f;
}

vec3 triangle_centroid(Triangle t)
{
    vec3 sum = vec3_add(vec3_add(t.a, t.b), t.c);
    return vec3_scale(sum, 1.0f / 3.0f);
}

void triangle_barycentric(Triangle t, vec3 p, f32 *out_u, f32 *out_v, f32 *out_w)
{
    /* standard formulation (Ericson, Real-Time Collision Detection 3.4):
       express p-a in the (v0,v1) edge basis via a 2x2 linear solve using
       dot products, avoiding an explicit matrix inverse. */
    vec3 v0 = vec3_sub(t.b, t.a);
    vec3 v1 = vec3_sub(t.c, t.a);
    vec3 v2 = vec3_sub(p, t.a);

    f32 d00 = vec3_dot(v0, v0);
    f32 d01 = vec3_dot(v0, v1);
    f32 d11 = vec3_dot(v1, v1);
    f32 d20 = vec3_dot(v2, v0);
    f32 d21 = vec3_dot(v2, v1);
    f32 denom = d00 * d11 - d01 * d01;
    f32 v, w;

    if (denom == 0.0f) {
        /* degenerate (zero-area) triangle - no meaningful barycentric
           split, put all weight on vertex a rather than divide by zero */
        *out_u = 1.0f;
        *out_v = 0.0f;
        *out_w = 0.0f;
        return;
    }

    v = (d11 * d20 - d01 * d21) / denom;
    w = (d00 * d21 - d01 * d20) / denom;

    *out_v = v;
    *out_w = w;
    *out_u = 1.0f - v - w;
}

i32 triangle_contains_point(Triangle t, vec3 p)
{
    f32 u, v, w;
    triangle_barycentric(t, p, &u, &v, &w);
    return (u >= 0.0f && v >= 0.0f && w >= 0.0f) ? 1 : 0;
}

#endif /* TRIANGLE_IMPLEMENTATION_ */
#endif /* TRIANGLE_IMPLEMENTATION */

#ifdef CAPSULE_IMPLEMENTATION
#ifndef CAPSULE_IMPLEMENTATION_
#define CAPSULE_IMPLEMENTATION_

#ifndef CAPSULE_EPSILON
#define CAPSULE_EPSILON 0.0000001f
#endif

static f32 capsule_clamp01_(f32 t)
{
    if (t < 0.0f) return 0.0f;
    if (t > 1.0f) return 1.0f;
    return t;
}

vec3 capsule_closest_point_on_segment(Capsule c, vec3 p)
{
    vec3 ab = vec3_sub(c.b, c.a);
    f32 ab_dot_ab = vec3_dot(ab, ab);
    f32 t;

    if (ab_dot_ab <= CAPSULE_EPSILON) return c.a; /* degenerate (a==b) segment */

    t = vec3_dot(vec3_sub(p, c.a), ab) / ab_dot_ab;
    t = capsule_clamp01_(t);
    return vec3_add(c.a, vec3_scale(ab, t));
}

i32 capsule_contains_point(Capsule c, vec3 p)
{
    vec3 closest = capsule_closest_point_on_segment(c, p);
    vec3 diff = vec3_sub(p, closest);
    f32 dist_sq = vec3_dot(diff, diff);
    return (dist_sq <= c.radius * c.radius) ? 1 : 0;
}

/* closest points between two segments (Ericson, RTCD section 5.1.9) -
 * the standard algorithm, handles the degenerate cases (either or both
 * segments zero-length, parallel segments) explicitly rather than
 * letting them divide by zero. */
static void capsule_closest_segment_segment_(vec3 p1, vec3 q1, vec3 p2, vec3 q2,
                                              vec3 *out_c1, vec3 *out_c2)
{
    vec3 d1 = vec3_sub(q1, p1);
    vec3 d2 = vec3_sub(q2, p2);
    vec3 r = vec3_sub(p1, p2);
    f32 a = vec3_dot(d1, d1);
    f32 e = vec3_dot(d2, d2);
    f32 f = vec3_dot(d2, r);
    f32 s, t;

    if (a <= CAPSULE_EPSILON && e <= CAPSULE_EPSILON) {
        *out_c1 = p1;
        *out_c2 = p2;
        return;
    }

    if (a <= CAPSULE_EPSILON) {
        s = 0.0f;
        t = capsule_clamp01_(f / e);
    } else {
        f32 c = vec3_dot(d1, r);
        if (e <= CAPSULE_EPSILON) {
            t = 0.0f;
            s = capsule_clamp01_(-c / a);
        } else {
            f32 b = vec3_dot(d1, d2);
            f32 denom = a * e - b * b;

            if (denom > CAPSULE_EPSILON || denom < -CAPSULE_EPSILON) {
                s = capsule_clamp01_((b * f - c * e) / denom);
            } else {
                s = 0.0f;
            }

            t = (b * s + f) / e;
            if (t < 0.0f) {
                t = 0.0f;
                s = capsule_clamp01_(-c / a);
            } else if (t > 1.0f) {
                t = 1.0f;
                s = capsule_clamp01_((b - c) / a);
            }
        }
    }

    *out_c1 = vec3_add(p1, vec3_scale(d1, s));
    *out_c2 = vec3_add(p2, vec3_scale(d2, t));
}

i32 capsule_intersects_capsule(Capsule a, Capsule b)
{
    vec3 c1, c2, diff;
    f32 dist_sq, radius_sum;

    capsule_closest_segment_segment_(a.a, a.b, b.a, b.b, &c1, &c2);
    diff = vec3_sub(c2, c1);
    dist_sq = vec3_dot(diff, diff);
    radius_sum = a.radius + b.radius;

    return (dist_sq <= radius_sum * radius_sum) ? 1 : 0;
}

#endif /* CAPSULE_IMPLEMENTATION_ */
#endif /* CAPSULE_IMPLEMENTATION */
