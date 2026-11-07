/* ecs_stats.h - souls-like combat stats as a dense structure-of-arrays table, built to be ticked with SIMD.
 *
 * WHY A SIDE TABLE AND NOT ONE ECS COMPONENT PER STAT
 *  An archetype column holds one struct per entity (array-of-structs). A regen tick wants "every entity's stamina" as ONE contiguous
 *  float array so the vector unit can do 8 entities per instruction. So the stats live here, column-per-field, all 32-byte aligned
 *  and padded; the ECS side just stores the entity handle. ecs_stats maps entity -> row (generation-checked, swap-remove keeps the
 *  rows dense), so the per-frame tick walks a gap-free array and never touches dead entities.
 *
 *  Columns are exposed raw (ecs_stats_col) so a host can hand them to something else without a copy - e.g. wrap a column as a
 *  PackedFloat32Array view in Godot, upload to a GPU buffer for a health-bar shader, or memcpy into a snapshot. A pointer is valid
 *  until the next ecs_stats_add (which may grow the table): re-fetch after adding.
 *
 * WHAT IT MODELS (the Dark/Demon's/Elden Ring family, deliberately engine-agnostic)
 *  Pools      HP, FP (mana/focus), SP (stamina), POISE - each with cur, max, regen/s, a regen DELAY after being spent/hit, and the
 *             running delay timer. SP regen pausing after an action and POISE recovering a while after the last hit are the same
 *             mechanism with different numbers.
 *  Load       equip weight vs. capacity -> a load ratio -> roll class (light/medium/heavy/overloaded) and an overload flag.
 *  Defense    per damage type: flat defense (soft curve, never fully negates) and a percentage resistance.
 *  Buildup    poison / bleed / frost / curse meters that fill from hits, decay over time, and when they cross their threshold trigger
 *             a timed status effect (with damage-over-time as a % of max HP) and reset - the Elden Ring status model.
 *  Attributes level -> derived max HP/FP/SP/load via piecewise soft-cap curves (all tunable in ecs_stats_config).
 *  Flags      DEAD, STAGGERED (poise broken), EXHAUSTED (SP empty), OVERLOADED, and one per active status - recomputed every tick as
 *             a pure function of the columns, so they can never go stale.
 *
 * Not thread-safe; tick it from the same thread (or job) that owns the world. Deterministic: see ecs_math.h. */
#ifndef ECS_STATS_H
#define ECS_STATS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ecs.h"
#include "ecs_event.h"
#include "ecs_math.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ecs_pool {
	ECS_POOL_HP,
	ECS_POOL_FP,
	ECS_POOL_SP,
	ECS_POOL_POISE,
	ECS_POOL_COUNT
} ecs_pool;

typedef enum ecs_dmg_type {
	ECS_DMG_PHYSICAL,
	ECS_DMG_MAGIC,
	ECS_DMG_FIRE,
	ECS_DMG_LIGHTNING,
	ECS_DMG_HOLY,
	ECS_DMG_COUNT
} ecs_dmg_type;

typedef enum ecs_buildup {
	ECS_BUILDUP_POISON,
	ECS_BUILDUP_BLEED,
	ECS_BUILDUP_FROST,
	ECS_BUILDUP_CURSE,
	ECS_BUILDUP_COUNT
} ecs_buildup;

enum {
	ECS_FLAG_DEAD       = 1u << 0,
	ECS_FLAG_STAGGERED  = 1u << 1,   /* poise broken: game code plays the stagger, then calls ecs_stats_reset_poise */
	ECS_FLAG_EXHAUSTED  = 1u << 2,   /* SP at zero */
	ECS_FLAG_OVERLOADED = 1u << 3,   /* carrying more than capacity */
	ECS_FLAG_POISONED   = 1u << 4,   /* ECS_FLAG_POISONED << ecs_buildup, i.e. one bit per buildup type, in enum order */
	ECS_FLAG_BLEEDING   = 1u << 5,
	ECS_FLAG_FROZEN     = 1u << 6,
	ECS_FLAG_CURSED     = 1u << 7
};

/* Three classes, as in the games: light / medium / heavy. Carrying MORE than capacity is not a fourth class here - it stays in HEAVY
 * and raises ECS_FLAG_OVERLOADED, which the game can use to forbid rolling/sprinting or slow movement. */
