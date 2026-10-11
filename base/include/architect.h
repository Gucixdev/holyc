#ifndef HOLYC_ARCHITECT_H_
#define HOLYC_ARCHITECT_H_

/* The Architect (name is the Matrix reference, per this file's own
 * original spec) - a dynamic-difficulty director: tracks the
 * player's recent performance (deaths, damage taken, objective
 * success), derives a frustration estimate from it, and nudges a
 * difficulty_scalar the game reads (to scale enemy toughness/spawn
 * rate/whatever) to keep frustration hovering near a deliberate
 * "flow channel" target - not so easy the player gets bored, not so
 * hard they get frustrated, per Csikszentmihalyi's flow concept
 * (the same idea Left 4 Dead's "AI Director" and countless DDA
 * systems since have implemented some version of).
 *
 * ALSO does the "reports anomalies" half of the spec: repeated-event
 * exploit detection (the same trivial action farmed far more times
 * than varied real play would produce, inside a short window) and
 * reaction-time consistency anomaly detection (real human reaction
 * times have natural variance; a macro/script's don't - suspiciously
 * LOW variance is the tell, not slow or fast reactions themselves).
 * This file only DETECTS and reports both kinds of anomaly - what a
 * game does about a flagged one (log it, penalize it, ignore it) is
 * the caller's own decision, not this file's.
 *
 * All tracking is exponential-moving-average based (no history
 * buffer to size/manage) - "recent" behavior, weighted toward the
 * newest reports, same shape gravity.c/qlearn.h etc use elsewhere in
 * this directory for "accumulate over time" state. */

#include "base.h"

#ifndef ARCHITECT_TARGET_FRUSTRATION
#define ARCHITECT_TARGET_FRUSTRATION 0.4f /* the "flow channel" sweet spot - some real challenge, not overwhelming */
#endif
#ifndef ARCHITECT_DIFFICULTY_MIN
#define ARCHITECT_DIFFICULTY_MIN 0.5f
#endif
#ifndef ARCHITECT_DIFFICULTY_MAX
#define ARCHITECT_DIFFICULTY_MAX 2.0f
#endif

typedef struct {
    /* rolling player-performance signals, each an EMA in [0,1] */
    f32 death_rate_ema;
    f32 damage_taken_ema; /* normalized 0..1 by the caller - this file has no notion of a health scale */
    f32 success_rate_ema;

    /* derived state, recomputed by architect_update() */
    f32 frustration;      /* 0..1 */
    f32 engagement;         /* 0..1, peaks exactly at ARCHITECT_TARGET_FRUSTRATION */
    f32 difficulty_scalar;    /* what the game actually reads - starts at 1.0 (baseline) */

    /* repeated-event exploit detection */
    i32 last_event_id;
    i32 event_repeat_count;
    f32 time_since_last_event;

    /* reaction-time consistency anomaly detection (Welford's online
     * mean/variance, no stored history) */
    i32 reaction_sample_count;
    f32 reaction_mean;
    f32 reaction_m2; /* sum of squared deviations from the running mean - variance = m2 / sample_count */
} Architect;

void architect_init(Architect *a);

/* feed the three raw performance signals - each call nudges its own
 * EMA toward the new sample. `died`/`succeeded` are 0 or 1;
 * `damage_taken` should already be caller-normalized to 0..1 (e.g.
 * damage / max_health) since this file has no health-scale concept
 * of its own. */
void architect_report_death(Architect *a, i32 died, f32 ema_alpha);
void architect_report_damage_taken(Architect *a, f32 damage_taken, f32 ema_alpha);
void architect_report_objective_result(Architect *a, i32 succeeded, f32 ema_alpha);

/* recomputes frustration/engagement/difficulty_scalar from the
 * current EMAs, and decays the exploit-detection repeat window by
 * `dt` seconds. Call this once per real tick (not once per report -
 * the reports above only update raw EMAs, this is the "think" step
 * that turns them into the actual director decision), same "throttle
 * the expensive part" shape brain.c's own think/update split uses.
 * `adjust_rate` controls how fast difficulty_scalar chases the
 * frustration target (a proportional-controller gain). */
void architect_update(Architect *a, f32 dt, f32 adjust_rate);

/* reports a specific gameplay event (a caller-defined id - "used the
 * healing exploit spot", "farmed this one easy kill", whatever).
 * Returns 1 the moment the SAME event_id repeating within
 * `anomaly_window` seconds of each other crosses `anomaly_threshold`
 * repeats - a real, if simple, exploit-farming signal. A different
 * event_id, or the same one arriving MORE than anomaly_window seconds
 * after the last report, resets the streak. */
i32 architect_report_event(Architect *a, i32 event_id, f32 anomaly_window, i32 anomaly_threshold);

/* feeds one reaction-time sample (seconds) into the running mean/
 * variance. */
void architect_report_reaction_time(Architect *a, f32 seconds);

