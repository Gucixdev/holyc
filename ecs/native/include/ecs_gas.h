/* ecs_gas.h - a lightweight gameplay-ability layer in the spirit of Unreal's GAS, done the data-oriented way: gameplay TAGS,
 * EFFECTS (instant / timed / periodic, stacking, granting tags), and ABILITIES (cost, cooldown, tag gates), all driven off the
 * ecs_stats table and reporting through the event bus. No scripting, no inheritance, no per-effect heap objects: definitions are
 * plain structs, running effects are rows in flat arrays that tick with vector loops.
 *
 * TAGS   A 64-bit mask per entity (names -> bits via ecs_gas_tag). Effective tags = permanent tags | tags granted by active effects.
 *        "Can I act?" is a mask test, not a string compare. Cooldowns are tags too.
 * EFFECT What it does to one stat column of the target:
 *          INSTANT    applies once (ADD / MUL / SET).
 *          DURATION   lasts `duration` s; INFINITE lasts until removed.
 *          periodic   (period > 0) adds magnitude*stacks every period: damage over time, heal over time, a drain. This is the DoT.
 *          modifier   a non-periodic timed ADD/MUL (e.g. +20 defense for 10 s) is applied when it lands and exactly undone when it
 *                     ends (the delta it applied is recorded). MUL is relative to the value when applied.
 *        Stacking is per (target, effect): NONE ignores a re-application while active, REFRESH resets the timer, STACK adds a stack
 *        (up to max_stacks) and resets the timer. Application can be gated by required/blocked tags, and an effect can cancel
 *        other effects by the tags they grant (a cleanse).
 * ABILITY  cost (spent via the souls rule for SP), cooldown (implemented as a timed effect granting a cooldown tag - no extra
 *        state), required/blocked tags on the user, and effects applied to the user and/or target on activation. Activation
 *        returns WHY it failed (resource, cooldown, tags, dead) and emits ABILITY_ACTIVATED / ABILITY_FAILED.
 *
 * Order each fixed step: ecs_gas_tick(gas, dt) THEN ecs_stats_tick(stats, dt), so effect results land in the same step's flag
 * pass. Entities are referenced by handle, so rows moving inside the stats table never invalidates a running effect; effects on an
 * entity that left the table are dropped. Not thread-safe. */
#ifndef ECS_GAS_H
#define ECS_GAS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ecs_event.h"
#include "ecs_stats.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ecs_effect_kind { ECS_EFFECT_INSTANT, ECS_EFFECT_DURATION, ECS_EFFECT_INFINITE } ecs_effect_kind;
typedef enum ecs_mod_op { ECS_MOD_ADD, ECS_MOD_MUL, ECS_MOD_SET } ecs_mod_op;
typedef enum ecs_stack_policy { ECS_STACK_NONE, ECS_STACK_REFRESH, ECS_STACK_STACK } ecs_stack_policy;

#define ECS_NO_STAT ((ecs_stat_col)-1)     /* an effect that only grants tags / carries a timer (a cooldown, a "rolling" state) */

typedef struct ecs_effect_def {
	const char *name;
	ecs_effect_kind kind;
	float duration;                 /* DURATION only */
	float period;                   /* > 0 on a DURATION/INFINITE effect: periodic */
	bool tick_on_apply;             /* periodic: also apply one tick immediately when it lands */
	ecs_stat_col stat;              /* column to modify, or ECS_NO_STAT */
	ecs_mod_op op;
	float magnitude;
	ecs_stack_policy stacking;
	int max_stacks;                 /* STACK policy cap (>= 1) */
	uint64_t grants_tags;           /* on the target while active */
	uint64_t requires_tags;         /* target must have ALL of these to be affected */
	uint64_t blocked_by_tags;       /* ... and NONE of these */
	uint64_t cancels_tags;          /* on landing, removes active effects whose granted tags intersect */
} ecs_effect_def;

typedef enum ecs_ability_result {
	ECS_ABILITY_OK,
	ECS_ABILITY_BAD_ENTITY,
	ECS_ABILITY_DEAD,
	ECS_ABILITY_MISSING_TAGS,
	ECS_ABILITY_BLOCKED,
	ECS_ABILITY_COOLDOWN,
	ECS_ABILITY_NO_RESOURCE
} ecs_ability_result;

typedef struct ecs_ability_def {
	const char *name;
	ecs_pool cost_pool;
	float cost;                     /* 0 = free */
	float cooldown;                 /* seconds, 0 = none */
	uint64_t required_tags;         /* on the user: ALL */
	uint64_t blocked_tags;          /* on the user: NONE */
	int effect_self;                /* effect def id applied to the user on activation, -1 none */
	int effect_target;              /* ... to the target, -1 none */
} ecs_ability_def;

typedef struct ecs_gas ecs_gas;

/* stats: the table effects act on (must outlive the gas). events: optional bus. */
ecs_gas *ecs_gas_new(ecs_stats *stats, ecs_events *events);
void ecs_gas_free(ecs_gas *g);

/* ---- tags ------------------------------------------------------------------------------------------------------------------ */
uint64_t ecs_gas_tag(ecs_gas *g, const char *name);            /* mask with one bit; same name -> same bit; 0 once 64 are used */
bool ecs_gas_has_tags(const ecs_gas *g, ecs_entity e, uint64_t all);
void ecs_gas_set_tags(ecs_gas *g, ecs_entity e, uint64_t tags);   /* permanent tags (replaces); effective = these | granted */

/* ---- effects ------------------------------------------------------------------------------------------------------------- */
int ecs_gas_add_effect(ecs_gas *g, const ecs_effect_def *def);    /* definition id (>= 0), -1 on failure */
/* Lands an effect on `target`. Returns false if the target is unknown, tag-gated, or NONE-stacking and already affected. */
bool ecs_gas_apply(ecs_gas *g, ecs_entity target, int effect, ecs_entity source);
int ecs_gas_remove(ecs_gas *g, ecs_entity target, int effect);    /* removes every instance; returns how many; undoes modifiers */
int ecs_gas_cancel_tags(ecs_gas *g, ecs_entity target, uint64_t tags);
int ecs_gas_stacks(const ecs_gas *g, ecs_entity target, int effect);   /* 0 if not active */
int ecs_gas_active_count(const ecs_gas *g);

/* ---- abilities ----------------------------------------------------------------------------------------------------------- */
int ecs_gas_add_ability(ecs_gas *g, const ecs_ability_def *def);  /* id >= 0, -1 on failure (e.g. out of tags for the cooldown) */
ecs_ability_result ecs_gas_can_activate(const ecs_gas *g, ecs_entity user, int ability);
ecs_ability_result ecs_gas_activate(ecs_gas *g, ecs_entity user, int ability, ecs_entity target);

/* ---- tick -------------------------------------------------------------------------------------------------------------- */
void ecs_gas_tick(ecs_gas *g, float dt);

#ifdef __cplusplus
}
#endif
#endif