typedef enum ecs_roll_class {
	ECS_ROLL_LIGHT,
	ECS_ROLL_MEDIUM,
	ECS_ROLL_HEAVY,
	ECS_ROLL_COUNT
} ecs_roll_class;

/* Flat column index into the table's float columns. Use the helpers below rather than doing the arithmetic by hand. */
typedef enum ecs_stat_col {
	ECS_COL_CUR      = 0,                                       /* + ecs_pool */
	ECS_COL_MAX      = ECS_COL_CUR + ECS_POOL_COUNT,            /* + ecs_pool */
	ECS_COL_REGEN    = ECS_COL_MAX + ECS_POOL_COUNT,            /* + ecs_pool: units per second (may be negative: a drain) */
	ECS_COL_DELAY    = ECS_COL_REGEN + ECS_POOL_COUNT,          /* + ecs_pool: seconds regen pauses after a spend/hit */
	ECS_COL_TIMER    = ECS_COL_DELAY + ECS_POOL_COUNT,          /* + ecs_pool: seconds left until regen resumes */
	ECS_COL_BU_VALUE = ECS_COL_TIMER + ECS_POOL_COUNT,          /* + ecs_buildup */
	ECS_COL_BU_THRESH = ECS_COL_BU_VALUE + ECS_BUILDUP_COUNT,   /* + ecs_buildup */
	ECS_COL_BU_DECAY = ECS_COL_BU_THRESH + ECS_BUILDUP_COUNT,   /* + ecs_buildup: meter drain per second */
	ECS_COL_BU_ACTIVE = ECS_COL_BU_DECAY + ECS_BUILDUP_COUNT,   /* + ecs_buildup: seconds of the triggered status left */
	ECS_COL_DEFENSE  = ECS_COL_BU_ACTIVE + ECS_BUILDUP_COUNT,   /* + ecs_dmg_type */
	ECS_COL_RESIST   = ECS_COL_DEFENSE + ECS_DMG_COUNT,         /* + ecs_dmg_type: 0..1 fraction removed after defense */
	ECS_COL_ATK      = ECS_COL_RESIST + ECS_DMG_COUNT,          /* + ecs_dmg_type: attack rating per damage type */
	ECS_COL_ATK_POISE = ECS_COL_ATK + ECS_DMG_COUNT,            /* poise damage of the equipped weapon */
	ECS_COL_ATK_BU   = ECS_COL_ATK_POISE + 1,                   /* + ecs_buildup: buildup the weapon inflicts per hit */
	ECS_COL_LOAD     = ECS_COL_ATK_BU + ECS_BUILDUP_COUNT,
	ECS_COL_LOAD_MAX,
	ECS_COL_COUNT
} ecs_stat_col;

/* Tuning: defaults for new rows + the world-wide rules. Pass NULL to ecs_stats_new for ecs_stats_config_default(). */
typedef struct ecs_stats_config {
	float pool_regen[ECS_POOL_COUNT];
	float pool_delay[ECS_POOL_COUNT];
	/* Multiplier on the regen delay when a spend leaves the pool EMPTY. Stamina in the souls games recovers a beat later after you
	 * bottom it out than after a normal swing - the penalty for exhausting yourself. 1.0 = no penalty. */
	float pool_exhaust_delay_mult[ECS_POOL_COUNT];
	float bu_threshold[ECS_BUILDUP_COUNT];
	float bu_decay[ECS_BUILDUP_COUNT];
	float bu_duration[ECS_BUILDUP_COUNT];         /* seconds the status lasts once triggered */
	float bu_dot_pct_max_hp[ECS_BUILDUP_COUNT];   /* HP drained per second while active, as a fraction of max HP */
	float roll_light_max;                         /* load ratio below which you roll light (default 0.30) */
	float roll_medium_max;                        /* ... medium (0.70); up to 1.0 heavy; above = overloaded */
} ecs_stats_config;

/* Character level inputs. Levels are plain ints; the curves are the soft-cap shape every souls-like table has. */
typedef struct ecs_attributes {
	int vigor, mind, endurance, strength, dexterity, intelligence, faith, arcane;
} ecs_attributes;

