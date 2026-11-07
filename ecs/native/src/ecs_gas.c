/* ecs_gas.c - see ecs_gas.h. */
#include "ecs_gas.h"

#include <math.h>
#include <string.h>

#include "ecs_arena.h"
#include "ecs_math.h"
#include "ecs_mem.h"

typedef struct ability_rec {
	ecs_ability_def def;
	char *name;
	uint64_t cd_tag;          /* cooldown tag (0 if no cooldown) */
	int cd_effect;            /* internal effect def carrying the cooldown */
} ability_rec;

struct ecs_gas {
	ecs_stats *stats;
	ecs_events *events;

	ecs_effect_def *defs;
	char **def_names;
	int n_defs, cap_defs;

	ability_rec *abilities;
	int n_abilities, cap_abilities;

	char *tag_names[64];
	int n_tags;

	/* active effects, structure-of-arrays, dense (swap-remove) */
	int n, cap;
	ecs_entity *target, *source;
	int32_t *def, *stacks;
	float *remaining;         /* seconds left; +INF for INFINITE and INSTANT-never-stored */
	float *period_left;       /* seconds to the next periodic tick; +INF when not periodic */
	float *applied;           /* sum of modifier deltas this instance has applied (undone on removal) */

	ecs_arena *scratch;       /* per-tick index lists */
};

static char *dupstr(const char *s)
{
	size_t n = strlen(s) + 1;
	char *p = ecs_malloc(n);
	if (p) memcpy(p, s, n);
	return p;
}

ecs_gas *ecs_gas_new(ecs_stats *stats, ecs_events *events)
{
	if (!stats) return NULL;
	ecs_gas *g = ecs_calloc(1, sizeof *g);
	if (!g) return NULL;
	g->stats = stats;
	g->events = events;
	g->scratch = ecs_arena_new(8 * 1024);
	if (!g->scratch) { ecs_free(g); return NULL; }
	return g;
}

void ecs_gas_free(ecs_gas *g)
{
	if (!g) return;
	for (int i = 0; i < g->n_defs; i++) ecs_free(g->def_names[i]);
	for (int i = 0; i < g->n_abilities; i++) ecs_free(g->abilities[i].name);
	for (int i = 0; i < g->n_tags; i++) ecs_free(g->tag_names[i]);
	ecs_free(g->defs); ecs_free(g->def_names); ecs_free(g->abilities);
	ecs_free(g->target); ecs_free(g->source); ecs_free(g->def); ecs_free(g->stacks);
	ecs_free(g->remaining); ecs_free(g->period_left); ecs_free(g->applied);
	ecs_arena_free(g->scratch);
	ecs_free(g);
}

/* ---- tags ------------------------------------------------------------------------------------------------------------------- */

uint64_t ecs_gas_tag(ecs_gas *g, const char *name)
{
	for (int i = 0; i < g->n_tags; i++)
		if (strcmp(g->tag_names[i], name) == 0) return 1ull << i;
	if (g->n_tags >= 64) return 0;
	char *n = dupstr(name);
	if (!n) return 0;
	g->tag_names[g->n_tags] = n;
	return 1ull << g->n_tags++;
}

bool ecs_gas_has_tags(const ecs_gas *g, ecs_entity e, uint64_t all)
{
	int row = ecs_stats_row(g->stats, e);
	if (row < 0) return false;
	return (ecs_stats_tags(g->stats)[row] & all) == all;
}

/* effective = permanent | granted by the target's active effects */
static void recompute_tags(ecs_gas *g, ecs_entity target)
{
	int row = ecs_stats_row(g->stats, target);
	if (row < 0) return;
	uint64_t t = ecs_stats_tags_base(g->stats)[row];
	for (int i = 0; i < g->n; i++)
		if (g->target[i] == target) t |= g->defs[g->def[i]].grants_tags;
	ecs_stats_tags(g->stats)[row] = t;
}

