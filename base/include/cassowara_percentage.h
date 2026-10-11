#ifndef HOLYC_CASSOWARA_PERCENTAGE_H_
#define HOLYC_CASSOWARA_PERCENTAGE_H_

/* cassowara.h's behind/in_front_of use a fixed cone angle. This is the
 * same query parameterized by an arc FRACTION of the full surrounding
 * circle instead: full=1.0 (360 deg, always true), half=0.5 (180 deg,
 * the front/back hemisphere), quarter=0.25 (90 deg cone), etc. Reads
 * as `cassowara_is_within_arc(a, forward, b, CASSOWARA_ARC_QUARTER)`
 * at call sites instead of a bare angle number. */

#include "base.h"
#include "mathx.h"
#include "cassowara.h"

#define CASSOWARA_ARC_FULL    1.0f
#define CASSOWARA_ARC_HALF    0.5f
#define CASSOWARA_ARC_QUARTER 0.25f
#define CASSOWARA_ARC_EIGHTH  0.125f

/* fraction in (0, 1]. A fraction of 1.0 covers the full circle (always
 * true); 0.5 is a half-circle centered on `a_forward`; 0.25 a quarter,
 * etc. Converts to a half-angle and calls cassowara_is_in_front_of. */
i32 cassowara_is_within_arc(vec3 a, vec3 a_forward, vec3 b, f32 fraction);

/* which named arc bucket `b` falls into relative to `a_forward`, as
 * the SMALLEST arc fraction that contains it (full/half/quarter/
 * eighth) - useful for "how squarely am I facing this" bucketing
 * without the caller re-deriving thresholds by hand. Returns
 * CASSOWARA_ARC_FULL if it doesn't fall within any smaller bucket. */
f32 cassowara_arc_bucket(vec3 a, vec3 a_forward, vec3 b);

#endif /* HOLYC_CASSOWARA_PERCENTAGE_H_ */

#ifdef CASSOWARA_PERCENTAGE_IMPLEMENTATION
#ifndef CASSOWARA_PERCENTAGE_IMPLEMENTATION_
#define CASSOWARA_PERCENTAGE_IMPLEMENTATION_

i32 cassowara_is_within_arc(vec3 a, vec3 a_forward, vec3 b, f32 fraction)
{
    f32 half_angle_deg;
    if (fraction >= 1.0f) return 1; /* the full circle always contains everything */
    if (fraction <= 0.0f) return 0;
    half_angle_deg = fraction * 180.0f; /* fraction of the full 360 deg circle, as a half-angle */
    return cassowara_is_in_front_of(a, a_forward, b, half_angle_deg);
}

f32 cassowara_arc_bucket(vec3 a, vec3 a_forward, vec3 b)
{
    if (cassowara_is_within_arc(a, a_forward, b, CASSOWARA_ARC_EIGHTH)) return CASSOWARA_ARC_EIGHTH;
    if (cassowara_is_within_arc(a, a_forward, b, CASSOWARA_ARC_QUARTER)) return CASSOWARA_ARC_QUARTER;
    if (cassowara_is_within_arc(a, a_forward, b, CASSOWARA_ARC_HALF)) return CASSOWARA_ARC_HALF;
    return CASSOWARA_ARC_FULL;
}

#endif /* CASSOWARA_PERCENTAGE_IMPLEMENTATION_ */
#endif /* CASSOWARA_PERCENTAGE_IMPLEMENTATION */