/* true once enough samples exist (>= 8, arbitrarily but reasonably -
 * variance from fewer samples is too noisy to trust) AND the sample
 * variance is below `min_variance` - suspiciously consistent
 * reaction times, the tell a real human's natural timing jitter
 * wouldn't produce. */
i32 architect_reaction_time_is_anomalous(const Architect *a, f32 min_variance);

#endif /* HOLYC_ARCHITECT_H_ */

#ifdef ARCHITECT_IMPLEMENTATION
#ifndef ARCHITECT_IMPLEMENTATION_
#define ARCHITECT_IMPLEMENTATION_

static f32 architect_clamp01_(f32 v)
{
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

static f32 architect_ema_(f32 old, f32 sample, f32 alpha)
{
    return old + alpha * (sample - old);
}

void architect_init(Architect *a)
{
    a->death_rate_ema = 0.0f;
    a->damage_taken_ema = 0.0f;
    a->success_rate_ema = 1.0f; /* optimistic prior - assume competent until shown otherwise */

    a->frustration = 0.0f;
    a->engagement = 0.0f;
    a->difficulty_scalar = 1.0f;

    a->last_event_id = -1;
    a->event_repeat_count = 0;
    a->time_since_last_event = 0.0f;

    a->reaction_sample_count = 0;
    a->reaction_mean = 0.0f;
    a->reaction_m2 = 0.0f;
}

void architect_report_death(Architect *a, i32 died, f32 ema_alpha)
{
    a->death_rate_ema = architect_ema_(a->death_rate_ema, died ? 1.0f : 0.0f, ema_alpha);
}

void architect_report_damage_taken(Architect *a, f32 damage_taken, f32 ema_alpha)
{
    a->damage_taken_ema = architect_ema_(a->damage_taken_ema, architect_clamp01_(damage_taken), ema_alpha);
}

void architect_report_objective_result(Architect *a, i32 succeeded, f32 ema_alpha)
{
    a->success_rate_ema = architect_ema_(a->success_rate_ema, succeeded ? 1.0f : 0.0f, ema_alpha);
}

void architect_update(Architect *a, f32 dt, f32 adjust_rate)
{
    f32 error;

    /* weighted blend of the three signals - deaths matter most,
     * then damage taken, then how often objectives are failed */
    a->frustration = architect_clamp01_(
        a->death_rate_ema * 0.5f +
        a->damage_taken_ema * 0.3f +
        (1.0f - a->success_rate_ema) * 0.2f
    );

    {
        f32 span = ARCHITECT_TARGET_FRUSTRATION;
        f32 dist = a->frustration - ARCHITECT_TARGET_FRUSTRATION;
        if (dist > 0.0f) span = 1.0f - ARCHITECT_TARGET_FRUSTRATION; /* normalize the two sides of the target separately - they're different widths */
        a->engagement = architect_clamp01_(1.0f - (dist < 0.0f ? -dist : dist) / (span > 0.0f ? span : 1.0f));
    }

    /* proportional controller: too frustrated (error < 0) -> ease off;
     * too comfortable (error > 0) -> turn it up */
    error = ARCHITECT_TARGET_FRUSTRATION - a->frustration;
    a->difficulty_scalar += adjust_rate * error * dt;
    if (a->difficulty_scalar < ARCHITECT_DIFFICULTY_MIN) a->difficulty_scalar = ARCHITECT_DIFFICULTY_MIN;
    if (a->difficulty_scalar > ARCHITECT_DIFFICULTY_MAX) a->difficulty_scalar = ARCHITECT_DIFFICULTY_MAX;

    a->time_since_last_event += dt;
}

i32 architect_report_event(Architect *a, i32 event_id, f32 anomaly_window, i32 anomaly_threshold)
{
    if (event_id == a->last_event_id && a->time_since_last_event <= anomaly_window) {
        a->event_repeat_count++;
    } else {
        a->event_repeat_count = 1;
    }

    a->last_event_id = event_id;
    a->time_since_last_event = 0.0f;

    return a->event_repeat_count >= anomaly_threshold;
}

void architect_report_reaction_time(Architect *a, f32 seconds)
{
    /* Welford's online algorithm - mean/variance without storing
     * every sample */
    f32 delta, delta2;

    a->reaction_sample_count++;
    delta = seconds - a->reaction_mean;
    a->reaction_mean += delta / (f32)a->reaction_sample_count;
    delta2 = seconds - a->reaction_mean;
    a->reaction_m2 += delta * delta2;
}

i32 architect_reaction_time_is_anomalous(const Architect *a, f32 min_variance)
{
    f32 variance;
    if (a->reaction_sample_count < 8) return 0;
    variance = a->reaction_m2 / (f32)a->reaction_sample_count;
    return variance < min_variance;
}

#endif /* ARCHITECT_IMPLEMENTATION_ */
#endif /* ARCHITECT_IMPLEMENTATION */
