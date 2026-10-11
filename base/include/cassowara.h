#ifndef HOLYC_CASSOWARA_H_
#define HOLYC_CASSOWARA_H_

/* Spatial relation queries between two positions (+ optional facing
 * direction): next_to, behind, in_front_of, in_range/out_of_range,
 * closest_to, farthest_to. Named for named-relation readability at
 * call sites (`cassowara_is_behind(enemy, player)` reads like the
 * design doc, not `vec3_dot(...) < 0`).
 *
 * All angle-based checks (behind/in_front_of/facing) take a forward
 * direction vector (need not be normalized - normalized internally)
 * and a half_angle_deg cone width. See cassowara_opts.h for sensible
 * default thresholds, cassowara_percentage.h for the same idea
 * parameterized by an arc FRACTION (half/quarter/...) instead of a
 * fixed angle. */

#include "base.h"
#include "mathx.h"
#include <math.h> /* cos - trigonometry.h's own precedent (see its header comment): call libm directly */

/* straight-line distance, no direction involved. */
i32 cassowara_is_next_to(vec3 a, vec3 b, f32 max_dist);

/* is `b` within [min_dist, max_dist] of `a`. */
i32 cassowara_is_in_range(vec3 a, vec3 b, f32 min_dist, f32 max_dist);
i32 cassowara_is_out_of_range(vec3 a, vec3 b, f32 min_dist, f32 max_dist);

/* is `b` inside the cone behind `a` (opposite `a_forward`), within
 * half_angle_deg of dead-behind. */
i32 cassowara_is_behind(vec3 a, vec3 a_forward, vec3 b, f32 half_angle_deg);

/* is `b` inside the cone in front of `a` (along `a_forward`), within
 * half_angle_deg of dead-ahead. */
i32 cassowara_is_in_front_of(vec3 a, vec3 a_forward, vec3 b, f32 half_angle_deg);

/* is `a_forward` roughly pointed at `b` (same as in_front_of, named
 * for the "am I looking at it" reading at call sites). */
i32 cassowara_is_facing(vec3 a, vec3 a_forward, vec3 b, f32 half_angle_deg);

/* index into `candidates` of the nearest/farthest point to `origin`.
 * Returns -1 for count == 0. */
i32 cassowara_closest_to(vec3 origin, const vec3 *candidates, i32 count);
i32 cassowara_farthest_to(vec3 origin, const vec3 *candidates, i32 count);

#endif /* HOLYC_CASSOWARA_H_ */

#ifdef CASSOWARA_IMPLEMENTATION
#ifndef CASSOWARA_IMPLEMENTATION_
#define CASSOWARA_IMPLEMENTATION_

i32 cassowara_is_next_to(vec3 a, vec3 b, f32 max_dist)
{
    return vec3_len(vec3_sub(b, a)) <= max_dist;
}

i32 cassowara_is_in_range(vec3 a, vec3 b, f32 min_dist, f32 max_dist)
{
    f32 d = vec3_len(vec3_sub(b, a));
    return d >= min_dist && d <= max_dist;
}

i32 cassowara_is_out_of_range(vec3 a, vec3 b, f32 min_dist, f32 max_dist)
{
    return !cassowara_is_in_range(a, b, min_dist, max_dist);
}

/* shared angle-cone check: is `to` within half_angle_deg of `dir`
 * (dir need not be normalized - only its direction matters, since
 * we compare against cos(half_angle) using normalized vectors). */
static i32 cassowara_within_cone_(vec3 from, vec3 dir, vec3 to, f32 half_angle_deg)
{
    vec3 to_target;
    f32 dir_len, to_len, cos_angle, cos_threshold;

    to_target = vec3_sub(to, from);
    dir_len = vec3_len(dir);
    to_len = vec3_len(to_target);
    if (dir_len <= 0.0f || to_len <= 0.0f) return 0; /* same position or zero direction - no defined angle */

    cos_angle = vec3_dot(dir, to_target) / (dir_len * to_len);
    cos_threshold = (f32)cos((f64)(half_angle_deg * (3.14159265358979323846 / 180.0)));
    return cos_angle >= cos_threshold;
}

i32 cassowara_is_behind(vec3 a, vec3 a_forward, vec3 b, f32 half_angle_deg)
{
    vec3 backward;
    backward.x = -a_forward.x; backward.y = -a_forward.y; backward.z = -a_forward.z;
    return cassowara_within_cone_(a, backward, b, half_angle_deg);
}

i32 cassowara_is_in_front_of(vec3 a, vec3 a_forward, vec3 b, f32 half_angle_deg)
{
    return cassowara_within_cone_(a, a_forward, b, half_angle_deg);
}

i32 cassowara_is_facing(vec3 a, vec3 a_forward, vec3 b, f32 half_angle_deg)
{
    return cassowara_is_in_front_of(a, a_forward, b, half_angle_deg);
}

i32 cassowara_closest_to(vec3 origin, const vec3 *candidates, i32 count)
{
    i32 i, best = -1;
    f32 best_dist = 0.0f;
    for (i = 0; i < count; i++) {
        f32 d = vec3_len(vec3_sub(candidates[i], origin));
        if (best == -1 || d < best_dist) { best = i; best_dist = d; }
    }
    return best;
}

i32 cassowara_farthest_to(vec3 origin, const vec3 *candidates, i32 count)
{
    i32 i, best = -1;
    f32 best_dist = 0.0f;
    for (i = 0; i < count; i++) {
        f32 d = vec3_len(vec3_sub(candidates[i], origin));
        if (best == -1 || d > best_dist) { best = i; best_dist = d; }
    }
    return best;
}

#endif /* CASSOWARA_IMPLEMENTATION_ */
#endif /* CASSOWARA_IMPLEMENTATION */
