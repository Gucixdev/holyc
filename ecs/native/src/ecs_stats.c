/* ecs_stats.c - see ecs_stats.h. */
#include "ecs_stats.h"

#include <math.h>
#include <string.h>

#include "ecs_math.h"
#include "ecs_mem.h"

#define ECS_INDEX_MASK 0xFFFFFFu   /* must match ecs.c: entity = generation << 24 | index */

struct ecs_stats {
	ecs_stats_config cfg;
	int count, cap;                 /* rows used / allocated (cap is a multiple of ECS_LANES) */
	float *col[ECS_COL_COUNT];
	uint32_t *flags;
	uint32_t *prev_flags;           /* flags as of the last edge pass: transitions between the two become events */
	uint64_t *tags, *tags_base;     /* gameplay tag masks: effective / permanent */
	ecs_events *events;
	float crit_mult;
	ecs_entity *ent;
	int32_t *sparse;                /* entity index -> row, -1 absent */
	int sparse_cap;
};

ecs_stats_config ecs_stats_config_default(void)
{
	ecs_stats_config c;
	memset(&c, 0, sizeof c);
	for (int i = 0; i < ECS_POOL_COUNT; i++) c.pool_exhaust_delay_mult[i] = 1.0f;
	c.pool_exhaust_delay_mult[ECS_POOL_SP] = 1.5f;     /* bottoming out stamina costs a beat more before it recovers */
	c.pool_regen[ECS_POOL_SP] = 35.0f;
	c.pool_delay[ECS_POOL_SP] = 1.0f;
	c.pool_regen[ECS_POOL_POISE] = 25.0f;
	c.pool_delay[ECS_POOL_POISE] = 6.0f;
	for (int i = 0; i < ECS_BUILDUP_COUNT; i++) {
		c.bu_threshold[i] = 100.0f;
		c.bu_decay[i] = 4.0f;
		c.bu_duration[i] = 20.0f;
	}
	c.bu_dot_pct_max_hp[ECS_BUILDUP_POISON] = 0.010f;     /* 1% max HP / s */
	c.bu_dot_pct_max_hp[ECS_BUILDUP_BLEED] = 0.0f;        /* bleed is a burst (see ecs_stats_add_buildup), no DoT */
	c.bu_duration[ECS_BUILDUP_BLEED] = 0.5f;
	c.bu_duration[ECS_BUILDUP_FROST] = 15.0f;
	c.bu_duration[ECS_BUILDUP_CURSE] = 60.0f;
	c.roll_light_max = 0.30f;
	c.roll_medium_max = 0.70f;
	return c;
}

static size_t pad(size_t n) { return (n + ECS_LANES - 1) & ~(size_t)(ECS_LANES - 1); }

static bool grow(ecs_stats *s, int want)
{
	if (want <= s->cap) return true;
	int ncap = s->cap ? s->cap : ECS_LANES;
	while (ncap < want) ncap *= 2;
	ncap = (int)pad((size_t)ncap);
	for (int c = 0; c < ECS_COL_COUNT; c++) {
		float *p = ecs_malloc_aligned((size_t)ncap * sizeof(float), ECS_SIMD_ALIGN);
		if (!p) return false;
		memset(p, 0, (size_t)ncap * sizeof(float));
		if (s->col[c]) {
			memcpy(p, s->col[c], (size_t)s->count * sizeof(float));
			ecs_free_aligned(s->col[c]);
		}
		s->col[c] = p;
	}
	uint32_t *f = ecs_malloc_aligned((size_t)ncap * sizeof(uint32_t), ECS_SIMD_ALIGN);
	if (!f) return false;
	memset(f, 0, (size_t)ncap * sizeof(uint32_t));
	if (s->flags) {
		memcpy(f, s->flags, (size_t)s->count * sizeof(uint32_t));
		ecs_free_aligned(s->flags);
	}
	s->flags = f;
	uint32_t *pf = ecs_malloc_aligned((size_t)ncap * sizeof(uint32_t), ECS_SIMD_ALIGN);
	uint64_t *tg = ecs_malloc_aligned((size_t)ncap * sizeof(uint64_t), ECS_SIMD_ALIGN);
	uint64_t *tb = ecs_malloc_aligned((size_t)ncap * sizeof(uint64_t), ECS_SIMD_ALIGN);
	if (!pf || !tg || !tb) {
		ecs_free_aligned(pf); ecs_free_aligned(tg); ecs_free_aligned(tb);
		return false;
	}
	memset(pf, 0, (size_t)ncap * sizeof(uint32_t));
	memset(tg, 0, (size_t)ncap * sizeof(uint64_t));
	memset(tb, 0, (size_t)ncap * sizeof(uint64_t));
	if (s->prev_flags) {
		memcpy(pf, s->prev_flags, (size_t)s->count * sizeof(uint32_t));
		memcpy(tg, s->tags, (size_t)s->count * sizeof(uint64_t));
		memcpy(tb, s->tags_base, (size_t)s->count * sizeof(uint64_t));
		ecs_free_aligned(s->prev_flags); ecs_free_aligned(s->tags); ecs_free_aligned(s->tags_base);
	}
	s->prev_flags = pf;
	s->tags = tg;
	s->tags_base = tb;
	ecs_entity *e = ecs_realloc(s->ent, (size_t)ncap * sizeof(ecs_entity));
	if (!e) return false;
	s->ent = e;
	s->cap = ncap;
	return true;
}

