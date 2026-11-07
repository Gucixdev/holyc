/* test_gas.c - tags, instant/duration/periodic effects, modifiers that undo exactly, stacking policies, tag gating and cancel,
 * DoT kills with attribution, abilities (cost, cooldown-as-tag, gates, reasons) and the events they emit. */
#include "ecs_gas.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); fails++; } } while (0)
#define NEAR(a, b) (fabsf((a) - (b)) < 1e-2f)

static int n_applied, n_expired, n_activated, n_failed, n_death;
static int last_reason;
static ecs_entity death_src;
static void on_event(const ecs_event *e, void *u)
{
	(void)u;
	switch (e->type) {
	case ECS_EV_EFFECT_APPLIED: n_applied++; break;
	case ECS_EV_EFFECT_EXPIRED: n_expired++; break;
	case ECS_EV_ABILITY_ACTIVATED: n_activated++; break;
	case ECS_EV_ABILITY_FAILED: n_failed++; last_reason = ((const ecs_ev_ability *)e->data)->reason; break;
	case ECS_EV_DEATH: n_death++; death_src = ((const ecs_ev_entity *)e->data)->source; break;
	default: break;
	}
}

static float *C(ecs_stats *s, ecs_stat_col c) { return ecs_stats_col(s, c); }
#define HP(s, r) C(s, ECS_COL_CUR + ECS_POOL_HP)[r]

static void step(ecs_gas *g, ecs_stats *s, ecs_events *bus, float dt)
{
	ecs_gas_tick(g, dt);
	ecs_stats_tick(s, dt);
	ecs_events_dispatch(bus);
}

