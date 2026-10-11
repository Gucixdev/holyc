#ifndef HOLYC_INTERSECT_H_
#define HOLYC_INTERSECT_H_

/* A small collection of the simplest 2D/3D intersection tests that
 * don't fit naturally into any of the more specific geometry files
 * (`src/gamedev/geometry/plane.h`/`triangle.h`/`sphere.h`/`capsule.h`/
 * `obb.h`/`aabb_collision.h`/`raycast.h`/`gjk.h`) - kept genuinely
 * small on purpose (3 functions), not a dumping ground: 2D segment-
 * segment (useful for 2D game logic, UI hit-testing, navmesh edge
 * checks), 2D circle-circle (the simplest possible 2D collision
 * primitive), and ray-plane (needed by both raycast.h's own AABB/
 * sphere/triangle family conceptually but not included there since
 * a plane by itself, unlike a triangle, isn't one of THIS file's
 * neighbors' actual shapes). */

#include "base.h"
#include "mathx.h"

/* standard 2D parametric-line-intersection test via the cross-product
 * form: solves A + t*(B-A) == C + u*(D-C) for t,u, only reports a hit
 * if BOTH t and u land in [0,1] (i.e. the actual finite segments cross,
 * not just their infinite extensions). Parallel (including collinear)
 * segments are reported as no intersection - a real, documented
 * simplification, not silently wrong: the collinear-overlap case needs
 * its own 1D interval-overlap logic this function doesn't attempt. */
i32 intersect_segment_segment_2d(vec2 a, vec2 b, vec2 c, vec2 d, vec2 *out_point);

/* two circles intersect (including exactly touching) when the distance
 * between their centers is <= the sum of their radii - this also
 * reports 1 when one circle fully contains the other (a real, common
 * convention: "do these two circular regions share any point" rather
 * than "do their boundary curves cross", which would need a second,
 * separate >= |r1-r2| check most callers don't actually want). */
i32 intersect_circle_circle_2d(vec2 c0, f32 r0, vec2 c1, f32 r1);

/* ray-plane intersection via the standard `t = dot(N, point-origin) /
 * dot(N, dir)` form. Returns 1 and sets *out_t only for a genuine
 * forward hit (t >= 0) on a non-parallel ray; a ray in the plane's own
 * plane (dot(N,dir) == 0) is reported as a miss, matching
 * raycast.h's ray_triangle's own "parallel is a documented miss, not a
 * special hit" convention. */
i32 intersect_ray_plane(vec3 origin, vec3 dir, vec3 plane_point, vec3 plane_normal, f32 *out_t);

#endif /* HOLYC_INTERSECT_H_ */

#ifdef INTERSECT_IMPLEMENTATION
#ifndef INTERSECT_IMPLEMENTATION_
#define INTERSECT_IMPLEMENTATION_

/* math.h was included here but never actually used (no sqrt/sin/cos/
 * etc call anywhere in this file) - dropped as part of this project's
 * "0 glibc" push, not replaced with anything since there was nothing
 * to replace. */

#ifndef INTERSECT_EPSILON_
#define INTERSECT_EPSILON_ 0.0000001f
#endif

i32 intersect_segment_segment_2d(vec2 a, vec2 b, vec2 c, vec2 d, vec2 *out_point)
{
    f32 d1x = b.x - a.x, d1y = b.y - a.y;
    f32 d2x = d.x - c.x, d2y = d.y - c.y;
    f32 denom = d1x * d2y - d1y * d2x;
    f32 t, u;

    if (denom > -INTERSECT_EPSILON_ && denom < INTERSECT_EPSILON_) return 0; /* parallel or collinear */

    t = ((c.x - a.x) * d2y - (c.y - a.y) * d2x) / denom;
    u = ((c.x - a.x) * d1y - (c.y - a.y) * d1x) / denom;

    if (t < 0.0f || t > 1.0f || u < 0.0f || u > 1.0f) return 0;

    out_point->x = a.x + t * d1x;
    out_point->y = a.y + t * d1y;
    return 1;
}

i32 intersect_circle_circle_2d(vec2 c0, f32 r0, vec2 c1, f32 r1)
{
    f32 dx = c1.x - c0.x;
    f32 dy = c1.y - c0.y;
    f32 dist_sq = dx * dx + dy * dy;
    f32 sum_r = r0 + r1;
    return dist_sq <= sum_r * sum_r;
}

i32 intersect_ray_plane(vec3 origin, vec3 dir, vec3 plane_point, vec3 plane_normal, f32 *out_t)
{
    f32 denom = vec3_dot(plane_normal, dir);
    f32 t;

    if (denom > -INTERSECT_EPSILON_ && denom < INTERSECT_EPSILON_) return 0; /* parallel */

    t = vec3_dot(plane_normal, vec3_sub(plane_point, origin)) / denom;
    if (t < 0.0f) return 0;

    *out_t = t;
    return 1;
}

#endif /* INTERSECT_IMPLEMENTATION_ */
#endif /* INTERSECT_IMPLEMENTATION */