ecs_stats *ecs_stats_new(const ecs_stats_config *cfg, int capacity_hint)
{
	ecs_stats *s = ecs_calloc(1, sizeof *s);
	if (!s) return NULL;
	s->cfg = cfg ? *cfg : ecs_stats_config_default();
	s->crit_mult = 1.8f;
	if (!grow(s, capacity_hint > 0 ? capacity_hint : 64)) {
		ecs_stats_free(s);
		return NULL;
	}
	return s;
}

void ecs_stats_free(ecs_stats *s)
{
	if (!s) return;
	for (int c = 0; c < ECS_COL_COUNT; c++) ecs_free_aligned(s->col[c]);
	ecs_free_aligned(s->flags);
	ecs_free_aligned(s->prev_flags);
	ecs_free_aligned(s->tags);
	ecs_free_aligned(s->tags_base);
	ecs_free(s->ent);
	ecs_free(s->sparse);
	ecs_free(s);
}

static float *C(const ecs_stats *s, ecs_stat_col base, int k) { return s->col[(int)base + k]; }

static bool sparse_ensure(ecs_stats *s, uint32_t idx)
{
	if ((int)idx < s->sparse_cap) return true;
	int n = s->sparse_cap ? s->sparse_cap : 256;
	while (n <= (int)idx) n *= 2;
	int32_t *p = ecs_realloc(s->sparse, (size_t)n * sizeof *p);
	if (!p) return false;
	for (int i = s->sparse_cap; i < n; i++) p[i] = -1;
	s->sparse = p;
	s->sparse_cap = n;
	return true;
}

int ecs_stats_row(const ecs_stats *s, ecs_entity e)
{
	uint32_t idx = e & ECS_INDEX_MASK;
	if (e == ECS_NULL || (int)idx >= s->sparse_cap) return -1;
	int r = s->sparse[idx];
	if (r < 0 || s->ent[r] != e) return -1;       /* slot reused by a newer generation: not the same entity */
	return r;
}

static void init_row(ecs_stats *s, int r)
{
	const ecs_stats_config *c = &s->cfg;
	for (int col = 0; col < ECS_COL_COUNT; col++) s->col[col][r] = 0.0f;
	s->flags[r] = 0;
	s->prev_flags[r] = 0;
	s->tags[r] = 0;
	s->tags_base[r] = 0;
	for (int p = 0; p < ECS_POOL_COUNT; p++) {
		C(s, ECS_COL_REGEN, p)[r] = c->pool_regen[p];
		C(s, ECS_COL_DELAY, p)[r] = c->pool_delay[p];
	}
	for (int b = 0; b < ECS_BUILDUP_COUNT; b++) {
		C(s, ECS_COL_BU_THRESH, b)[r] = c->bu_threshold[b];
		C(s, ECS_COL_BU_DECAY, b)[r] = c->bu_decay[b];
	}
}

int ecs_stats_add(ecs_stats *s, ecs_entity e)
{
	if (e == ECS_NULL) return -1;
	int r = ecs_stats_row(s, e);
	if (r >= 0) return r;
	if (!grow(s, s->count + 1) || !sparse_ensure(s, e & ECS_INDEX_MASK)) return -1;
	r = s->count++;
	s->ent[r] = e;
	s->sparse[e & ECS_INDEX_MASK] = r;
	init_row(s, r);
	return r;
}

