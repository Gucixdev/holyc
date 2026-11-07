/* test_combat.c - the souls rules added on top of the stat table: the longer stamina delay after bottoming out, weapon scaling ->
 * attack rating, attack -> hit (defense, poise, crit, buildup), and the event stream (hit/death/stagger/exhaust/status, exactly once). */
#include "ecs_stats.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); fails++; } } while (0)
#define NEAR(a, b) (fabsf((a) - (b)) < 1e-2f)

static struct { int damage, death, stagger, exhausted, applied, ended; ecs_ev_damage last; ecs_entity death_src, death_ent; } ev;
static void on_event(const ecs_event *e, void *u)
{
	(void)u;
	switch (e->type) {
	case ECS_EV_DAMAGE: ev.damage++; ev.last = *(const ecs_ev_damage *)e->data; break;
	case ECS_EV_DEATH: ev.death++; ev.death_src = ((const ecs_ev_entity *)e->data)->source; ev.death_ent = ((const ecs_ev_entity *)e->data)->entity; break;
	case ECS_EV_STAGGER: ev.stagger++; break;
	case ECS_EV_EXHAUSTED: ev.exhausted++; break;
	case ECS_EV_STATUS_APPLIED: ev.applied++; break;
	case ECS_EV_STATUS_ENDED: ev.ended++; break;
	default: break;
	}
}

static float *C(ecs_stats *s, ecs_stat_col c) { return ecs_stats_col(s, c); }