void ecs_gas_set_tags(ecs_gas *g, ecs_entity e, uint64_t tags)
{
	int row = ecs_stats_row(g->stats, e);
	if (row < 0) return;
	ecs_stats_tags_base(g->stats)[row] = tags;
	recompute_tags(g, e);
}

/* ---- definitions ----------------------------------------------------------------------------------------------------------- */

int ecs_gas_add_effect(ecs_gas *g, const ecs_effect_def *d)
{
	if (!g || !d) return -1;
	if (g->n_defs == g->cap_defs) {
		int nc = g->cap_defs ? g->cap_defs * 2 : 16;
		ecs_effect_def *p = ecs_realloc(g->defs, (size_t)nc * sizeof *p);
		char **np = ecs_realloc(g->def_names, (size_t)nc * sizeof *np);
		if (p) g->defs = p;
		if (np) g->def_names = np;
		if (!p || !np) return -1;
		g->cap_defs = nc;
	}
	char *nm = dupstr(d->name ? d->name : "");
	if (!nm) return -1;
	ecs_effect_def c = *d;
	c.name = nm;
	if (c.max_stacks < 1) c.max_stacks = 1;
	g->defs[g->n_defs] = c;
	g->def_names[g->n_defs] = nm;
	return g->n_defs++;
}

/* ---- effect storage ------------------------------------------------------------------------------------------------------- */

static bool ensure(ecs_gas *g)
{
	if (g->n < g->cap) return true;
	int nc = g->cap ? g->cap * 2 : 64;
#define RG(a, T) do { T *p_ = ecs_realloc(g->a, (size_t)nc * sizeof(T)); if (!p_) return false; g->a = p_; } while (0)
	RG(target, ecs_entity); RG(source, ecs_entity); RG(def, int32_t); RG(stacks, int32_t);
	RG(remaining, float); RG(period_left, float); RG(applied, float);
#undef RG
	g->cap = nc;
	return true;
}

static int find(const ecs_gas *g, ecs_entity target, int def)
{
	for (int i = 0; i < g->n; i++)
		if (g->target[i] == target && g->def[i] == def) return i;
	return -1;
}

static float *stat_ptr(ecs_gas *g, ecs_stat_col col, int row)
{
	if ((int)col < 0 || col >= ECS_COL_COUNT) return NULL;
	float *c = ecs_stats_col(g->stats, col);
	return c ? &c[row] : NULL;
}

/* pools are clamped to [0,max]; lowering MAX drags CUR down with it */
static void clamp_pool(ecs_gas *g, ecs_stat_col col, int row)
{
	int c = (int)col;
	if (c >= (int)ECS_COL_CUR && c < (int)ECS_COL_CUR + ECS_POOL_COUNT) {
		int p = c - (int)ECS_COL_CUR;
		float *cur = stat_ptr(g, ECS_COL_CUR + p, row), *mx = stat_ptr(g, ECS_COL_MAX + p, row);
		*cur = *cur < 0.0f ? 0.0f : (*cur > *mx ? *mx : *cur);
	} else if (c >= (int)ECS_COL_MAX && c < (int)ECS_COL_MAX + ECS_POOL_COUNT) {
		int p = c - (int)ECS_COL_MAX;
		float *cur = stat_ptr(g, ECS_COL_CUR + p, row), *mx = stat_ptr(g, ECS_COL_MAX + p, row);
		if (*cur > *mx) *cur = *mx;
	}
}

static void emit_effect(ecs_gas *g, uint32_t type, ecs_entity target, ecs_entity source, int def, int stacks)
{
	if (!g->events) return;
	ecs_ev_effect ev = { target, source, def, stacks };
	ecs_event_emit(g->events, type, &ev);
}