static void remove_row(ecs_stats *s, int r)
{
	int last = s->count - 1;
	s->sparse[s->ent[r] & ECS_INDEX_MASK] = -1;
	if (r != last) {
		for (int c = 0; c < ECS_COL_COUNT; c++) s->col[c][r] = s->col[c][last];
		s->flags[r] = s->flags[last];
		s->prev_flags[r] = s->prev_flags[last];
		s->tags[r] = s->tags[last];
		s->tags_base[r] = s->tags_base[last];
		s->ent[r] = s->ent[last];
		s->sparse[s->ent[r] & ECS_INDEX_MASK] = r;
	}
	s->count--;
	/* keep the padding tail zeroed so full-width vector loops over [0, pad(count)) read clean values */
	for (int c = 0; c < ECS_COL_COUNT; c++) s->col[c][last] = 0.0f;
	s->flags[last] = 0;
	s->prev_flags[last] = 0;
	s->tags[last] = 0;
	s->tags_base[last] = 0;
}

void ecs_stats_remove(ecs_stats *s, ecs_entity e)
{
	int r = ecs_stats_row(s, e);
	if (r >= 0) remove_row(s, r);
}

int ecs_stats_count(const ecs_stats *s) { return s->count; }
const ecs_entity *ecs_stats_entities(const ecs_stats *s) { return s->ent; }

int ecs_stats_collect(ecs_stats *s, const ecs_world *w)
{
	int dropped = 0;
	for (int r = s->count - 1; r >= 0; r--) {
		if (!ecs_alive(w, s->ent[r])) {
			remove_row(s, r);
			dropped++;
		}
	}
	return dropped;
}

float *ecs_stats_col(ecs_stats *s, ecs_stat_col col) { return (col >= 0 && col < ECS_COL_COUNT) ? s->col[col] : NULL; }
uint32_t *ecs_stats_flags(ecs_stats *s) { return s->flags; }

/* ---- tick ------------------------------------------------------------------------------------------------------------------- */

static void emit_entity(ecs_stats *s, int type, int row, ecs_entity source)
{
	ecs_ev_entity ev = { s->ent[row], source };
	ecs_event_emit(s->events, (uint32_t)type, &ev);
}

static void emit_status(ecs_stats *s, int type, int row, int b)
{
	ecs_ev_status ev = { s->ent[row], (uint32_t)b };
	ecs_event_emit(s->events, (uint32_t)type, &ev);
}

/* Turns flag transitions since the last pass into events. Cheap when nothing changed: one xor per row. */
static void edge_events(ecs_stats *s)
{
	const int n = s->count;
	for (int i = 0; i < n; i++) {
		uint32_t d = s->flags[i] ^ s->prev_flags[i];
		if (!d) continue;
		uint32_t now = s->flags[i];
		if ((d & ECS_FLAG_DEAD) && (now & ECS_FLAG_DEAD)) emit_entity(s, ECS_EV_DEATH, i, ECS_NULL);
		if ((d & ECS_FLAG_STAGGERED) && (now & ECS_FLAG_STAGGERED)) emit_entity(s, ECS_EV_STAGGER, i, ECS_NULL);
		if ((d & ECS_FLAG_EXHAUSTED) && (now & ECS_FLAG_EXHAUSTED)) emit_entity(s, ECS_EV_EXHAUSTED, i, ECS_NULL);
		for (int b = 0; b < ECS_BUILDUP_COUNT; b++) {
			uint32_t bit = (uint32_t)ECS_FLAG_POISONED << b;
			if (!(d & bit)) continue;
			emit_status(s, (now & bit) ? ECS_EV_STATUS_APPLIED : ECS_EV_STATUS_ENDED, i, b);
		}
		s->prev_flags[i] = now;
	}
}