/* A weapon: base damage per type, how much each attribute scales it (0..1+, fraction of base added at full scaling), poise damage,
 * and the buildup it inflicts. Scaling uses the same soft-cap shape as the derive curves (see ecs_stats_scaling). */
typedef struct ecs_weapon {
	float base[ECS_DMG_COUNT];
	float scale_str, scale_dex, scale_int, scale_fai, scale_arc;
	float poise_damage;
	float buildup[ECS_BUILDUP_COUNT];
} ecs_weapon;

/* One attack, ready to land: what ecs_stats_make_attack builds from an attacker's columns and ecs_stats_apply_attack resolves. */
typedef struct ecs_attack {
	float damage[ECS_DMG_COUNT];
	float poise_damage;
	float buildup[ECS_BUILDUP_COUNT];
	ecs_entity source;
	bool crit;                       /* a crit multiplies damage x crit_mult (stats config) and poise x the same */
} ecs_attack;

/* What a hit did, for animation/audio/AI to react to. */
typedef struct ecs_hit_result {
	float dealt;                     /* HP removed */
	float overkill;                  /* damage beyond what was left */
	bool killed, staggered;
	uint32_t status_triggered;       /* bitmask of ecs_buildup types that crossed their threshold */
} ecs_hit_result;

typedef struct ecs_stats ecs_stats;

ecs_stats_config ecs_stats_config_default(void);
ecs_stats *ecs_stats_new(const ecs_stats_config *cfg, int capacity_hint);   /* cfg is copied; NULL = defaults */
void ecs_stats_free(ecs_stats *s);

/* ---- membership -------------------------------------------------------------------------------------------------------------- */
int ecs_stats_add(ecs_stats *s, ecs_entity e);        /* row (existing row if already present), -1 on a null handle / OOM */
void ecs_stats_remove(ecs_stats *s, ecs_entity e);    /* swap-removes the row; no-op if absent */
int ecs_stats_row(const ecs_stats *s, ecs_entity e);  /* -1 if absent */
int ecs_stats_count(const ecs_stats *s);
const ecs_entity *ecs_stats_entities(const ecs_stats *s);   /* count handles, parallel to every column */
/* Drops rows whose entity is no longer alive in `w` (the core has no destroy-observers yet). Returns how many were dropped. */
int ecs_stats_collect(ecs_stats *s, const ecs_world *w);

/* ---- raw columns (zero-copy, SIMD-friendly) --------------------------------------------------------------------------------- */
float *ecs_stats_col(ecs_stats *s, ecs_stat_col col);        /* ecs_stats_count() valid floats, 32-byte aligned, padded */
uint32_t *ecs_stats_flags(ecs_stats *s);

/* ---- per-frame bulk tick ---------------------------------------------------------------------------------------------------- */
/* Advances every row by dt seconds: delay timers, regen (gated by the timer, clamped to [0,max]), buildup decay, status timers and
 * damage-over-time, then recomputes all flags. One pass of vector loops over the columns; no per-entity branching on the hot path. */
void ecs_stats_tick(ecs_stats *s, float dt);

/* ---- per-entity actions (events: a hit landed, an action was performed) ------------------------------------------------------ */
/* Applies a hit. amounts is indexed by ecs_dmg_type. Per type: reduced by the defense curve (attack^2 / (attack + defense)), then by
 * resist. Total goes off HP; poise_damage goes off POISE; both restart their regen delay. Returns HP actually removed (post-clamp). */
float ecs_stats_damage(ecs_stats *s, int row, const float amounts[ECS_DMG_COUNT], float poise_damage);
/* Same maths without applying it: what ecs_stats_damage would take off HP (UI previews, AI damage estimates). */
float ecs_stats_damage_preview(const ecs_stats *s, int row, const float amounts[ECS_DMG_COUNT]);
/* Spend from a pool (an attack's SP cost, a spell's FP). Souls rule: you may act on any SP > 0 and it floors at 0; returns false
 * with no change when the pool is already empty. Restarts that pool's regen delay. */
