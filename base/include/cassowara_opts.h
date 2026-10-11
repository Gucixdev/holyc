#ifndef HOLYC_CASSOWARA_OPTS_H_
#define HOLYC_CASSOWARA_OPTS_H_

/* Default thresholds for cassowara.h's queries, bundled so a caller
 * can tune once (per-entity-type, per-game-mode, whatever) instead of
 * re-typing magic numbers at every call site. */

#include "base.h"

typedef struct {
    f32 next_to_dist;    /* cassowara_is_next_to default max_dist */
    f32 facing_half_angle_deg; /* cassowara_is_behind/in_front_of/facing default half_angle_deg */
    f32 in_range_min;    /* cassowara_is_in_range default min_dist */
    f32 in_range_max;    /* cassowara_is_in_range default max_dist */
} Cassowara_Opts;

/* next_to=2.0, facing cone=45deg (a common "am I looking at it"
 * threshold), range=[0, 10]. */
void cassowara_opts_defaults(Cassowara_Opts *opts);

#endif /* HOLYC_CASSOWARA_OPTS_H_ */

#ifdef CASSOWARA_OPTS_IMPLEMENTATION
#ifndef CASSOWARA_OPTS_IMPLEMENTATION_
#define CASSOWARA_OPTS_IMPLEMENTATION_

void cassowara_opts_defaults(Cassowara_Opts *opts)
{
    opts->next_to_dist = 2.0f;
    opts->facing_half_angle_deg = 45.0f;
    opts->in_range_min = 0.0f;
    opts->in_range_max = 10.0f;
}

#endif /* CASSOWARA_OPTS_IMPLEMENTATION_ */
#endif /* CASSOWARA_OPTS_IMPLEMENTATION */