ECS_KERNEL static void tick_columns(ecs_stats *s, float dt)
{
	const size_t n = (size_t)s->count;
	if (n == 0 || dt <= 0.0f) return;

	/* Pools: timer counts down; once it has expired the pool regenerates, clamped to [0, max]. The select is written as a ternary
	 * on the value so the compiler emits a blend, not a branch. */
	for (int p = 0; p < ECS_POOL_COUNT; p++) {
		float *cur = C(s, ECS_COL_CUR, p), *mx = C(s, ECS_COL_MAX, p), *rg = C(s, ECS_COL_REGEN, p), *tm = C(s, ECS_COL_TIMER, p);
		for (size_t i = 0; i < n; i++) {
			float t = tm[i] - dt;
			t = t > 0.0f ? t : 0.0f;
			tm[i] = t;
			float add = t <= 0.0f ? rg[i] * dt : 0.0f;
			float v = cur[i] + add;
			v = v < 0.0f ? 0.0f : v;
			cur[i] = v > mx[i] ? mx[i] : v;
		}
	}

	/* Buildup meters drain; triggered-status timers run down. */
	for (int b = 0; b < ECS_BUILDUP_COUNT; b++) {
		float *val = C(s, ECS_COL_BU_VALUE, b), *dec = C(s, ECS_COL_BU_DECAY, b), *act = C(s, ECS_COL_BU_ACTIVE, b);
		for (size_t i = 0; i < n; i++) {
			float v = val[i] - dec[i] * dt;
			val[i] = v > 0.0f ? v : 0.0f;
			float a = act[i] - dt;
			act[i] = a > 0.0f ? a : 0.0f;
		}
	}

	/* Damage over time: while a status is active it drains a fraction of max HP per second (floors at 0, never heals). */
	{
		float *hp = C(s, ECS_COL_CUR, ECS_POOL_HP), *hpmax = C(s, ECS_COL_MAX, ECS_POOL_HP);
		for (int b = 0; b < ECS_BUILDUP_COUNT; b++) {
			const float pct = s->cfg.bu_dot_pct_max_hp[b];
			if (pct <= 0.0f) continue;
			float *act = C(s, ECS_COL_BU_ACTIVE, b);
			for (size_t i = 0; i < n; i++) {
				float d = act[i] > 0.0f ? hpmax[i] * pct * dt : 0.0f;
				float v = hp[i] - d;
				hp[i] = v > 0.0f ? v : 0.0f;
			}
		}
	}

	/* Flags: a pure function of the columns, rebuilt every tick. */
	{
		const float *hp = C(s, ECS_COL_CUR, ECS_POOL_HP), *poise = C(s, ECS_COL_CUR, ECS_POOL_POISE);
		const float *sp = C(s, ECS_COL_CUR, ECS_POOL_SP), *spmax = C(s, ECS_COL_MAX, ECS_POOL_SP);
		const float *poisemax = C(s, ECS_COL_MAX, ECS_POOL_POISE), *hpmax = C(s, ECS_COL_MAX, ECS_POOL_HP);
		const float *load = s->col[ECS_COL_LOAD], *lmax = s->col[ECS_COL_LOAD_MAX];
		const float *act[ECS_BUILDUP_COUNT];
		for (int b = 0; b < ECS_BUILDUP_COUNT; b++) act[b] = C(s, ECS_COL_BU_ACTIVE, b);
		uint32_t *f = s->flags;
		for (size_t i = 0; i < n; i++) {
			uint32_t v = 0;
			v |= (hpmax[i] > 0.0f && hp[i] <= 0.0f) ? ECS_FLAG_DEAD : 0u;
			v |= (poisemax[i] > 0.0f && poise[i] <= 0.0f) ? ECS_FLAG_STAGGERED : 0u;
			v |= (spmax[i] > 0.0f && sp[i] <= 0.0f) ? ECS_FLAG_EXHAUSTED : 0u;
			v |= (lmax[i] > 0.0f && load[i] > lmax[i]) ? ECS_FLAG_OVERLOADED : 0u;
			for (int b = 0; b < ECS_BUILDUP_COUNT; b++) v |= act[b][i] > 0.0f ? ((uint32_t)ECS_FLAG_POISONED << b) : 0u;
			f[i] = v;
		}
	}
}

void ecs_stats_tick(ecs_stats *s, float dt)
{
	if (s->count == 0 || dt <= 0.0f) return;
	tick_columns(s, dt);
	if (s->events) edge_events(s);
}

/* ---- actions ---------------------------------------------------------------------------------------------------------------- */

static float mitigate(float attack, float defense, float resist)
{
	if (attack <= 0.0f) return 0.0f;
	float d = defense > 0.0f ? defense : 0.0f;
	float after = attack * attack / (attack + d);             /* defense bends the hit, can never fully negate it */
	float r = resist < 0.0f ? 0.0f : (resist > 1.0f ? 1.0f : resist);
	return after * (1.0f - r);
}

