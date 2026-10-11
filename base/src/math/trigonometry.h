#ifndef HOLYC_TRIGONOMETRY_H_
#define HOLYC_TRIGONOMETRY_H_

/* Deg/rad/turn conversion, nothing else - checked HandmadeMath and cglm
 * before writing this: neither one reimplements sin/cos/tan, both just
 * call libm's sinf/cosf/tanf directly and add angle-unit conversion on
 * top. Same pragmatic stance nn.h already took on tanh/sin/cos/asin -
 * don't reinvent libm badly, this file's only job is the conversion
 * constants libm doesn't provide. euler-angle composition (triple ->
 * rotation matrix/quaternion) is linear_algebra.h's job, not this file's -
 * cglm bundles that into its own trig-adjacent euler.h, but it's really
 * orientation math, not a trig helper. */

#include "core/base.h"

#ifndef HOLYC_PI
#define HOLYC_PI 3.14159265358979323846
#endif

#define DEG_TO_RAD_ (HOLYC_PI / 180.0)
#define RAD_TO_DEG_ (180.0 / HOLYC_PI)
#define TURN_TO_RAD_ (2.0 * HOLYC_PI)
#define RAD_TO_TURN_ (1.0 / (2.0 * HOLYC_PI))

f32 deg_to_rad(f32 deg);
f32 rad_to_deg(f32 rad);
f32 turn_to_rad(f32 turn); /* 1 turn == 360 degrees == 2*pi radians */
f32 rad_to_turn(f32 rad);

#endif /* HOLYC_TRIGONOMETRY_H_ */

#ifdef TRIGONOMETRY_IMPLEMENTATION
#ifndef TRIGONOMETRY_IMPLEMENTATION_
#define TRIGONOMETRY_IMPLEMENTATION_

f32 deg_to_rad(f32 deg)  { return (f32)((f64)deg * DEG_TO_RAD_); }
f32 rad_to_deg(f32 rad)  { return (f32)((f64)rad * RAD_TO_DEG_); }
f32 turn_to_rad(f32 turn) { return (f32)((f64)turn * TURN_TO_RAD_); }
f32 rad_to_turn(f32 rad)  { return (f32)((f64)rad * RAD_TO_TURN_); }

#endif /* TRIGONOMETRY_IMPLEMENTATION_ */
#endif /* TRIGONOMETRY_IMPLEMENTATION */
