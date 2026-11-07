/* test_stress.c - thousands of random creates/destroys/adds/removes across several component types and sizes, checking after every
 * single operation that every entity's component data still matches what it should be. This is the test that would actually catch a
 * broken swap-removal or a wrong column copied during an archetype move - the directed tests in test_ecs.c cover the shapes I thought
 * of; this covers the ones I didn't. */
#include "ecs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); fails++; } } while (0)

#define NENT 200
#define NCOMP 5
#define NOPS 20000

typedef struct { int32_t tag; double a, b, c; } Blob;    /* the biggest component: enough room to catch a short memcpy */

static uint32_t rng_state = 0xC0FFEEu;
static uint32_t rnd(void) { rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5; return rng_state; }
static int rnd_n(int n) { return (int)(rnd() % (uint32_t)n); }

/* what the test believes is true, kept independently of the ECS so it has something to check the ECS against */
typedef struct { ecs_entity e; bool alive; bool has[NCOMP]; } track;

/* every registered component's first 4 bytes always get set to `expect_tag(entity_slot, comp)` when added - a cheap, distinctive
 * fingerprint that a wrong-slot copy during a move would almost certainly break. */
static int32_t expect_tag(int slot, int comp) { return slot * 1000 + comp; }

int main(void)
{
	ecs_world *w = ecs_world_new();
	size_t sizes[NCOMP] = {sizeof(int32_t), sizeof(int64_t), sizeof(Blob), 0 /* a tag */, sizeof(int32_t) * 3};
	int comp[NCOMP];
	char name[32];
	for (int i = 0; i < NCOMP; i++) { snprintf(name, sizeof name, "C%d", i); comp[i] = ecs_register(w, name, sizes[i]); }

	track tr[NENT];
	memset(tr, 0, sizeof tr);
	for (int i = 0; i < NENT; i++) { tr[i].e = ecs_create(w); tr[i].alive = true; }

	for (int op = 0; op < NOPS; op++) {
		int i = rnd_n(NENT);
		int action = rnd_n(4);
		if (!tr[i].alive) {
			if (action != 3) continue;                  /* only "recreate" makes sense on a dead slot */
		}
		switch (action) {
		case 0: {                                        /* add a random not-yet-present component */
			int c = rnd_n(NCOMP);
			if (tr[i].has[c]) break;
			void *p = ecs_add(w, tr[i].e, comp[c]);
			CHECK(p != NULL, "add on a live entity always succeeds");
			if (sizes[c] > 0 && p) memcpy(p, &(int32_t){expect_tag(i, c)}, sizeof(int32_t));
			tr[i].has[c] = true;
			break;
		}
		case 1: {                                         /* remove a random present component */
			int c = rnd_n(NCOMP);
			if (!tr[i].has[c]) break;
			ecs_remove(w, tr[i].e, comp[c]);
			tr[i].has[c] = false;
			break;
		}
		case 2: {                                         /* re-write a random present component's fingerprint (still the "same" value, just re-touched) */
			int c = rnd_n(NCOMP);
			if (!tr[i].has[c] || sizes[c] == 0) break;
			void *p = ecs_get(w, tr[i].e, comp[c]);
			CHECK(p != NULL, "get on a component the tracker believes is present must not be NULL");
			if (p) memcpy(p, &(int32_t){expect_tag(i, c)}, sizeof(int32_t));
			break;
		}
		case 3: {                                          /* destroy (if alive), then always recreate so every slot stays populated */
			if (tr[i].alive) ecs_destroy(w, tr[i].e);
			tr[i].e = ecs_create(w);
			tr[i].alive = true;
			memset(tr[i].has, 0, sizeof tr[i].has);
			break;
		}
		}

		/* the fingerprint check, every single op: cheap enough (NCOMP is tiny) to run every iteration rather than just at the end,
		 * so a corruption is caught at the operation that caused it, not thousands of ops later. */
		for (int k = 0; k < NENT; k++) {
			if (!tr[k].alive) continue;
			for (int c = 0; c < NCOMP; c++) {
				bool has = ecs_has(w, tr[k].e, comp[c]);
				CHECK(has == tr[k].has[c], "ecs_has agrees with what the tracker believes was added/removed");
				if (has && sizes[c] > 0) {
					int32_t *p = ecs_get(w, tr[k].e, comp[c]);
					CHECK(p && *p == expect_tag(k, c), "a live component's data still matches its fingerprint after other entities' moves");
				}
			}
		}
	}

	/* every entity is still exactly where the tracker thinks, one final full pass */
	int alive_count = 0;
	for (int i = 0; i < NENT; i++) if (tr[i].alive) alive_count++;
	CHECK(ecs_entity_count(w) == alive_count, "the world's live count matches the tracker's");

	for (int c = 0; c < NCOMP; c++) {
		int expected = 0;
		for (int i = 0; i < NENT; i++) if (tr[i].alive && tr[i].has[c]) expected++;
		CHECK(ecs_type_count_entities(w, comp[c]) == expected, "a component's world-wide count matches the tracker's, for every type");
	}

	/* bulk/table iteration over a couple of component combinations must visit exactly the tracked set too */
	for (int c = 0; c < NCOMP; c++) {
		int qc[1] = {comp[c]};
		ecs_view *v = ecs_view_new(w, qc, 1);
		int seen = 0;
		while (ecs_view_next_table(v)) {
			int n = ecs_view_count(v);
			const ecs_entity *ents = ecs_view_entities(v);
			for (int r = 0; r < n; r++) {
				seen++;
				int slot = -1;
				for (int i = 0; i < NENT; i++) if (tr[i].alive && tr[i].e == ents[r]) { slot = i; break; }
				CHECK(slot >= 0 && tr[slot].has[c], "every entity a view visits for component c is one the tracker says really has it");
			}
		}
		int expected = 0;
		for (int i = 0; i < NENT; i++) if (tr[i].alive && tr[i].has[c]) expected++;
		CHECK(seen == expected, "bulk iteration's total visit count matches the tracker's, for every component");
		ecs_view_free(v);
	}

	if (fails) printf("stress: %d failed\n", fails);
	else printf("stress: ok (%d ops, %d entities)\n", NOPS, NENT);
	ecs_world_free(w);
	return fails != 0;
}