float ecs_stats_damage_preview(const ecs_stats *s, int row, const float amounts[ECS_DMG_COUNT])
{
	if (row < 0 || row >= s->count) return 0.0f;
	float total = 0.0f;
	for (int t = 0; t < ECS_DMG_COUNT; t++)
		total += mitigate(amounts[t], C(s, ECS_COL_DEFENSE, t)[row], C(s, ECS_COL_RESIST, t)[row]);
	float hp = C(s, ECS_COL_CUR, ECS_POOL_HP)[row];
	return total < hp ? total : hp;
}

static ecs_hit_result resolve(ecs_stats *s, int row, const float amounts[ECS_DMG_COUNT], float poise_damage, ecs_entity source,
			      bool crit, const float *buildup)
{
	ecs_hit_result r = { 0 };
	if (row < 0 || row >= s->count) return r;
	const float mult = crit ? s->crit_mult : 1.0f;
	float total = 0.0f;
	for (int t = 0; t < ECS_DMG_COUNT; t++)
		total += mitigate(amounts[t] * mult, C(s, ECS_COL_DEFENSE, t)[row], C(s, ECS_COL_RESIST, t)[row]);
	float *hp = &C(s, ECS_COL_CUR, ECS_POOL_HP)[row];
	const bool was_dead = *hp <= 0.0f && C(s, ECS_COL_MAX, ECS_POOL_HP)[row] > 0.0f;
	r.dealt = total < *hp ? total : *hp;
	r.overkill = total - r.dealt;
	*hp -= r.dealt;
	if (r.dealt > 0.0f) C(s, ECS_COL_TIMER, ECS_POOL_HP)[row] = C(s, ECS_COL_DELAY, ECS_POOL_HP)[row];

	float *po = &C(s, ECS_COL_CUR, ECS_POOL_POISE)[row];
	const bool was_staggered = (s->flags[row] & ECS_FLAG_STAGGERED) != 0;
	if (poise_damage > 0.0f) {
		*po -= poise_damage * mult;
		if (*po < 0.0f) *po = 0.0f;
		C(s, ECS_COL_TIMER, ECS_POOL_POISE)[row] = C(s, ECS_COL_DELAY, ECS_POOL_POISE)[row];
	}
	r.killed = !was_dead && *hp <= 0.0f && C(s, ECS_COL_MAX, ECS_POOL_HP)[row] > 0.0f;
	r.staggered = !was_staggered && *po <= 0.0f && C(s, ECS_COL_MAX, ECS_POOL_POISE)[row] > 0.0f && poise_damage > 0.0f;
	if (r.killed) s->flags[row] |= ECS_FLAG_DEAD;
	if (r.staggered) s->flags[row] |= ECS_FLAG_STAGGERED;

	if (buildup) {
		for (int b = 0; b < ECS_BUILDUP_COUNT; b++)
			if (buildup[b] > 0.0f && ecs_stats_add_buildup(s, row, (ecs_buildup)b, buildup[b])) r.status_triggered |= 1u << b;
	}

	if (s->events) {
		ecs_ev_damage ev = { s->ent[row], source, r.dealt, r.overkill, poise_damage * mult,
				     (r.killed ? ECS_HIT_KILLED : 0u) | (r.staggered ? ECS_HIT_STAGGERED : 0u) | (crit ? ECS_HIT_CRIT : 0u) };
		ecs_event_emit(s->events, ECS_EV_DAMAGE, &ev);
		if (r.killed) {
			s->prev_flags[row] |= ECS_FLAG_DEAD;                 /* already announced: the tick edge pass must not repeat it */
			emit_entity(s, ECS_EV_DEATH, row, source);
		}
		if (r.staggered) {
			s->prev_flags[row] |= ECS_FLAG_STAGGERED;
			emit_entity(s, ECS_EV_STAGGER, row, source);
		}
	}
	return r;
}

float ecs_stats_damage(ecs_stats *s, int row, const float amounts[ECS_DMG_COUNT], float poise_damage)
{
	return resolve(s, row, amounts, poise_damage, ECS_NULL, false, NULL).dealt;
}

