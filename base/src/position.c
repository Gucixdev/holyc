#ifndef HOLYC_POSITION_C_
#define HOLYC_POSITION_C_

/* Minimal position component: a stored vec3 plus get/set/translate.
 * Deliberately tiny - a building-block component, not a full transform
 * hierarchy system (that would be a separate, bigger transform.h if
 * ever wanted). Self-contained per pool_alloc.c's established pattern
 * (its own guard around declarations, its own IMPLEMENTATION guard
 * around the body) rather than a separate paired header. */

#include "base.h"
#include "mathx.h"

typedef struct {
    vec3 value;
} Position;

void position_init(Position *p, vec3 value);
void position_set(Position *p, vec3 value);
void position_translate(Position *p, vec3 delta);

#endif /* HOLYC_POSITION_C_ */

#ifdef POSITION_IMPLEMENTATION
#ifndef POSITION_IMPLEMENTATION_
#define POSITION_IMPLEMENTATION_

void position_init(Position *p, vec3 value)
{
    p->value = value;
}

void position_set(Position *p, vec3 value)
{
    p->value = value;
}

void position_translate(Position *p, vec3 delta)
{
    p->value = vec3_add(p->value, delta);
}

#endif /* POSITION_IMPLEMENTATION_ */
#endif /* POSITION_IMPLEMENTATION */
