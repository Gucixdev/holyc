#ifndef HOLYC_MIXTURE_H_
#define HOLYC_MIXTURE_H_

/* A weighted blend of named f64 components - literally a "mix", not a
 * selection: mixture_value() returns the weighted average of every
 * ingredient's value, all of them contributing proportionally to their
 * weight, never just picking one. NOT the same job as gamedev/uai.h's
 * uai_select_weighted (that picks exactly ONE option, probabilistically,
 * by weight) - a mixture always uses every ingredient, an animation
 * blend tree / material-property blend / weighted-average-of-opinions
 * kind of job, not a random-choice one. Fixed capacity, no arena at
 * all - matches signal.h/pkg.h's own small-fixed-struct stance. */

#include "base.h"

#ifndef MIXTURE_MAX_INGREDIENTS
#define MIXTURE_MAX_INGREDIENTS 16
#endif

typedef struct {
    const char *name;
    f64 value;
    f64 weight;
} Mixture_Ingredient;

typedef struct {
    Mixture_Ingredient ingredients[MIXTURE_MAX_INGREDIENTS];
    i32 count;
} Mixture;

void mixture_init(Mixture *m);

/* weight must be >= 0 - a negative weight would make "the weighted
 * average of every ingredient" stop meaning what it says (a component
 * that SUBTRACTS from the total isn't a mixture ingredient, it's a
 * different operation entirely) - HOLYC_ASSERT, not silently clamped.
 * Returns 0 if already at MIXTURE_MAX_INGREDIENTS. */
i32 mixture_add(Mixture *m, const char *name, f64 value, f64 weight);

/* changes an existing ingredient's weight in place (its value and
 * position are untouched) - the live "how much of each" knob a real
 * blend tree adjusts every frame, without needing to remove and
 * re-add. Returns 0 if no ingredient with that name exists. */
i32 mixture_set_weight(Mixture *m, const char *name, f64 weight);

/* the weighted average: sum(value_i * weight_i) / sum(weight_i).
 * Returns 0 (leaving *out unspecified) if the mixture is empty or
 * every weight is exactly 0 - a real, checkable "no defined average"
 * case (0/0), not a silent NaN. */
i32 mixture_value(const Mixture *m, f64 *out);

#endif /* HOLYC_MIXTURE_H_ */

#ifdef MIXTURE_IMPLEMENTATION
#ifndef MIXTURE_IMPLEMENTATION_
#define MIXTURE_IMPLEMENTATION_

static i32 mixture_streq_(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

void mixture_init(Mixture *m)
{
    m->count = 0;
}

i32 mixture_add(Mixture *m, const char *name, f64 value, f64 weight)
{
    HOLYC_ASSERT(weight >= 0.0);
    if (m->count >= MIXTURE_MAX_INGREDIENTS) return 0;
    m->ingredients[m->count].name = name;
    m->ingredients[m->count].value = value;
    m->ingredients[m->count].weight = weight;
    m->count++;
    return 1;
}

i32 mixture_set_weight(Mixture *m, const char *name, f64 weight)
{
    i32 i;
    HOLYC_ASSERT(weight >= 0.0);
    for (i = 0; i < m->count; i++) {
        if (mixture_streq_(m->ingredients[i].name, name)) {
            m->ingredients[i].weight = weight;
            return 1;
        }
    }
    return 0;
}

i32 mixture_value(const Mixture *m, f64 *out)
{
    f64 weighted_sum = 0.0, weight_total = 0.0;
    i32 i;
    for (i = 0; i < m->count; i++) {
        weighted_sum += m->ingredients[i].value * m->ingredients[i].weight;
        weight_total += m->ingredients[i].weight;
    }
    if (weight_total == 0.0) return 0;
    *out = weighted_sum / weight_total;
    return 1;
}

#endif /* MIXTURE_IMPLEMENTATION_ */
#endif /* MIXTURE_IMPLEMENTATION */