ecs_hit_result ecs_stats_apply_attack(ecs_stats *s, int target_row, const ecs_attack *a)
{
	return resolve(s, target_row, a->damage, a->poise_damage, a->source, a->crit, a->buildup);
}

ecs_hit_result ecs_stats_hit(ecs_stats *s, int attacker_row, int target_row, float motion)
{
	ecs_attack a = ecs_stats_make_attack(s, attacker_row, motion);
	return ecs_stats_apply_attack(s, target_row, &a);
}

float ecs_stats_scaling(int level)
{
	return ecs_softcap((float)level, 20.0f, 60.0f, 0.0f, 0.5f, 1.0f);
}

void ecs_stats_set_weapon(ecs_stats *s, int row, const ecs_weapon *w, const ecs_attributes *a)
{
	if (row < 0 || row >= s->count || !w || !a) return;
	const float str = ecs_stats_scaling(a->strength), dex = ecs_stats_scaling(a->dexterity), in = ecs_stats_scaling(a->intelligence),
		    fai = ecs_stats_scaling(a->faith), arc = ecs_stats_scaling(a->arcane);
	float k[ECS_DMG_COUNT];
	k[ECS_DMG_PHYSICAL]  = w->scale_str * str + w->scale_dex * dex;
	k[ECS_DMG_MAGIC]     = w->scale_int * in;
	k[ECS_DMG_FIRE]      = 0.5f * w->scale_str * str + 0.5f * w->scale_fai * fai;
	k[ECS_DMG_LIGHTNING] = 0.5f * w->scale_dex * dex + 0.5f * w->scale_arc * arc;
	k[ECS_DMG_HOLY]      = w->scale_fai * fai;
	for (int t = 0; t < ECS_DMG_COUNT; t++) C(s, ECS_COL_ATK, t)[row] = w->base[t] * (1.0f + k[t]);
	C(s, ECS_COL_ATK_POISE, 0)[row] = w->poise_damage;
	for (int b = 0; b < ECS_BUILDUP_COUNT; b++) C(s, ECS_COL_ATK_BU, b)[row] = w->buildup[b];
}

ecs_attack ecs_stats_make_attack(const ecs_stats *s, int row, float motion)
{
	ecs_attack a;
	memset(&a, 0, sizeof a);
	a.source = ECS_NULL;
	if (row < 0 || row >= s->count) return a;
	for (int t = 0; t < ECS_DMG_COUNT; t++) a.damage[t] = C(s, ECS_COL_ATK, t)[row] * motion;
	a.poise_damage = C(s, ECS_COL_ATK_POISE, 0)[row] * motion;
	for (int b = 0; b < ECS_BUILDUP_COUNT; b++) a.buildup[b] = C(s, ECS_COL_ATK_BU, b)[row];
	a.source = s->ent[row];
	return a;
}

float ecs_stats_apply_delta(ecs_stats *s, int row, ecs_pool pool, float delta, ecs_entity source)
{
	if (row < 0 || row >= s->count || (int)pool >= ECS_POOL_COUNT) return 0.0f;
	float *cur = &C(s, ECS_COL_CUR, (int)pool)[row];
	const float mx = C(s, ECS_COL_MAX, (int)pool)[row];
	const float before = *cur;
	float v = before + delta;
	v = v < 0.0f ? 0.0f : (v > mx ? mx : v);
	*cur = v;
	const float applied = v - before;
	if (applied == 0.0f) return 0.0f;
	if (applied < 0.0f) C(s, ECS_COL_TIMER, (int)pool)[row] = C(s, ECS_COL_DELAY, (int)pool)[row];
	uint32_t newbits = 0;
	if (pool == ECS_POOL_HP && v <= 0.0f && mx > 0.0f && !(s->flags[row] & ECS_FLAG_DEAD)) newbits |= ECS_FLAG_DEAD;
	if (pool == ECS_POOL_POISE && v <= 0.0f && mx > 0.0f && !(s->flags[row] & ECS_FLAG_STAGGERED)) newbits |= ECS_FLAG_STAGGERED;
	if (pool == ECS_POOL_SP && v <= 0.0f && mx > 0.0f && !(s->flags[row] & ECS_FLAG_EXHAUSTED)) newbits |= ECS_FLAG_EXHAUSTED;
	s->flags[row] |= newbits;
	if (s->events) {
		if (pool == ECS_POOL_HP && applied < 0.0f) {
			ecs_ev_damage ev = { s->ent[row], source, -applied, 0.0f, 0.0f, (newbits & ECS_FLAG_DEAD) ? ECS_HIT_KILLED : 0u };
			ecs_event_emit(s->events, ECS_EV_DAMAGE, &ev);
		}
		s->prev_flags[row] |= newbits;
		if (newbits & ECS_FLAG_DEAD) emit_entity(s, ECS_EV_DEATH, row, source);
		if (newbits & ECS_FLAG_STAGGERED) emit_entity(s, ECS_EV_STAGGER, row, source);
		if (newbits & ECS_FLAG_EXHAUSTED) emit_entity(s, ECS_EV_EXHAUSTED, row, source);
	}
	return applied;
}