bool ecs_stats_spend(ecs_stats *s, int row, ecs_pool pool, float amount);
void ecs_stats_restore(ecs_stats *s, int row, ecs_pool pool, float amount);         /* heal / flask: clamps to max, no delay */
void ecs_stats_reset_poise(ecs_stats *s, int row);                                  /* back to max, e.g. after a stagger animation */
/* Fills a buildup meter. Returns true if it crossed its threshold: the meter resets and the timed status starts. */
bool ecs_stats_add_buildup(ecs_stats *s, int row, ecs_buildup type, float amount);

/* ---- attack & damage ------------------------------------------------------------------------------------------------------- */
/* Scaling factor 0..1 for an attribute level: 0 at level 0, 0.5 around level 20, 1.0 at level 60, creeping beyond. */
float ecs_stats_scaling(int level);
/* Computes the row's attack-rating columns from a weapon and the wielder's attributes:
 *   AR[type] = base[type] * (1 + k[type]), where with f = ecs_stats_scaling(level):
 *     physical  k = scale_str*f(STR) + scale_dex*f(DEX)
 *     magic     k = scale_int*f(INT)
 *     fire      k = 0.5*scale_str*f(STR) + 0.5*scale_fai*f(FAI)
 *     lightning k = 0.5*scale_dex*f(DEX) + 0.5*scale_arc*f(ARC)
 *     holy      k = scale_fai*f(FAI) */
void ecs_stats_set_weapon(ecs_stats *s, int row, const ecs_weapon *w, const ecs_attributes *a);
/* Builds an attack from the row's ATK columns. motion scales damage and poise (light 1.0, heavy 1.6, charged 2.2...). */
ecs_attack ecs_stats_make_attack(const ecs_stats *s, int row, float motion);
/* Resolves an attack against a target row: defense/resist, HP, poise, buildup, then events (if a bus is attached). */
ecs_hit_result ecs_stats_apply_attack(ecs_stats *s, int target_row, const ecs_attack *atk);
/* Convenience: attacker row -> target row in one call. */
ecs_hit_result ecs_stats_hit(ecs_stats *s, int attacker_row, int target_row, float motion);
/* Raw change to a pool, bypassing defense/resist (a DoT tick, a heal over time, a drain). Clamped to [0,max]. Does the same
 * bookkeeping as a hit - death/stagger/exhaust flags and events, attributed to `source` - so a poison that kills reports who
 * poisoned. Returns the signed change actually applied. */
float ecs_stats_apply_delta(ecs_stats *s, int row, ecs_pool pool, float delta, ecs_entity source);
/* Multiplier applied to damage and poise on a crit (default 1.8). */
void ecs_stats_set_crit_mult(ecs_stats *s, float mult);

/* ---- events -------------------------------------------------------------------------------------------------------------------- */
/* Attach a bus and the table emits ECS_EV_DAMAGE / DEATH / STAGGER / EXHAUSTED / STATUS_APPLIED / STATUS_ENDED. Actions emit when they
 * happen; the tick emits for transitions it caused (a poison tick that kills emits DEATH). Pass NULL to detach. */
void ecs_stats_attach_events(ecs_stats *s, ecs_events *ev);

/* ---- tags (gameplay tag masks, the substrate ecs_gas builds on) ------------------------------------------------------------- */
uint64_t *ecs_stats_tags(ecs_stats *s);          /* effective tags: base | whatever active effects grant. Read these. */
uint64_t *ecs_stats_tags_base(ecs_stats *s);     /* permanent tags set by game code */
/* The pools' regen-delay multiplier on empty can also be changed at runtime (e.g. a ring that softens the exhaustion penalty). */
void ecs_stats_set_exhaust_mult(ecs_stats *s, ecs_pool pool, float mult);
const ecs_stats_config *ecs_stats_get_config(const ecs_stats *s);

/* ---- load, attributes ------------------------------------------------------------------------------------------------------- */
void ecs_stats_set_load(ecs_stats *s, int row, float carried, float capacity);
float ecs_stats_load_ratio(const ecs_stats *s, int row);
ecs_roll_class ecs_stats_roll_class(const ecs_stats *s, int row);
/* Sets max HP/FP/SP and load capacity from attribute levels and fills the pools to the new max. */
void ecs_stats_derive(ecs_stats *s, int row, const ecs_attributes *attr);

#ifdef __cplusplus
}
#endif
#endif