int main(void)
{
	ecs_world *w = ecs_world_new();

	/* ---- stamina: bottoming out delays recovery longer than a normal swing ---- */
	{
		ecs_stats *s = ecs_stats_new(NULL, 8);
		int r = ecs_stats_add(s, ecs_create(w));
		ecs_attributes at = { .vigor = 20, .endurance = 30 };
		ecs_stats_derive(s, r, &at);
		float sp = C(s, ECS_COL_MAX + ECS_POOL_SP)[r], delay = C(s, ECS_COL_DELAY + ECS_POOL_SP)[r];

		ecs_stats_spend(s, r, ECS_POOL_SP, 10.0f);
		CHECK(NEAR(C(s, ECS_COL_TIMER + ECS_POOL_SP)[r], delay), "a normal swing waits the base delay");
		ecs_stats_spend(s, r, ECS_POOL_SP, sp);                         /* drain it */
		float t = C(s, ECS_COL_TIMER + ECS_POOL_SP)[r];
		CHECK(C(s, ECS_COL_CUR + ECS_POOL_SP)[r] == 0.0f, "emptied");
		CHECK(NEAR(t, delay * 1.5f), "bottoming out waits 1.5x the base delay (slightly longer, as in the games)");
		ecs_stats_set_exhaust_mult(s, ECS_POOL_SP, 2.0f);
		ecs_stats_restore(s, r, ECS_POOL_SP, 50.0f);
		ecs_stats_spend(s, r, ECS_POOL_SP, 9999.0f);
		CHECK(NEAR(C(s, ECS_COL_TIMER + ECS_POOL_SP)[r], delay * 2.0f), "the penalty is tunable at runtime");
		ecs_stats_free(s);
	}

	/* ---- 3 roll classes; overweight stays heavy and sets the flag ---- */
	{
		ecs_stats *s = ecs_stats_new(NULL, 8);
		int r = ecs_stats_add(s, ecs_create(w));
		ecs_stats_set_load(s, r, 29.0f, 100.0f);
		CHECK(ecs_stats_roll_class(s, r) == ECS_ROLL_LIGHT, "29% light");
		ecs_stats_set_load(s, r, 30.0f, 100.0f);
		CHECK(ecs_stats_roll_class(s, r) == ECS_ROLL_MEDIUM, "30% medium (boundary)");
		ecs_stats_set_load(s, r, 70.0f, 100.0f);
		CHECK(ecs_stats_roll_class(s, r) == ECS_ROLL_HEAVY, "70% heavy (boundary)");
		ecs_stats_set_load(s, r, 100.0f, 100.0f);
		CHECK(ecs_stats_roll_class(s, r) == ECS_ROLL_HEAVY, "exactly full is heavy");
		ecs_stats_set_load(s, r, 150.0f, 100.0f);
		CHECK(ecs_stats_roll_class(s, r) == ECS_ROLL_HEAVY, "over capacity is still heavy: there are only three classes");
		ecs_stats_tick(s, 0.01f);
		CHECK((ecs_stats_flags(s)[r] & ECS_FLAG_OVERLOADED) != 0, "...but OVERLOADED is raised");
		CHECK(ECS_ROLL_COUNT == 3, "the enum has three classes");
		ecs_stats_free(s);
	}

	/* ---- weapon scaling -> attack rating ---- */
	{
		ecs_stats *s = ecs_stats_new(NULL, 8);
		int r = ecs_stats_add(s, ecs_create(w));
		ecs_weapon sword = { .base = { 100, 0, 0, 0, 0 }, .scale_str = 0.4f, .scale_dex = 0.4f, .poise_damage = 30.0f };
		ecs_attributes weak = { .strength = 0, .dexterity = 0 }, mid = { .strength = 20, .dexterity = 20 }, strong = { .strength = 60, .dexterity = 60 };
		ecs_stats_set_weapon(s, r, &sword, &weak);
		CHECK(NEAR(C(s, ECS_COL_ATK + ECS_DMG_PHYSICAL)[r], 100.0f), "no stats, no scaling: base damage");
		ecs_stats_set_weapon(s, r, &sword, &mid);
		CHECK(NEAR(C(s, ECS_COL_ATK + ECS_DMG_PHYSICAL)[r], 100.0f * (1.0f + 0.4f * 0.5f + 0.4f * 0.5f)), "level 20 = half scaling");
		ecs_stats_set_weapon(s, r, &sword, &strong);
		CHECK(NEAR(C(s, ECS_COL_ATK + ECS_DMG_PHYSICAL)[r], 100.0f * (1.0f + 0.4f + 0.4f)), "level 60 = full scaling");
		CHECK(NEAR(C(s, ECS_COL_ATK_POISE)[r], 30.0f), "weapon poise damage stored");
		ecs_attack a = ecs_stats_make_attack(s, r, 1.6f);
		CHECK(NEAR(a.damage[ECS_DMG_PHYSICAL], 180.0f * 1.6f) && NEAR(a.poise_damage, 48.0f), "motion value scales damage and poise");
		ecs_stats_free(s);
	}

	/* ---- a fight, with events ---- */
	{
		ecs_events *bus = ecs_events_new();
		ecs_event_subscribe(bus, ECS_EVENT_ANY, on_event, NULL);
		ecs_stats *s = ecs_stats_new(NULL, 8);
		ecs_stats_attach_events(s, bus);
		ecs_entity ea = ecs_create(w), eb = ecs_create(w);
		int a = ecs_stats_add(s, ea), b = ecs_stats_add(s, eb);
		ecs_attributes at = { .vigor = 20, .endurance = 20, .strength = 40 };
		ecs_stats_derive(s, a, &at);
		ecs_stats_derive(s, b, &at);
		ecs_weapon axe = { .base = { 200, 0, 0, 0, 0 }, .scale_str = 0.5f, .poise_damage = 60.0f };
		ecs_stats_set_weapon(s, a, &axe, &at);
		C(s, ECS_COL_DEFENSE + ECS_DMG_PHYSICAL)[b] = 100.0f;
		memset(&ev, 0, sizeof ev);

		float hp_b = C(s, ECS_COL_CUR + ECS_POOL_HP)[b];
		ecs_hit_result h = ecs_stats_hit(s, a, b, 1.0f);
		CHECK(h.dealt > 0.0f && NEAR(C(s, ECS_COL_CUR + ECS_POOL_HP)[b], hp_b - h.dealt), "a hit removes HP");
		CHECK(!h.killed && !h.staggered, "first hit neither kills nor staggers");
		ecs_events_dispatch(bus);
		CHECK(ev.damage == 1 && ev.last.source == ea && ev.last.target == eb && NEAR(ev.last.dealt, h.dealt), "DAMAGE event carries source, target, amount");

		/* defense matters */
		float vs0 = ecs_stats_damage_preview(s, b, (float[ECS_DMG_COUNT]){ 300, 0, 0, 0, 0 });
		C(s, ECS_COL_DEFENSE + ECS_DMG_PHYSICAL)[b] = 0.0f;
		float vs1 = ecs_stats_damage_preview(s, b, (float[ECS_DMG_COUNT]){ 300, 0, 0, 0, 0 });
		CHECK(vs1 > vs0, "defense reduces damage");
		C(s, ECS_COL_DEFENSE + ECS_DMG_PHYSICAL)[b] = 100.0f;

		/* crit */
		ecs_attack atk = ecs_stats_make_attack(s, a, 1.0f);
		float hp0 = C(s, ECS_COL_CUR + ECS_POOL_HP)[b];
		ecs_stats_apply_attack(s, b, &atk);
		float normal = hp0 - C(s, ECS_COL_CUR + ECS_POOL_HP)[b];
		ecs_stats_restore(s, b, ECS_POOL_HP, 1e6f);
		ecs_stats_reset_poise(s, b);
		atk.crit = true;
		hp0 = C(s, ECS_COL_CUR + ECS_POOL_HP)[b];
		ecs_stats_apply_attack(s, b, &atk);
		float crit = hp0 - C(s, ECS_COL_CUR + ECS_POOL_HP)[b];
		CHECK(crit > normal * 1.5f, "a crit hits harder");
		ecs_events_dispatch(bus);

		/* poise break -> exactly one STAGGER */
		ecs_stats_restore(s, b, ECS_POOL_HP, 1e6f);
		ecs_stats_reset_poise(s, b);
		memset(&ev, 0, sizeof ev);
		C(s, ECS_COL_CUR + ECS_POOL_POISE)[b] = 50.0f;
		h = ecs_stats_hit(s, a, b, 1.0f);                         /* 60 poise damage vs 50 poise */
		CHECK(h.staggered, "poise broke");
		ecs_stats_tick(s, 0.01f);                                 /* the edge pass must not repeat what the action announced */
		ecs_events_dispatch(bus);
		CHECK(ev.stagger == 1, "STAGGER announced exactly once");

		/* lethal hit -> exactly one DEATH, with the killer */
		memset(&ev, 0, sizeof ev);
		C(s, ECS_COL_CUR + ECS_POOL_HP)[b] = 1.0f;
		h = ecs_stats_hit(s, a, b, 1.0f);
		CHECK(h.killed && h.overkill > 0.0f, "lethal hit reports killed and overkill");
		ecs_stats_tick(s, 0.01f);
		ecs_stats_tick(s, 0.01f);
		ecs_events_dispatch(bus);
		CHECK(ev.death == 1 && ev.death_ent == eb && ev.death_src == ea, "DEATH once, attributed to the attacker");
		CHECK((ecs_stats_flags(s)[b] & ECS_FLAG_DEAD) != 0, "DEAD flag set");

		/* death by damage-over-time (no attacker in the action path): the tick edge pass announces it */
		int c = ecs_stats_add(s, ecs_create(w));
		ecs_stats_derive(s, c, &at);
		ecs_entity ec = ecs_stats_entities(s)[c];
		ecs_events_dispatch(bus);
		memset(&ev, 0, sizeof ev);
		C(s, ECS_COL_CUR + ECS_POOL_HP)[c] = 0.0f;                 /* e.g. a script or a DoT drove it to zero */
		ecs_stats_tick(s, 0.01f);
		ecs_events_dispatch(bus);
		CHECK(ev.death == 1 && ev.death_ent == ec, "a death the tick discovered is announced by the tick");
		ecs_stats_tick(s, 0.01f);
		ecs_events_dispatch(bus);
		CHECK(ev.death == 1, "and not again on later ticks");

		/* exhaustion event */
		int d = ecs_stats_add(s, ecs_create(w));
		ecs_stats_derive(s, d, &at);
		ecs_events_dispatch(bus);
		memset(&ev, 0, sizeof ev);
		ecs_stats_spend(s, d, ECS_POOL_SP, 1e6f);
		ecs_stats_tick(s, 0.01f);
		ecs_events_dispatch(bus);
		CHECK(ev.exhausted == 1, "EXHAUSTED once when SP bottoms out");

		/* status buildup events: applied then ended */
		int e = ecs_stats_add(s, ecs_create(w));
		ecs_stats_derive(s, e, &at);
		ecs_events_dispatch(bus);
		memset(&ev, 0, sizeof ev);
		ecs_weapon venom = { .base = { 10, 0, 0, 0, 0 }, .buildup = { 120, 0, 0, 0 } };
		ecs_stats_set_weapon(s, a, &venom, &at);
		h = ecs_stats_hit(s, a, e, 1.0f);
		CHECK((h.status_triggered & (1u << ECS_BUILDUP_POISON)) != 0, "hit reports the status it triggered");
		ecs_stats_tick(s, 0.01f);
		ecs_events_dispatch(bus);
		CHECK(ev.applied == 1, "STATUS_APPLIED once");
		ecs_stats_tick(s, 30.0f);
		ecs_events_dispatch(bus);
		CHECK(ev.ended == 1, "STATUS_ENDED when it wears off");

		/* apply_delta attributes a kill to a source (the hook DoT effects use) */
		int f = ecs_stats_add(s, ecs_create(w));
		ecs_stats_derive(s, f, &at);
		ecs_entity ef = ecs_stats_entities(s)[f];
		ecs_events_dispatch(bus);
		memset(&ev, 0, sizeof ev);
		float took = ecs_stats_apply_delta(s, f, ECS_POOL_HP, -1e9f, ea);
		CHECK(took < 0.0f && C(s, ECS_COL_CUR + ECS_POOL_HP)[f] == 0.0f, "apply_delta clamps at zero");
		ecs_events_dispatch(bus);
		CHECK(ev.death == 1 && ev.death_ent == ef && ev.death_src == ea, "and credits the source with the kill");

		ecs_stats_free(s);
		ecs_events_free(bus);
	}

	ecs_world_free(w);
	if (fails) { printf("%d FAILED\n", fails); return 1; }
	printf("test_combat: ok\n");
	return 0;
}
