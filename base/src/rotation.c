#ifndef HOLYC_ROTATION_C_
#define HOLYC_ROTATION_C_

/* Minimal orientation component: a stored Quat plus get/set/apply. Same
 * shape as position.c/scale.c - a tiny building block, not a transform
 * hierarchy system. Self-contained per pool_alloc.c's established
 * pattern (its own guard around declarations, its own IMPLEMENTATION
 * guard around the body) rather than a separate paired header. */

#include "base.h"
#include "mathx.h"

typedef struct {
    Quat value;
} Rotation;

void rotation_init(Rotation *r, Quat value);
void rotation_set(Rotation *r, Quat value);
vec3 rotation_apply(const Rotation *r, vec3 v);

#endif /* HOLYC_ROTATION_C_ */

#ifdef ROTATION_IMPLEMENTATION
#ifndef ROTATION_IMPLEMENTATION_
#define ROTATION_IMPLEMENTATION_

void rotation_init(Rotation *r, Quat value)
{
    r->value = value;
}

void rotation_set(Rotation *r, Quat value)
{
    r->value = value;
}

vec3 rotation_apply(const Rotation *r, vec3 v)
{
    return quat_mul_vec3(r->value, v);
}

#endif /* ROTATION_IMPLEMENTATION_ */
#endif /* ROTATION_IMPLEMENTATION */