void ecs_stats_set_crit_mult(ecs_stats *s, float mult) { if (mult > 0.0f) s->crit_mult = mult; }

void ecs_stats_attach_events(ecs_stats *s, ecs_events *ev)
{
	s->events = ev;
	if (ev) memcpy(s->prev_flags, s->flags, (size_t)s->count * sizeof(uint32_t));   /* don't report old state as new transitions */
}

uint64_t *ecs_stats_tags(ecs_stats *s) { return s->tags; }
uint64_t *ecs_stats_tags_base(ecs_stats *s) { return s->tags_base; }
void ecs_stats_set_exhaust_mult(ecs_stats *s, ecs_pool pool, float mult) { if ((int)pool < ECS_POOL_COUNT && mult >= 1.0f) s->cfg.pool_exhaust_delay_mult[pool] = mult; }
const ecs_stats_config *ecs_stats_get_config(const ecs_stats *s) { return &s->cfg; }

bool ecs_stats_spend(ecs_stats *s, int row, ecs_pool pool, float amount)
{
	if (row < 0 || row >= s->count || (int)pool >= ECS_POOL_COUNT) return false;
	float *cur = &C(s, ECS_COL_CUR, (int)pool)[row];
	if (*cur <= 0.0f) return false;
	*cur -= amount;
	float delay = C(s, ECS_COL_DELAY, (int)pool)[row];
	if (*cur <= 0.0f) {
		*cur = 0.0f;
		delay *= s->cfg.pool_exhaust_delay_mult[pool];       /* bottomed out: recovery starts a beat later */
		if (pool == ECS_POOL_SP && !(s->flags[row] & ECS_FLAG_EXHAUSTED)) {
			s->flags[row] |= ECS_FLAG_EXHAUSTED;
			if (s->events) {
				s->prev_flags[row] |= ECS_FLAG_EXHAUSTED;
				emit_entity(s, ECS_EV_EXHAUSTED, row, ECS_NULL);
			}
		}
	}
	C(s, ECS_COL_TIMER, (int)pool)[row] = delay;
	return true;
}

void ecs_stats_restore(ecs_stats *s, int row, ecs_pool pool, float amount)
{
	if (row < 0 || row >= s->count || (int)pool >= ECS_POOL_COUNT) return;
	float *cur = &C(s, ECS_COL_CUR, (int)pool)[row];
	float mx = C(s, ECS_COL_MAX, (int)pool)[row];
	*cur += amount;
	if (*cur > mx) *cur = mx;
	if (*cur < 0.0f) *cur = 0.0f;
	/* flags set by actions are only recomputed on the next tick - clear the ones this change just invalidated, so the next
	 * break/death/exhaustion is seen as a NEW transition (and announced) rather than swallowed by a stale bit */
	uint32_t clear = 0;
	if (*cur > 0.0f) {
		if (pool == ECS_POOL_HP) clear |= ECS_FLAG_DEAD;
		if (pool == ECS_POOL_SP) clear |= ECS_FLAG_EXHAUSTED;
		if (pool == ECS_POOL_POISE) clear |= ECS_FLAG_STAGGERED;
	}
	s->flags[row] &= ~clear;
	s->prev_flags[row] &= ~clear;
}

void ecs_stats_reset_poise(ecs_stats *s, int row)
{
	if (row < 0 || row >= s->count) return;
	C(s, ECS_COL_CUR, ECS_POOL_POISE)[row] = C(s, ECS_COL_MAX, ECS_POOL_POISE)[row];
	C(s, ECS_COL_TIMER, ECS_POOL_POISE)[row] = 0.0f;
	s->flags[row] &= ~(uint32_t)ECS_FLAG_STAGGERED;
	s->prev_flags[row] &= ~(uint32_t)ECS_FLAG_STAGGERED;
}

