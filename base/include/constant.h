#ifndef HOLYC_CONSTANT_H_
#define HOLYC_CONSTANT_H_

/* Common mathematical constants as f64 #defines, nothing else - no
 * function bodies, so no IMPLEMENTATION guard needed (matches
 * base_type.h's own `#define FP16_FRAC_BITS` style for a values-only
 * header). This is the raw numeric CONSTANTS other files pull from
 * instead of each hand-writing "3.14159265358979323846" in isolation -
 * NOT the deg/rad/turn conversion FUNCTIONS (that's trigonometry.h's
 * job, unchanged here). HOLYC_PI is defined guarded the same way
 * trigonometry.h already guards its own copy (`#ifndef HOLYC_PI`) so
 * whichever of the two files gets included first "wins" without a
 * conflict or duplicate value, no matter the include order. */

#ifndef HOLYC_PI
#define HOLYC_PI 3.14159265358979323846
#endif

#define HOLYC_TAU (2.0 * HOLYC_PI) /* one full turn, in radians */
#define HOLYC_E 2.71828182845904523536
#define HOLYC_SQRT2 1.41421356237309504880
#define HOLYC_GOLDEN_RATIO 1.61803398874989484820 /* phi = (1+sqrt(5))/2 */

#define HOLYC_DEG_TO_RAD (HOLYC_PI / 180.0)
#define HOLYC_RAD_TO_DEG (180.0 / HOLYC_PI)

#endif /* HOLYC_CONSTANT_H_ */
