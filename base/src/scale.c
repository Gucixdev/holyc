#ifndef HOLYC_SCALE_C_
#define HOLYC_SCALE_C_

/* Minimal scale component: a stored vec3 scale factor plus
 * get/set/set_uniform/apply. Same shape as position.c/rotation.c - a
 * tiny building block, not a full transform hierarchy system.
 * Self-contained per pool_alloc.c's established pattern (its own guard
 * around declarations, its own IMPLEMENTATION guard around the body)
 * rather than a separate paired header. */

#include "base.h"
#include "mathx.h"

typedef struct {
    vec3 value;
} Scale;

void scale_init(Scale *s, vec3 value);
void scale_set(Scale *s, vec3 value);
void scale_set_uniform(Scale *s, f32 factor);
vec3 scale_apply(const Scale *s, vec3 v);

#endif /* HOLYC_SCALE_C_ */

#ifdef SCALE_IMPLEMENTATION
#ifndef SCALE_IMPLEMENTATION_
#define SCALE_IMPLEMENTATION_

void scale_init(Scale *s, vec3 value)
{
    s->value = value;
}

void scale_set(Scale *s, vec3 value)
{
    s->value = value;
}

void scale_set_uniform(Scale *s, f32 factor)
{
    s->value = vec3_make(factor, factor, factor);
}

vec3 scale_apply(const Scale *s, vec3 v)
{
    return vec3_make(v.x * s->value.x, v.y * s->value.y, v.z * s->value.z);
}

#endif /* SCALE_IMPLEMENTATION_ */
#endif /* SCALE_IMPLEMENTATION */
