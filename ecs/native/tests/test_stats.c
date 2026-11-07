/* test_stats.c - membership (swap-remove, stale handles), the bulk tick (regen delay, clamp, buildup, DoT, flags), damage/defense,
 * spend rules, load classes, attribute curves; plus a 20000-row differential test of the bulk tick against a scalar model. */
#include "ecs_stats.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); fails++; } } while (0)
#define NEAR(a, b) (fabsf((a) - (b)) < 1e-3f)

static float *col(ecs_stats *s, ecs_stat_col c) { return ecs_stats_col(s, c); }

int main(void)
{
	ecs_world *w = ecs_world_new();
	ecs_stats *s = ecs_stats_new(NULL, 4);

	/* ---- membership ---- */
	ecs_entity a = ecs_create(w), b = ecs_create(w), c = ecs_create(w);
	int ra = ecs_stats_add(s, a), rb = ecs_stats_add(s, b), rc = ecs_stats_add(s, c);
	CHECK(ra == 0 && rb == 1 && rc == 2 && ecs_stats_count(s) == 3, "rows assigned densely");
	CHECK(ecs_stats_add(s, a) == ra, "adding twice returns the same row");
	CHECK(ecs_stats_row(s, b) == 1 && ecs_stats_row(s, ECS_NULL) == -1, "row lookup");
	col(s, ECS_COL_CUR + ECS_POOL_HP)[rc] = 77.0f;
	ecs_stats_remove(s, a);                         /* swap-remove: c moves into row 0 */
	CHECK(ecs_stats_count(s) == 2 && ecs_stats_row(s, c) == 0 && ecs_stats_row(s, a) == -1, "swap-remove keeps rows dense");
	CHECK(col(s, ECS_COL_CUR + ECS_POOL_HP)[0] == 77.0f, "moved row carries its data");
	CHECK(ecs_stats_entities(s)[0] == c, "entity column follows");

	ecs_destroy(w, b);
	ecs_entity b2 = ecs_create(w);                  /* reuses b's slot with a new generation */
	CHECK(ecs_stats_row(s, b2) == -1, "a recycled slot is not mistaken for the old entity");
	CHECK(ecs_stats_collect(s, w) == 1 && ecs_stats_count(s) == 1, "collect drops rows of dead entities");

	/* growth keeps data and re-fetched pointers are valid */
	ecs_entity many[300];
	for (int i = 0; i < 300; i++) { many[i] = ecs_create(w); int r = ecs_stats_add(s, many[i]); col(s, ECS_COL_LOAD)[r] = (float)i; }
	CHECK(ecs_stats_count(s) == 301, "grew past the initial capacity");
	int ok = 1;
	for (int i = 0; i < 300; i++) if (col(s, ECS_COL_LOAD)[ecs_stats_row(s, many[i])] != (float)i) ok = 0;
	CHECK(ok, "data survived reallocation");
	CHECK(((uintptr_t)col(s, ECS_COL_CUR) % ECS_SIMD_ALIGN) == 0, "columns are 32-byte aligned");
	for (int i = 0; i < 300; i++) ecs_stats_remove(s, many[i]);

	/* ---- a character ---- */
	ecs_entity hero = ecs_create(w);
	int h = ecs_stats_add(s, hero);
	ecs_attributes at = { .vigor = 40, .mind = 20, .endurance = 30 };
	ecs_stats_derive(s, h, &at);
	float hpmax = col(s, ECS_COL_MAX + ECS_POOL_HP)[h];
	CHECK(NEAR(hpmax, 1100.0f), "vigor 40 sits on the soft cap of the HP curve");
	CHECK(NEAR(col(s, ECS_COL_CUR + ECS_POOL_HP)[h], hpmax), "derive fills the pools");
	ecs_attributes at2 = at; at2.vigor = 60;
	int h2 = ecs_stats_add(s, ecs_create(w));
	ecs_stats_derive(s, h2, &at2);
	CHECK(col(s, ECS_COL_MAX + ECS_POOL_HP)[h2] > hpmax, "more vigor, more HP");

	/* SP: spend, delay, regen */
	float spmax = col(s, ECS_COL_MAX + ECS_POOL_SP)[h];
	CHECK(ecs_stats_spend(s, h, ECS_POOL_SP, 40.0f), "spend succeeds");
	CHECK(NEAR(col(s, ECS_COL_CUR + ECS_POOL_SP)[h], spmax - 40.0f), "and removes the cost");
	ecs_stats_tick(s, 0.5f);
	CHECK(NEAR(col(s, ECS_COL_CUR + ECS_POOL_SP)[h], spmax - 40.0f), "no regen during the delay");
	ecs_stats_tick(s, 0.6f);                         /* delay (1.0s) expires inside this tick: remaining timer 0 */
	float after = col(s, ECS_COL_CUR + ECS_POOL_SP)[h];
	CHECK(after > spmax - 40.0f, "regen resumes once the delay has run out");
	for (int i = 0; i < 50; i++) ecs_stats_tick(s, 0.1f);
	CHECK(NEAR(col(s, ECS_COL_CUR + ECS_POOL_SP)[h], spmax), "regen clamps at max");

	/* souls rule: any SP > 0 allows the action and floors at 0; empty refuses */
	CHECK(ecs_stats_spend(s, h, ECS_POOL_SP, 9999.0f), "can act on any SP above zero");
	CHECK(col(s, ECS_COL_CUR + ECS_POOL_SP)[h] == 0.0f, "floors at zero");
	CHECK(!ecs_stats_spend(s, h, ECS_POOL_SP, 1.0f), "refused when already empty");
	ecs_stats_tick(s, 0.01f);
	CHECK((ecs_stats_flags(s)[h] & ECS_FLAG_EXHAUSTED) != 0, "EXHAUSTED while SP is empty");

	/* damage: defense bends it, resist scales it, poise breaks */
	float amt[ECS_DMG_COUNT] = { 0 };
	amt[ECS_DMG_PHYSICAL] = 100.0f;
	float none = ecs_stats_damage_preview(s, h, amt);
	CHECK(NEAR(none, 100.0f), "no defense: full damage");
	col(s, ECS_COL_DEFENSE + ECS_DMG_PHYSICAL)[h] = 100.0f;
	float def = ecs_stats_damage_preview(s, h, amt);
	CHECK(NEAR(def, 50.0f), "attack 100 vs defense 100 -> 100^2/200 = 50");
	col(s, ECS_COL_RESIST + ECS_DMG_PHYSICAL)[h] = 0.5f;
	CHECK(NEAR(ecs_stats_damage_preview(s, h, amt), 25.0f), "resist 50% halves the rest");
	CHECK(ecs_stats_damage_preview(s, h, (float[ECS_DMG_COUNT]){ 0 }) == 0.0f, "no damage, no effect");

	float hp0 = col(s, ECS_COL_CUR + ECS_POOL_HP)[h];
	float dealt = ecs_stats_damage(s, h, amt, 120.0f);
	CHECK(NEAR(dealt, 25.0f) && NEAR(col(s, ECS_COL_CUR + ECS_POOL_HP)[h], hp0 - 25.0f), "damage applied to HP");
	CHECK(col(s, ECS_COL_CUR + ECS_POOL_POISE)[h] == 0.0f, "120 poise damage breaks 100 poise");
	ecs_stats_tick(s, 0.01f);
	CHECK((ecs_stats_flags(s)[h] & ECS_FLAG_STAGGERED) != 0, "STAGGERED once poise is gone");
	ecs_stats_reset_poise(s, h);
	ecs_stats_tick(s, 0.01f);
	CHECK((ecs_stats_flags(s)[h] & ECS_FLAG_STAGGERED) == 0, "reset_poise clears it");

	/* overkill clamps; dead flag */
	amt[ECS_DMG_PHYSICAL] = 1e6f;
	col(s, ECS_COL_DEFENSE + ECS_DMG_PHYSICAL)[h] = 0.0f;
	col(s, ECS_COL_RESIST + ECS_DMG_PHYSICAL)[h] = 0.0f;
	float hp_now = col(s, ECS_COL_CUR + ECS_POOL_HP)[h];
	CHECK(NEAR(ecs_stats_damage(s, h, amt, 0.0f), hp_now), "overkill only takes what was there");
	ecs_stats_tick(s, 0.01f);
	CHECK((ecs_stats_flags(s)[h] & ECS_FLAG_DEAD) != 0, "DEAD at zero HP");
	ecs_stats_restore(s, h, ECS_POOL_HP, 1e9f);
	CHECK(NEAR(col(s, ECS_COL_CUR + ECS_POOL_HP)[h], hpmax), "restore clamps at max");

	/* buildup: meter fills, triggers, DoT, expires */
	int p = ecs_stats_add(s, ecs_create(w));
	ecs_stats_derive(s, p, &at);
	float php = col(s, ECS_COL_CUR + ECS_POOL_HP)[p];
	CHECK(!ecs_stats_add_buildup(s, p, ECS_BUILDUP_POISON, 60.0f), "below threshold: no trigger");
	ecs_stats_tick(s, 1.0f);                         /* 60 - 4 decay */
	CHECK(NEAR(col(s, ECS_COL_BU_VALUE + ECS_BUILDUP_POISON)[p], 56.0f), "meter decays");
	CHECK(ecs_stats_add_buildup(s, p, ECS_BUILDUP_POISON, 60.0f), "crossing the threshold triggers");
	CHECK(col(s, ECS_COL_BU_VALUE + ECS_BUILDUP_POISON)[p] == 0.0f, "meter resets on trigger");
	ecs_stats_tick(s, 1.0f);
	CHECK((ecs_stats_flags(s)[p] & ECS_FLAG_POISONED) != 0, "POISONED flag while active");
	CHECK(NEAR(col(s, ECS_COL_CUR + ECS_POOL_HP)[p], php - hpmax * 0.01f), "poison drains 1% max HP per second");
	CHECK(!ecs_stats_add_buildup(s, p, ECS_BUILDUP_POISON, 500.0f), "no re-trigger while afflicted");
	ecs_stats_tick(s, 25.0f);                        /* duration 20s */
	CHECK((ecs_stats_flags(s)[p] & ECS_FLAG_POISONED) == 0, "status wears off");

	/* load / roll class */
	ecs_stats_set_load(s, p, 10.0f, 100.0f);
	CHECK(ecs_stats_roll_class(s, p) == ECS_ROLL_LIGHT, "10% -> light");
	ecs_stats_set_load(s, p, 50.0f, 100.0f);
	CHECK(ecs_stats_roll_class(s, p) == ECS_ROLL_MEDIUM, "50% -> medium");
	ecs_stats_set_load(s, p, 90.0f, 100.0f);
	CHECK(ecs_stats_roll_class(s, p) == ECS_ROLL_HEAVY, "90% -> heavy");
	ecs_stats_set_load(s, p, 120.0f, 100.0f);
	CHECK(ecs_stats_roll_class(s, p) == ECS_ROLL_HEAVY, "120% stays heavy (3 classes) and raises OVERLOADED");
	ecs_stats_tick(s, 0.01f);
	CHECK((ecs_stats_flags(s)[p] & ECS_FLAG_OVERLOADED) != 0, "OVERLOADED flag");

	ecs_stats_free(s);
	ecs_world_free(w);

	/* ---- 20000-row differential: vector tick vs a scalar model, many ticks, random timers ---- */
	{
		enum { N = 20000 };
		ecs_world *w2 = ecs_world_new();
		ecs_stats *s2 = ecs_stats_new(NULL, 16);
		static float mh[N], mr[N], mt[N], mm[N];       /* model: hp-like pool 2 (SP) cur/regen/timer/max */
		unsigned r = 99;
		for (int i = 0; i < N; i++) {
			int row = ecs_stats_add(s2, ecs_create(w2));
			r = r * 1664525u + 1013904223u; float mx = 50.0f + (float)((r >> 8) % 100);
			r = r * 1664525u + 1013904223u; float cur = (float)((r >> 8) % 100);
			r = r * 1664525u + 1013904223u; float tm = (float)((r >> 8) % 300) / 100.0f;
			if (cur > mx) cur = mx;
			col(s2, ECS_COL_MAX + ECS_POOL_SP)[row] = mm[i] = mx;
			col(s2, ECS_COL_CUR + ECS_POOL_SP)[row] = mh[i] = cur;
			col(s2, ECS_COL_TIMER + ECS_POOL_SP)[row] = mt[i] = tm;
			mr[i] = col(s2, ECS_COL_REGEN + ECS_POOL_SP)[row];
		}
		for (int step = 0; step < 40; step++) {
			ecs_stats_tick(s2, 0.1f);
			for (int i = 0; i < N; i++) {
				float t = mt[i] - 0.1f; t = t > 0.0f ? t : 0.0f; mt[i] = t;
				float add = t <= 0.0f ? mr[i] * 0.1f : 0.0f;
				float v = mh[i] + add; v = v < 0.0f ? 0.0f : v; mh[i] = v > mm[i] ? mm[i] : v;
			}
		}
		int bad = 0;
		for (int i = 0; i < N; i++) {
			float got = col(s2, ECS_COL_CUR + ECS_POOL_SP)[i];
			if (memcmp(&got, &mh[i], sizeof got) != 0) bad++;
		}
		CHECK(bad == 0, "vector tick is bit-identical to the scalar model over 20000 rows x 40 ticks");
		ecs_stats_free(s2);
		ecs_world_free(w2);
	}

	if (fails) { printf("%d FAILED\n", fails); return 1; }
	printf("test_stats: ok\n");
	return 0;
}
