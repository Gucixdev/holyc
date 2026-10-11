/* test_slab.c - slab and the arena_calloc/used/reserved additions in arena_alloc.h:
 * zeroed blocks, pointer stability across growth, reuse, double release refused, reset. */
#define ARENA_ALLOC_IMPLEMENTATION
#include "core/arena_alloc.h"
#include <stdint.h>
#include <stdio.h>

static int fails;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); fails++; } } while (0)

int main(void)
{
	typedef struct { uint64_t a, b, c; } item;
	Slab *sl = slab_new(sizeof(item), 8);
	item *it[40];
	size_t i, cap;
	int ok = 1;
	item *victim, *again;
	Arena ar = {0};
	unsigned char *z;

	for (i = 0; i < 40; i++) {
		it[i] = slab_alloc(sl);
		CHECK(it[i] && it[i]->a == 0 && it[i]->c == 0, "blocks come back zeroed");
		CHECK(((uintptr_t)it[i] % 16) == 0, "16-byte aligned");
		it[i]->a = (uint64_t)i + 1000;
	}
	CHECK(slab_used(sl) == 40 && slab_capacity(sl) >= 40, "usage and growth tracked");
	for (i = 0; i < 40; i++) if (it[i]->a != (uint64_t)i + 1000) ok = 0;
	CHECK(ok, "pointers stay valid across growth");
	victim = it[7];
	CHECK(slab_release(sl, victim), "release succeeds");
	CHECK(!slab_release(sl, victim), "double release is refused");
	CHECK(slab_used(sl) == 39, "double release did not corrupt the count");
	again = slab_alloc(sl);
	CHECK(again == victim && again->a == 0, "released block is reused and re-zeroed");
	CHECK(!slab_release(sl, NULL), "NULL release is a refused no-op");
	cap = slab_capacity(sl);
	slab_reset(sl);
	CHECK(slab_used(sl) == 0 && slab_capacity(sl) == cap, "reset frees every block, keeps memory");
	for (i = 0; i < cap; i++) CHECK(slab_alloc(sl) != NULL, "all blocks allocatable after reset");
	CHECK(slab_capacity(sl) == cap, "no growth needed");
	slab_destroy(sl);

	z = arena_calloc(&ar, 200);
	ok = 1;
	for (i = 0; i < 200; i++) if (z[i]) ok = 0;
	CHECK(ok, "arena_calloc zeroes");
	CHECK(arena_used(&ar) >= 200 && arena_reserved(&ar) >= arena_used(&ar), "used/reserved");
	arena_free(&ar);

	if (fails) { printf("%d FAILED\n", fails); return 1; }
	printf("test_slab: ok\n");
	return 0;
}