int main(void)
{
	ecs_world *w = ecs_world_new();
	ecs_events *bus = ecs_events_new();
	ecs_event_subscribe(bus, ECS_EVENT_ANY, on_event, NULL);
	ecs_stats *s = ecs_stats_new(NULL, 16);
	ecs_stats_attach_events(s, bus);
	ecs_gas *g = ecs_gas_new(s, bus);

	ecs_attributes at = { .vigor = 40, .mind = 30, .endurance = 30 };
	ecs_entity hero = ecs_create(w), foe = ecs_create(w);
	int rh = ecs_stats_add(s, hero), rf = ecs_stats_add(s, foe);
	ecs_stats_derive(s, rh, &at);
	ecs_stats_derive(s, rf, &at);

	/* ---- tags ---- */
	uint64_t T_BURN = ecs_gas_tag(g, "status.burning"), T_ROLL = ecs_gas_tag(g, "state.rolling"), T_BURN2 = ecs_gas_tag(g, "status.burning");
	CHECK(T_BURN && T_ROLL && T_BURN != T_ROLL && T_BURN == T_BURN2, "tags are single bits, stable per name");
	ecs_gas_set_tags(g, hero, T_ROLL);
	CHECK(ecs_gas_has_tags(g, hero, T_ROLL) && !ecs_gas_has_tags(g, hero, T_BURN), "permanent tags");
	ecs_gas_set_tags(g, hero, 0);

	/* ---- instant ---- */
	ecs_effect_def heal = { .name = "heal", .kind = ECS_EFFECT_INSTANT, .stat = ECS_COL_CUR + ECS_POOL_HP, .op = ECS_MOD_ADD, .magnitude = 100 };
	int E_HEAL = ecs_gas_add_effect(g, &heal);
	HP(s, rh) = 500.0f;
	CHECK(ecs_gas_apply(g, hero, E_HEAL, hero) && NEAR(HP(s, rh), 600.0f), "instant heal adds");
	CHECK(ecs_gas_active_count(g) == 0, "instant effects leave nothing running");
	HP(s, rh) = C(s, ECS_COL_MAX + ECS_POOL_HP)[rh] - 10.0f;
	ecs_gas_apply(g, hero, E_HEAL, hero);
	CHECK(NEAR(HP(s, rh), C(s, ECS_COL_MAX + ECS_POOL_HP)[rh]), "and clamp at max");

	/* ---- periodic DoT: burning, 5 dmg every 0.5 s for 3 s, stacks to 3 ---- */
	ecs_effect_def burn = { .name = "burn", .kind = ECS_EFFECT_DURATION, .duration = 3.0f, .period = 0.5f,
		.stat = ECS_COL_CUR + ECS_POOL_HP, .op = ECS_MOD_ADD, .magnitude = -5.0f, .stacking = ECS_STACK_STACK, .max_stacks = 3,
		.grants_tags = 0 };
	burn.grants_tags = T_BURN;
	int E_BURN = ecs_gas_add_effect(g, &burn);
	HP(s, rf) = 1000.0f;
	n_applied = n_expired = 0;
	CHECK(ecs_gas_apply(g, foe, E_BURN, hero), "burn lands");
	CHECK(ecs_gas_has_tags(g, foe, T_BURN), "and grants its tag");
	CHECK(ecs_gas_stacks(g, foe, E_BURN) == 1, "one stack");
	for (int i = 0; i < 6; i++) step(g, s, bus, 0.1f);                 /* 0.6 s: one 0.5 s period elapsed (not sitting on the float edge) */
	CHECK(NEAR(HP(s, rf), 995.0f), "first period (0.5 s) -> one tick of 5");
	ecs_gas_apply(g, foe, E_BURN, hero);
	ecs_gas_apply(g, foe, E_BURN, hero);
	ecs_gas_apply(g, foe, E_BURN, hero);                           /* 4th application is capped at 3 stacks */
	CHECK(ecs_gas_stacks(g, foe, E_BURN) == 3, "stacking caps at max_stacks");
	for (int i = 0; i < 5; i++) step(g, s, bus, 0.1f);
	CHECK(NEAR(HP(s, rf), 995.0f - 15.0f), "three stacks tick for 3x5 each period");
	for (int i = 0; i < 40; i++) step(g, s, bus, 0.1f);
	CHECK(ecs_gas_stacks(g, foe, E_BURN) == 0 && !ecs_gas_has_tags(g, foe, T_BURN), "expires and takes its tag with it");
	CHECK(n_expired == 1, "EFFECT_EXPIRED announced once");
	float after = HP(s, rf);
	for (int i = 0; i < 20; i++) step(g, s, bus, 0.1f);
	CHECK(NEAR(HP(s, rf), after), "no damage after expiry");

	/* ---- DoT kill is attributed ---- */
	n_death = 0;
	HP(s, rf) = 4.0f;
	ecs_gas_apply(g, foe, E_BURN, hero);
	for (int i = 0; i < 12; i++) step(g, s, bus, 0.1f);
	CHECK(HP(s, rf) == 0.0f && n_death == 1 && death_src == hero, "a DoT kill emits DEATH credited to whoever applied it");
	ecs_stats_restore(s, rf, ECS_POOL_HP, 1e6f);
	ecs_stats_tick(s, 0.01f);

	/* ---- timed modifier undoes exactly ---- */
	ecs_effect_def ward = { .name = "ward", .kind = ECS_EFFECT_DURATION, .duration = 5.0f, .stat = ECS_COL_DEFENSE + ECS_DMG_PHYSICAL,
		.op = ECS_MOD_ADD, .magnitude = 40.0f, .stacking = ECS_STACK_STACK, .max_stacks = 2 };
	int E_WARD = ecs_gas_add_effect(g, &ward);
	float base = C(s, ECS_COL_DEFENSE + ECS_DMG_PHYSICAL)[rh];
	ecs_gas_apply(g, hero, E_WARD, hero);
	CHECK(NEAR(C(s, ECS_COL_DEFENSE + ECS_DMG_PHYSICAL)[rh], base + 40.0f), "modifier applies on landing");
	ecs_gas_apply(g, hero, E_WARD, hero);
	CHECK(NEAR(C(s, ECS_COL_DEFENSE + ECS_DMG_PHYSICAL)[rh], base + 80.0f), "a stack adds another delta");
	for (int i = 0; i < 60; i++) step(g, s, bus, 0.1f);
	CHECK(C(s, ECS_COL_DEFENSE + ECS_DMG_PHYSICAL)[rh] == base, "and it is undone EXACTLY when it ends (bit-for-bit)");

	ecs_effect_def mul = { .name = "frenzy", .kind = ECS_EFFECT_INFINITE, .stat = ECS_COL_REGEN + ECS_POOL_SP, .op = ECS_MOD_MUL, .magnitude = 2.0f };
	int E_MUL = ecs_gas_add_effect(g, &mul);
	float regen = C(s, ECS_COL_REGEN + ECS_POOL_SP)[rh];
	ecs_gas_apply(g, hero, E_MUL, hero);
	CHECK(NEAR(C(s, ECS_COL_REGEN + ECS_POOL_SP)[rh], regen * 2.0f), "a MUL modifier doubles stamina regen");
	CHECK(ecs_gas_remove(g, hero, E_MUL) == 1 && NEAR(C(s, ECS_COL_REGEN + ECS_POOL_SP)[rh], regen), "removing an infinite effect restores the stat");

	/* lowering MAX drags CUR down with it */
	ecs_effect_def curse = { .name = "curse", .kind = ECS_EFFECT_INFINITE, .stat = ECS_COL_MAX + ECS_POOL_HP, .op = ECS_MOD_ADD, .magnitude = -400.0f };
	int E_CURSE = ecs_gas_add_effect(g, &curse);
	float mx = C(s, ECS_COL_MAX + ECS_POOL_HP)[rh];
	HP(s, rh) = mx;
	ecs_gas_apply(g, hero, E_CURSE, foe);
	CHECK(NEAR(C(s, ECS_COL_MAX + ECS_POOL_HP)[rh], mx - 400.0f) && NEAR(HP(s, rh), mx - 400.0f), "max HP down, current HP clamped");
	ecs_gas_remove(g, hero, E_CURSE);
	CHECK(NEAR(C(s, ECS_COL_MAX + ECS_POOL_HP)[rh], mx), "max HP restored");

	/* ---- policies: NONE ignores, REFRESH resets timer ---- */
	ecs_effect_def once = { .name = "once", .kind = ECS_EFFECT_DURATION, .duration = 2.0f, .stat = ECS_NO_STAT, .stacking = ECS_STACK_NONE };
	ecs_effect_def refr = { .name = "refr", .kind = ECS_EFFECT_DURATION, .duration = 2.0f, .stat = ECS_NO_STAT, .stacking = ECS_STACK_REFRESH };
	int E_ONCE = ecs_gas_add_effect(g, &once), E_REFR = ecs_gas_add_effect(g, &refr);
	CHECK(ecs_gas_apply(g, hero, E_ONCE, hero) && !ecs_gas_apply(g, hero, E_ONCE, hero), "NONE: re-application while active is refused");
	ecs_gas_apply(g, hero, E_REFR, hero);
	for (int i = 0; i < 15; i++) step(g, s, bus, 0.1f);            /* 1.5 s in */
	ecs_gas_apply(g, hero, E_REFR, hero);                          /* refresh to 2 s again */
	for (int i = 0; i < 15; i++) step(g, s, bus, 0.1f);            /* 3.0 s total, but only 1.5 since the refresh */
	CHECK(ecs_gas_stacks(g, hero, E_REFR) == 1, "REFRESH keeps it alive past the original expiry");
	CHECK(ecs_gas_stacks(g, hero, E_ONCE) == 0, "while the NONE one expired on schedule");
	ecs_gas_remove(g, hero, E_REFR);

	/* ---- tag gating and cancel ---- */
	uint64_t T_IMMUNE = ecs_gas_tag(g, "status.immune");
	ecs_effect_def immune = { .name = "immune", .kind = ECS_EFFECT_INFINITE, .stat = ECS_NO_STAT, .grants_tags = T_IMMUNE };
	int E_IMMUNE = ecs_gas_add_effect(g, &immune);
	burn.blocked_by_tags = T_IMMUNE;                                 /* a second, gated variant */
	burn.name = "burn_gated";
	int E_BURN_G = ecs_gas_add_effect(g, &burn);
	ecs_gas_apply(g, hero, E_IMMUNE, hero);
	CHECK(!ecs_gas_apply(g, hero, E_BURN_G, foe), "blocked_by_tags refuses the effect");
	ecs_effect_def cleanse = { .name = "cleanse", .kind = ECS_EFFECT_INSTANT, .stat = ECS_NO_STAT, .cancels_tags = T_BURN };
	int E_CLEANSE = ecs_gas_add_effect(g, &cleanse);
	ecs_gas_remove(g, hero, E_IMMUNE);
	ecs_gas_apply(g, hero, E_BURN_G, foe);
	CHECK(ecs_gas_stacks(g, hero, E_BURN_G) == 1, "burning once the immunity is gone");
	ecs_gas_apply(g, hero, E_CLEANSE, hero);
	CHECK(ecs_gas_stacks(g, hero, E_BURN_G) == 0 && !ecs_gas_has_tags(g, hero, T_BURN), "cleanse cancels effects by the tags they grant");

	/* ---- abilities ---- */
	uint64_t T_RECOVER = ecs_gas_tag(g, "state.recovering");
	ecs_effect_def iframes = { .name = "iframes", .kind = ECS_EFFECT_DURATION, .duration = 0.4f, .stat = ECS_NO_STAT, .grants_tags = T_ROLL };
	int E_IFRAMES = ecs_gas_add_effect(g, &iframes);
	ecs_ability_def roll = { .name = "roll", .cost_pool = ECS_POOL_SP, .cost = 20.0f, .cooldown = 0.5f, .blocked_tags = T_RECOVER,
		.effect_self = E_IFRAMES, .effect_target = -1 };
	int A_ROLL = ecs_gas_add_ability(g, &roll);
	ecs_ability_def fireball = { .name = "fireball", .cost_pool = ECS_POOL_FP, .cost = 30.0f, .effect_self = -1, .effect_target = E_BURN };
	int A_FIRE = ecs_gas_add_ability(g, &fireball);
	CHECK(A_ROLL >= 0 && A_FIRE >= 0, "abilities register");

	ecs_stats_restore(s, rh, ECS_POOL_SP, 1e6f);
	float sp0 = C(s, ECS_COL_CUR + ECS_POOL_SP)[rh];
	n_activated = n_failed = 0;
	CHECK(ecs_gas_activate(g, hero, A_ROLL, ECS_NULL) == ECS_ABILITY_OK, "roll activates");
	CHECK(NEAR(C(s, ECS_COL_CUR + ECS_POOL_SP)[rh], sp0 - 20.0f), "and spends stamina");
	CHECK(ecs_gas_has_tags(g, hero, T_ROLL), "i-frame effect granted the rolling tag");
	CHECK(ecs_gas_activate(g, hero, A_ROLL, ECS_NULL) == ECS_ABILITY_COOLDOWN, "second roll is on cooldown");
	CHECK(NEAR(C(s, ECS_COL_CUR + ECS_POOL_SP)[rh], sp0 - 20.0f), "a refused activation costs nothing");
	step(g, s, bus, 0.45f);
	CHECK(!ecs_gas_has_tags(g, hero, T_ROLL) && ecs_gas_can_activate(g, hero, A_ROLL) == ECS_ABILITY_COOLDOWN, "i-frames over, cooldown still running");
	step(g, s, bus, 0.1f);
	CHECK(ecs_gas_can_activate(g, hero, A_ROLL) == ECS_ABILITY_OK, "cooldown over: ready again");
	CHECK(n_activated == 1 && n_failed >= 1 && last_reason == ECS_ABILITY_COOLDOWN, "events: one activation, failures carry the reason");

	ecs_gas_set_tags(g, hero, T_RECOVER);
	CHECK(ecs_gas_activate(g, hero, A_ROLL, ECS_NULL) == ECS_ABILITY_BLOCKED, "blocked_tags gate the ability");
	ecs_gas_set_tags(g, hero, 0);

	C(s, ECS_COL_CUR + ECS_POOL_FP)[rh] = 10.0f;
	CHECK(ecs_gas_activate(g, hero, A_FIRE, foe) == ECS_ABILITY_NO_RESOURCE, "FP spell needs the full cost");
	C(s, ECS_COL_CUR + ECS_POOL_FP)[rh] = 100.0f;
	ecs_gas_activate(g, hero, A_FIRE, foe);
	CHECK(ecs_gas_stacks(g, foe, E_BURN) == 1, "effect_target landed on the target");
	CHECK(NEAR(C(s, ECS_COL_CUR + ECS_POOL_FP)[rh], 70.0f), "FP spent");

	/* souls rule: stamina abilities can fire on any SP > 0 */
	C(s, ECS_COL_CUR + ECS_POOL_SP)[rh] = 1.0f;
	ecs_effect_def nothing = { .name = "nothing", .kind = ECS_EFFECT_INSTANT, .stat = ECS_NO_STAT };
	(void)ecs_gas_add_effect(g, &nothing);
	step(g, s, bus, 0.6f);
	C(s, ECS_COL_CUR + ECS_POOL_SP)[rh] = 1.0f;
	CHECK(ecs_gas_activate(g, hero, A_ROLL, ECS_NULL) == ECS_ABILITY_OK && C(s, ECS_COL_CUR + ECS_POOL_SP)[rh] == 0.0f, "1 SP is enough to roll (floors at 0)");
	CHECK(ecs_gas_activate(g, hero, A_ROLL, ECS_NULL) != ECS_ABILITY_OK, "and now it can't");

	/* dead can't act */
	ecs_stats_apply_delta(s, rh, ECS_POOL_HP, -1e9f, foe);
	CHECK(ecs_gas_can_activate(g, hero, A_FIRE) == ECS_ABILITY_DEAD, "the dead cannot activate abilities");

	/* an entity that left the table: its effects are dropped, nothing dangles */
	ecs_gas_apply(g, foe, E_CURSE, hero);
	int before = ecs_gas_active_count(g);
	ecs_stats_remove(s, foe);
	step(g, s, bus, 0.1f);
	CHECK(ecs_gas_active_count(g) < before, "effects on a removed entity are dropped");

	ecs_gas_free(g);
	ecs_stats_free(s);
	ecs_events_free(bus);
	ecs_world_free(w);
	if (fails) { printf("%d FAILED\n", fails); return 1; }
	printf("test_gas: ok\n");
	return 0;
}