bool ecs_stats_add_buildup(ecs_stats *s, int row, ecs_buildup type, float amount)
{
	if (row < 0 || row >= s->count || (int)type >= ECS_BUILDUP_COUNT || amount <= 0.0f) return false;
	float *val = &C(s, ECS_COL_BU_VALUE, (int)type)[row];
	float *act = &C(s, ECS_COL_BU_ACTIVE, (int)type)[row];
	if (*act > 0.0f) return false;                      /* already afflicted: the meter stays empty until it wears off */
	*val += amount;
	if (*val < C(s, ECS_COL_BU_THRESH, (int)type)[row]) return false;
	*val = 0.0f;
	*act = s->cfg.bu_duration[type];
	s->flags[row] |= (uint32_t)ECS_FLAG_POISONED << (int)type;
	if (s->events) {
		s->prev_flags[row] |= (uint32_t)ECS_FLAG_POISONED << (int)type;      /* announced now; the edge pass won't repeat it */
		emit_status(s, ECS_EV_STATUS_APPLIED, row, (int)type);
	}
	if (type == ECS_BUILDUP_BLEED) {                    /* bleed is the burst variety: a lump of max-HP damage on trigger */
		float *hp = &C(s, ECS_COL_CUR, ECS_POOL_HP)[row];
		*hp -= C(s, ECS_COL_MAX, ECS_POOL_HP)[row] * 0.15f;
		if (*hp < 0.0f) *hp = 0.0f;
	}
	return true;
}

/* ---- load, attributes -------------------------------------------------------------------------------------------------------- */

void ecs_stats_set_load(ecs_stats *s, int row, float carried, float capacity)
{
	if (row < 0 || row >= s->count) return;
	s->col[ECS_COL_LOAD][row] = carried;
	s->col[ECS_COL_LOAD_MAX][row] = capacity;
}

float ecs_stats_load_ratio(const ecs_stats *s, int row)
{
	if (row < 0 || row >= s->count) return 0.0f;
	float cap = s->col[ECS_COL_LOAD_MAX][row];
	return cap > 0.0f ? s->col[ECS_COL_LOAD][row] / cap : 0.0f;
}

ecs_roll_class ecs_stats_roll_class(const ecs_stats *s, int row)
{
	float r = ecs_stats_load_ratio(s, row);
	if (r < s->cfg.roll_light_max) return ECS_ROLL_LIGHT;
	if (r < s->cfg.roll_medium_max) return ECS_ROLL_MEDIUM;
	return ECS_ROLL_HEAVY;                                      /* over capacity stays heavy; ECS_FLAG_OVERLOADED marks it */
}

void ecs_stats_derive(ecs_stats *s, int row, const ecs_attributes *a)
{
	if (row < 0 || row >= s->count || !a) return;
	float hp = ecs_softcap((float)a->vigor, 40.0f, 60.0f, 300.0f, 1100.0f, 1900.0f);
	float fp = ecs_softcap((float)a->mind, 55.0f, 99.0f, 40.0f, 330.0f, 500.0f);
	float sp = ecs_softcap((float)a->endurance, 50.0f, 99.0f, 80.0f, 155.0f, 170.0f);
	float ld = ecs_softcap((float)a->endurance, 25.0f, 60.0f, 45.0f, 80.0f, 120.0f);
	C(s, ECS_COL_MAX, ECS_POOL_HP)[row] = hp;
	C(s, ECS_COL_MAX, ECS_POOL_FP)[row] = fp;
	C(s, ECS_COL_MAX, ECS_POOL_SP)[row] = sp;
	s->col[ECS_COL_LOAD_MAX][row] = ld;
	C(s, ECS_COL_CUR, ECS_POOL_HP)[row] = hp;
	C(s, ECS_COL_CUR, ECS_POOL_FP)[row] = fp;
	C(s, ECS_COL_CUR, ECS_POOL_SP)[row] = sp;
	if (C(s, ECS_COL_MAX, ECS_POOL_POISE)[row] <= 0.0f) {
		C(s, ECS_COL_MAX, ECS_POOL_POISE)[row] = 100.0f;
		C(s, ECS_COL_CUR, ECS_POOL_POISE)[row] = 100.0f;
	}
}
