#ifndef HOLYC_AABB_COLLISION_H_
#define HOLYC_AABB_COLLISION_H_

/* Axis-Aligned Bounding Box - min/max vec3 corners, plus the standard
 * collision tests. No existing Aabb type was found anywhere else in
 * this project (checked before defining one here). aabb_intersects_
 * sphere lives HERE (not in sphere.h) since this file is specifically
 * "AABB collision tests" - sphere.h stays independent of this file in
 * the other direction (it doesn't need to know AABBs exist at all),
 * matching this project's established "minimal, independent" split
 * (vector.h/matrix.h/quat.h) - a one-directional dependency (this file
 * needs sphere.h's Sphere type) rather than a tangle. */

#include "base.h"
#include "mathx.h"
#include "geometry.h"

typedef struct {
    vec3 min, max;
} Aabb;

Aabb aabb_from_points(const vec3 *points, size_t count);
i32 aabb_intersects_aabb(Aabb a, Aabb b);
i32 aabb_contains_point(Aabb box, vec3 p);
i32 aabb_intersects_sphere(Aabb box, Sphere s);

#endif /* HOLYC_AABB_COLLISION_H_ */

#ifdef AABB_COLLISION_IMPLEMENTATION
#ifndef AABB_COLLISION_IMPLEMENTATION_
#define AABB_COLLISION_IMPLEMENTATION_

static f32 aabb_min_(f32 a, f32 b) { return a < b ? a : b; }
static f32 aabb_max_(f32 a, f32 b) { return a > b ? a : b; }
static f32 aabb_clamp_(f32 v, f32 lo, f32 hi) { return aabb_max_(lo, aabb_min_(v, hi)); }

Aabb aabb_from_points(const vec3 *points, size_t count)
{
    Aabb box;
    size_t i;

    if (count == 0) {
        box.min = vec3_make(0.0f, 0.0f, 0.0f);
        box.max = vec3_make(0.0f, 0.0f, 0.0f);
        return box;
    }

    box.min = points[0];
    box.max = points[0];
    for (i = 1; i < count; i++) {
        box.min.x = aabb_min_(box.min.x, points[i].x);
        box.min.y = aabb_min_(box.min.y, points[i].y);
        box.min.z = aabb_min_(box.min.z, points[i].z);
        box.max.x = aabb_max_(box.max.x, points[i].x);
        box.max.y = aabb_max_(box.max.y, points[i].y);
        box.max.z = aabb_max_(box.max.z, points[i].z);
    }
    return box;
}

i32 aabb_intersects_aabb(Aabb a, Aabb b)
{
    if (a.max.x < b.min.x || a.min.x > b.max.x) return 0;
    if (a.max.y < b.min.y || a.min.y > b.max.y) return 0;
    if (a.max.z < b.min.z || a.min.z > b.max.z) return 0;
    return 1;
}

i32 aabb_contains_point(Aabb box, vec3 p)
{
    return (p.x >= box.min.x && p.x <= box.max.x &&
            p.y >= box.min.y && p.y <= box.max.y &&
            p.z >= box.min.z && p.z <= box.max.z) ? 1 : 0;
}

i32 aabb_intersects_sphere(Aabb box, Sphere s)
{
    vec3 closest;
    vec3 diff;
    f32 dist_sq;

    closest.x = aabb_clamp_(s.center.x, box.min.x, box.max.x);
    closest.y = aabb_clamp_(s.center.y, box.min.y, box.max.y);
    closest.z = aabb_clamp_(s.center.z, box.min.z, box.max.z);

    diff = vec3_sub(s.center, closest);
    dist_sq = vec3_dot(diff, diff);

    return (dist_sq <= s.radius * s.radius) ? 1 : 0;
}

#endif /* AABB_COLLISION_IMPLEMENTATION_ */
#endif /* AABB_COLLISION_IMPLEMENTATION */