/* one-shot application of the effect's op to the target's stat; returns the delta it applied (for later undo) */
static float apply_op(ecs_gas *g, const ecs_effect_def *d, int row, ecs_entity source, float scale)
{
	if (d->stat == ECS_NO_STAT) return 0.0f;
	float *v = stat_ptr(g, d->stat, row);
	if (!v) return 0.0f;
	const float before = *v;
	const int c = (int)d->stat;
	const bool is_cur = c >= (int)ECS_COL_CUR && c < (int)ECS_COL_CUR + ECS_POOL_COUNT;
	if (is_cur && d->op == ECS_MOD_ADD)                       /* pool changes go through the stat table's bookkeeping + events */
		return ecs_stats_apply_delta(g->stats, row, (ecs_pool)(c - (int)ECS_COL_CUR), d->magnitude * scale, source);
	switch (d->op) {
	case ECS_MOD_ADD: *v = before + d->magnitude * scale; break;
	case ECS_MOD_MUL: *v = before * (scale == 1.0f ? d->magnitude : powf(d->magnitude, scale)); break;
	case ECS_MOD_SET: *v = d->magnitude; break;
	}
	clamp_pool(g, d->stat, row);
	return *v - before;
}

static void remove_at(ecs_gas *g, int i, bool undo, bool announce)
{
	const ecs_effect_def *d = &g->defs[g->def[i]];
	ecs_entity target = g->target[i], source = g->source[i];
	int def = g->def[i], stacks = g->stacks[i];
	int row = ecs_stats_row(g->stats, target);
	if (undo && row >= 0 && d->stat != ECS_NO_STAT && d->period <= 0.0f && d->kind != ECS_EFFECT_INSTANT && g->applied[i] != 0.0f) {
		float *v = stat_ptr(g, d->stat, row);
		if (v) {
			*v -= g->applied[i];
			clamp_pool(g, d->stat, row);
		}
	}
	int last = --g->n;
	if (i != last) {
		g->target[i] = g->target[last]; g->source[i] = g->source[last]; g->def[i] = g->def[last];
		g->stacks[i] = g->stacks[last]; g->remaining[i] = g->remaining[last]; g->period_left[i] = g->period_left[last];
		g->applied[i] = g->applied[last];
	}
	if (announce) emit_effect(g, ECS_EV_EFFECT_EXPIRED, target, source, def, stacks);
	if (row >= 0) recompute_tags(g, target);
}

bool ecs_gas_apply(ecs_gas *g, ecs_entity target, int effect, ecs_entity source)
{
	if (!g || effect < 0 || effect >= g->n_defs) return false;
	int row = ecs_stats_row(g->stats, target);
	if (row < 0) return false;
	const ecs_effect_def *d = &g->defs[effect];
	const uint64_t tags = ecs_stats_tags(g->stats)[row];
	if ((tags & d->requires_tags) != d->requires_tags) return false;
	if (tags & d->blocked_by_tags) return false;

	if (d->cancels_tags) ecs_gas_cancel_tags(g, target, d->cancels_tags);

	if (d->kind == ECS_EFFECT_INSTANT) {
		apply_op(g, d, row, source, 1.0f);
		emit_effect(g, ECS_EV_EFFECT_APPLIED, target, source, effect, 1);
		return true;
	}

	const bool periodic = d->period > 0.0f;
	const float dur = d->kind == ECS_EFFECT_DURATION ? d->duration : INFINITY;
	int i = find(g, target, effect);
	if (i >= 0) {
		switch (d->stacking) {
		case ECS_STACK_NONE:
			return false;
		case ECS_STACK_REFRESH:
			g->remaining[i] = dur;
			g->source[i] = source;
			emit_effect(g, ECS_EV_EFFECT_APPLIED, target, source, effect, g->stacks[i]);
			return true;
		case ECS_STACK_STACK:
			if (g->stacks[i] < d->max_stacks) {
				g->stacks[i]++;
				if (!periodic) g->applied[i] += apply_op(g, d, row, source, 1.0f);
			}
			g->remaining[i] = dur;
			g->source[i] = source;
			emit_effect(g, ECS_EV_EFFECT_APPLIED, target, source, effect, g->stacks[i]);
			return true;
		}
	}

	if (!ensure(g)) return false;
	i = g->n++;
	g->target[i] = target;
	g->source[i] = source;
	g->def[i] = effect;
	g->stacks[i] = 1;
	g->remaining[i] = dur;
	g->period_left[i] = periodic ? d->period : INFINITY;
	g->applied[i] = 0.0f;
	if (!periodic) g->applied[i] = apply_op(g, d, row, source, 1.0f);
	else if (d->tick_on_apply) apply_op(g, d, row, source, 1.0f);
	recompute_tags(g, target);
	emit_effect(g, ECS_EV_EFFECT_APPLIED, target, source, effect, 1);
	return true;
}

