/* test_ecs.c - entities (create/destroy/alive/stale handles), component registration (incl. tags), add/remove/get/has across archetype
 * moves (the part most likely to corrupt data if the copy-on-move logic is wrong), and views (per-entity and bulk/table iteration). */
#include "ecs.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); fails++; } } while (0)

typedef struct { float x, y; } Pos;
typedef struct { float x, y; } Vel;
typedef struct { int hp, maxhp; } Health;

int main(void)
{
	ecs_world *w = ecs_world_new();

	/* ---- entities ---- */
	ecs_entity a = ecs_create(w), b = ecs_create(w);
	CHECK(ecs_alive(w, a) && ecs_alive(w, b) && a != b, "two fresh entities are alive and distinct");
	CHECK(ecs_entity_count(w) == 2, "the world counts them");
	CHECK(!ecs_alive(w, ECS_NULL), "ECS_NULL is never alive");
	ecs_destroy(w, a);
	CHECK(!ecs_alive(w, a) && ecs_alive(w, b), "destroying one leaves the other alive");
	CHECK(ecs_entity_count(w) == 1, "and the count drops");
	ecs_destroy(w, a);                                                          /* destroying it again must not crash or double-count */
	CHECK(ecs_entity_count(w) == 1, "destroying an already-dead handle is a no-op");
	ecs_entity c = ecs_create(w);                                               /* recycles a's slot */
	CHECK(ecs_alive(w, c) && !ecs_alive(w, a), "a recycled slot's new handle is alive, the old stale one still reads dead");

	/* ---- component registration, incl. tags ---- */
	int POS = ecs_register(w, "Pos", sizeof(Pos));
	int VEL = ecs_register(w, "Vel", sizeof(Vel));
	int HP = ecs_register(w, "Health", sizeof(Health));
	int ENEMY = ecs_register_tag(w, "Enemy");
	CHECK(POS >= 0 && VEL >= 0 && HP >= 0 && ENEMY >= 0, "every registration succeeds");
	CHECK(ecs_register(w, "Pos", sizeof(Pos)) == -1, "registering the same name twice fails");
	CHECK(ecs_component_id(w, "Vel") == VEL, "a type looks itself up by name");
	CHECK(ecs_component_id(w, "nope") == -1, "an unknown name is -1");
	CHECK(!strcmp(ecs_component_name(w, POS), "Pos"), "a type's name round-trips");
	CHECK(ecs_component_size(w, POS) == sizeof(Pos) && ecs_component_size(w, ENEMY) == 0, "a tag's size is 0");
	CHECK(ecs_type_count(w) == 4, "four types are registered");

	/* ---- add/get/has across archetype moves - the part that would corrupt data if the copy-on-move were wrong ---- */
	ecs_entity e1 = ecs_create(w);
	CHECK(!ecs_has(w, e1, POS), "a fresh entity has no components");
	Pos *p = ecs_add(w, e1, POS);
	CHECK(p != NULL && p->x == 0 && p->y == 0, "add returns a zeroed slot");
	p->x = 1.0f; p->y = 2.0f;
	Vel *v = ecs_add(w, e1, VEL);                                               /* moves e1 to a new {Pos,Vel} archetype */
	v->x = 5.0f; v->y = 6.0f;
	Pos *p2 = ecs_get(w, e1, POS);
	CHECK(p2 && p2->x == 1.0f && p2->y == 2.0f, "Pos survived the move to the {Pos,Vel} archetype intact");
	Health *hp = ecs_add(w, e1, HP);                                            /* moves it again, to {Pos,Vel,Health} */
	hp->hp = 10; hp->maxhp = 10;
	CHECK(((Pos *)ecs_get(w, e1, POS))->x == 1.0f && ((Vel *)ecs_get(w, e1, VEL))->x == 5.0f, "both earlier components survived a second move");
	CHECK(ecs_has(w, e1, ENEMY) == false, "no tag yet");
	void *tagp = ecs_add(w, e1, ENEMY);
	CHECK(tagp != NULL && ecs_has(w, e1, ENEMY), "adding a tag reports present, with a non-NULL (if non-dereferenceable) marker");
	CHECK(ecs_get(w, e1, ENEMY) != NULL, "a tag's ecs_get is also non-NULL");

	/* a second entity sharing the {Pos,Vel} shape must not disturb the first's data when IT moves to a different shape */
	ecs_entity e2 = ecs_create(w);
	Pos *p2a = ecs_add(w, e2, POS); p2a->x = 100.0f; p2a->y = 200.0f;
	Vel *v2 = ecs_add(w, e2, VEL); v2->x = 9.0f; v2->y = 9.0f;
	ecs_remove(w, e2, VEL);                                                     /* e2 -> {Pos} only; must not corrupt e1's {Pos,Vel,Health,Enemy} row */
	CHECK(((Pos *)ecs_get(w, e2, POS))->x == 100.0f, "e2's own Pos survived losing Vel");
	CHECK(!ecs_has(w, e2, VEL), "...and Vel is really gone");
	CHECK(((Pos *)ecs_get(w, e1, POS))->x == 1.0f && ((Vel *)ecs_get(w, e1, VEL))->x == 5.0f && ((Health *)ecs_get(w, e1, HP))->hp == 10,
	      "e1's unrelated data is untouched by e2's move");

	/* remove down to nothing, and re-add: an entity can round-trip back to the empty archetype and out again */
	ecs_remove(w, e2, POS);
	CHECK(!ecs_has(w, e2, POS) && !ecs_has(w, e2, VEL) && !ecs_has(w, e2, HP), "e2 is back to no components at all");
	ecs_remove(w, e2, POS);                                                     /* removing what it doesn't have is a no-op, not a crash */
	Pos *p2b = ecs_add(w, e2, POS);
	CHECK(p2b != NULL && p2b->x == 0, "adding it back gives a fresh zeroed slot, not stale data from before");

	/* destroying an entity mid-archetype must not corrupt whoever gets swapped into its old row */
	ecs_entity e3 = ecs_create(w);
	Pos *p3 = ecs_add(w, e3, POS); p3->x = 42.0f;                               /* e3 shares e2's {Pos} archetype, likely lands in the row after it */
	ecs_destroy(w, e2);
	CHECK(ecs_alive(w, e3) && ((Pos *)ecs_get(w, e3, POS))->x == 42.0f, "destroying e2 left e3 (possibly swapped into e2's old row) with its own data intact");

	/* out-of-range component ids and dead handles are handled gracefully everywhere, not a crash */
	CHECK(ecs_get(w, e1, 999) == NULL && !ecs_has(w, e1, 999) && !ecs_has(w, e1, -1), "an invalid component id is handled, not a crash");
	CHECK(ecs_add(w, ECS_NULL, POS) == NULL && ecs_get(w, ECS_NULL, POS) == NULL, "ECS_NULL is handled everywhere, not a crash");
	ecs_remove(w, ECS_NULL, POS);                                               /* must not crash */

	/* ---- views: per-entity iteration ---- */
	int q_pos[1] = {POS};
	ecs_view *vw = ecs_view_new(w, q_pos, 1);
	int seen = 0;
	ecs_entity out;
	while (ecs_view_next(vw, &out)) seen++;
	CHECK(seen == ecs_type_count_entities(w, POS), "a one-component view visits exactly as many entities as have that component");
	ecs_view_free(vw);

	int q_posvel[2] = {POS, VEL};
	ecs_view *vw2 = ecs_view_new(w, q_posvel, 2);
	seen = 0;
	bool saw_e1 = false;
	while (ecs_view_next(vw2, &out)) { seen++; if (out == e1) saw_e1 = true; }
	CHECK(seen == 1 && saw_e1, "a {Pos,Vel} view finds exactly e1 (the only entity with both right now)");
	ecs_view_free(vw2);

	ecs_view *vw3 = ecs_view_new(w, q_pos, 1);
	CHECK(!ecs_view_next(NULL, &out), "a NULL view is handled, not a crash");
	int n0 = 0; while (ecs_view_next(vw3, &out)) n0++;
	CHECK(!ecs_view_next(vw3, &out), "a spent view just keeps returning false, not looping back around");
	ecs_view_free(vw3);
	(void)n0;

	/* ---- views: bulk/table iteration (the SIMD-friendly path) ---- */
	ecs_view *vwt = ecs_view_new(w, q_pos, 1);
	int total = 0, tables = 0;
	while (ecs_view_next_table(vwt)) {
		tables++;
		int n = ecs_view_count(vwt);
		total += n;
		const ecs_entity *ents = ecs_view_entities(vwt);
		Pos *col = ecs_view_column(vwt, POS);
		CHECK((n == 0) || (ents != NULL && col != NULL), "a non-empty table's entity list and column are both real arrays");
		for (int i = 0; i < n; i++) CHECK(ecs_get(w, ents[i], POS) == &col[i], "the column pointer really is that entity's Pos storage, not a copy");
	}
	CHECK(total == ecs_type_count_entities(w, POS), "bulk iteration visits the same total as per-entity iteration");
	CHECK(tables == ecs_view_archetype_count(vwt), "the table count matches how many archetypes matched");
	ecs_view_free(vwt);

	int q_enemy[1] = {ENEMY};
	ecs_view *vwe = ecs_view_new(w, q_enemy, 1);
	CHECK(ecs_view_next_table(vwe), "the Enemy tag has at least one matching table (e1's)");
	CHECK(ecs_view_count(vwe) == 1 && ecs_view_entities(vwe)[0] == e1, "and it holds exactly e1");
	CHECK(ecs_view_column(vwe, ENEMY) == NULL, "a tag has no column to hand back, even for a table that has it");
	ecs_view_free(vwe);

	printf(fails ? "ecs: %d failed\n" : "ecs: ok\n", fails);
	ecs_world_free(w);
	return fails != 0;
}