int ecs_gas_remove(ecs_gas *g, ecs_entity target, int effect)
{
	int removed = 0;
	for (int i = g->n - 1; i >= 0; i--) {
		if (g->target[i] == target && g->def[i] == effect) {
			remove_at(g, i, true, true);
			removed++;
		}
	}
	return removed;
}

int ecs_gas_cancel_tags(ecs_gas *g, ecs_entity target, uint64_t tags)
{
	int removed = 0;
	for (int i = g->n - 1; i >= 0; i--) {
		if (g->target[i] == target && (g->defs[g->def[i]].grants_tags & tags)) {
			remove_at(g, i, true, true);
			removed++;
		}
	}
	return removed;
}

int ecs_gas_stacks(const ecs_gas *g, ecs_entity target, int effect)
{
	int i = find(g, target, effect);
	return i < 0 ? 0 : g->stacks[i];
}

int ecs_gas_active_count(const ecs_gas *g) { return g->n; }

/* ---- abilities -------------------------------------------------------------------------------------------------------------- */

int ecs_gas_add_ability(ecs_gas *g, const ecs_ability_def *def)
{
	if (!g || !def) return -1;
	if (g->n_abilities == g->cap_abilities) {
		int nc = g->cap_abilities ? g->cap_abilities * 2 : 16;
		ability_rec *p = ecs_realloc(g->abilities, (size_t)nc * sizeof *p);
		if (!p) return -1;
		g->abilities = p;
		g->cap_abilities = nc;
	}
	ability_rec r;
	memset(&r, 0, sizeof r);
	r.def = *def;
	r.name = dupstr(def->name ? def->name : "");
	r.cd_effect = -1;
	if (!r.name) return -1;
	r.def.name = r.name;
	if (def->cooldown > 0.0f) {
		char buf[96];
		size_t n = strlen(r.name);
		memcpy(buf, "cd.", 3);
		memcpy(buf + 3, r.name, n < 90 ? n : 90);
		buf[3 + (n < 90 ? n : 90)] = 0;
		r.cd_tag = ecs_gas_tag(g, buf);
		ecs_effect_def cd;
		memset(&cd, 0, sizeof cd);
		cd.name = buf;
		cd.kind = ECS_EFFECT_DURATION;
		cd.duration = def->cooldown;
		cd.stat = ECS_NO_STAT;
		cd.stacking = ECS_STACK_REFRESH;
		cd.grants_tags = r.cd_tag;
		r.cd_effect = ecs_gas_add_effect(g, &cd);
		if (r.cd_tag == 0 || r.cd_effect < 0) { ecs_free(r.name); return -1; }
	}
	g->abilities[g->n_abilities] = r;
	return g->n_abilities++;
}

ecs_ability_result ecs_gas_can_activate(const ecs_gas *g, ecs_entity user, int ability)
{
	if (ability < 0 || ability >= g->n_abilities) return ECS_ABILITY_BAD_ENTITY;
	const ability_rec *a = &g->abilities[ability];
	int row = ecs_stats_row(g->stats, user);
	if (row < 0) return ECS_ABILITY_BAD_ENTITY;
	if (ecs_stats_flags(g->stats)[row] & ECS_FLAG_DEAD) return ECS_ABILITY_DEAD;
	const uint64_t tags = ecs_stats_tags(g->stats)[row];
	if (a->cd_tag && (tags & a->cd_tag)) return ECS_ABILITY_COOLDOWN;
	if ((tags & a->def.required_tags) != a->def.required_tags) return ECS_ABILITY_MISSING_TAGS;
	if (tags & a->def.blocked_tags) return ECS_ABILITY_BLOCKED;
	if (a->def.cost > 0.0f) {
		float cur = ecs_stats_col(g->stats, ECS_COL_CUR + (int)a->def.cost_pool)[row];
		bool ok = a->def.cost_pool == ECS_POOL_SP ? cur > 0.0f : cur >= a->def.cost;     /* stamina: any > 0 may act */
		if (!ok) return ECS_ABILITY_NO_RESOURCE;
	}
	return ECS_ABILITY_OK;
}

ecs_ability_result ecs_gas_activate(ecs_gas *g, ecs_entity user, int ability, ecs_entity target)
{
	ecs_ability_result r = ecs_gas_can_activate(g, user, ability);
	if (r != ECS_ABILITY_OK) {
		if (g->events) {
			ecs_ev_ability ev = { user, target, ability, (int32_t)r };
			ecs_event_emit(g->events, ECS_EV_ABILITY_FAILED, &ev);
		}
		return r;
	}
	const ability_rec *a = &g->abilities[ability];
	int row = ecs_stats_row(g->stats, user);
	if (a->def.cost > 0.0f) ecs_stats_spend(g->stats, row, a->def.cost_pool, a->def.cost);
	if (a->cd_effect >= 0) ecs_gas_apply(g, user, a->cd_effect, user);
	if (a->def.effect_self >= 0) ecs_gas_apply(g, user, a->def.effect_self, user);
	if (a->def.effect_target >= 0 && target != ECS_NULL) ecs_gas_apply(g, target, a->def.effect_target, user);
	if (g->events) {
		ecs_ev_ability ev = { user, target, ability, 0 };
		ecs_event_emit(g->events, ECS_EV_ABILITY_ACTIVATED, &ev);
	}
	return ECS_ABILITY_OK;
}

/* ---- tick ------------------------------------------------------------------------------------------------------------------- */

ECS_KERNEL static void advance(float *remaining, float *period_left, float dt, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		remaining[i] = remaining[i] - dt;           /* +INF stays +INF */
		period_left[i] = period_left[i] - dt;
	}
}

void ecs_gas_tick(ecs_gas *g, float dt)
{
	if (g->n == 0 || dt <= 0.0f) return;
	const size_t n = (size_t)g->n;
	ecs_arena_mark mark = ecs_arena_mark_get(g->scratch);

	advance(g->remaining, g->period_left, dt, n);

	uint32_t *due = ecs_arena_alloc(g->scratch, n * sizeof *due);
	uint32_t *dead = ecs_arena_alloc(g->scratch, n * sizeof *dead);
	if (!due || !dead) { ecs_arena_rewind(g->scratch, mark); return; }

	/* periodic ticks first (a DoT that lands its last tick on the same frame it expires still counts) */
	size_t n_due = ecs_f32_select_le(due, g->period_left, 0.0f, n);
	for (size_t k = 0; k < n_due; k++) {
		int i = (int)due[k];
		const ecs_effect_def *d = &g->defs[g->def[i]];
		int row = ecs_stats_row(g->stats, g->target[i]);
		if (row < 0) continue;
		int guard = 4;                               /* catch up a few missed periods after a long step, not unboundedly */
		while (g->period_left[i] <= 0.0f && guard-- > 0) {
			apply_op(g, d, row, g->source[i], (float)g->stacks[i]);
			g->period_left[i] += d->period;
		}
		if (g->period_left[i] <= 0.0f) g->period_left[i] = d->period;
	}

	/* expiry (and effects whose target left the stats table), removed back to front so swap-remove never skips one */
	size_t n_dead = ecs_f32_select_le(dead, g->remaining, 0.0f, n);
	for (size_t k = n_dead; k-- > 0;) remove_at(g, (int)dead[k], true, true);
	for (int i = g->n - 1; i >= 0; i--)
		if (ecs_stats_row(g->stats, g->target[i]) < 0) remove_at(g, i, false, false);

	ecs_arena_rewind(g->scratch, mark);
}
