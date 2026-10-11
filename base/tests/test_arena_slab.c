/* test_arena.c - arena: alignment, growth, mark/rewind, reset reuse, large requests; slab: alloc/release reuse, double free refused,
 * pointer stability across growth, reset, zeroed blocks. */
#define MEM_HOOK_IMPLEMENTATION
#define ARENA_SLAB_IMPLEMENTATION
#include "core/arena_slab.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); fails++; } } while (0)

int main(void)
{
	hc_arena *a = hc_arena_new(4096);
	void *p1 = hc_arena_alloc(a, 1), *p2 = hc_arena_alloc(a, 100);
	CHECK(p1 && p2 && ((uintptr_t)p1 % 16) == 0 && ((uintptr_t)p2 % 16) == 0, "default 16-byte alignment");
	CHECK(p1 != p2, "distinct blocks");
	void *p3 = hc_arena_alloc_aligned(a, 10, 64);
	CHECK(((uintptr_t)p3 % 64) == 0, "explicit 64-byte alignment");
	CHECK(hc_arena_alloc_aligned(a, 8, 24) == NULL, "non power-of-two alignment is refused");
	unsigned char *z = hc_arena_calloc(a, 200);
	int zero = 1;
	for (int i = 0; i < 200; i++) if (z[i]) zero = 0;
	CHECK(zero, "calloc zeroes");
	char *s = hc_arena_strdup(a, "souls");
	CHECK(s && strcmp(s, "souls") == 0, "strdup");

	/* growth keeps old pointers valid (chunks never move) */
	memset(p2, 0x5A, 100);
	for (int i = 0; i < 2000; i++) hc_arena_alloc(a, 64);
	CHECK(((unsigned char *)p2)[99] == 0x5A, "earlier allocations survive growth");
	CHECK(hc_arena_reserved(a) > 4096, "grew into more chunks");

	/* mark / rewind */
	size_t before = hc_arena_used(a);
	hc_arena_mark m = hc_arena_mark_get(a);
	for (int i = 0; i < 500; i++) hc_arena_alloc(a, 128);
	CHECK(hc_arena_used(a) > before, "temporaries counted");
	hc_arena_rewind(a, m);
	CHECK(hc_arena_used(a) == before, "rewind frees everything since the mark");
	void *after = hc_arena_alloc(a, 32);
	CHECK(after != NULL, "arena still usable after a rewind across chunks");

	/* oversized request gets its own chunk */
	void *big = hc_arena_alloc(a, 1 << 20);
	CHECK(big != NULL, "request larger than a chunk");
	memset(big, 1, 1 << 20);

	/* reset keeps one chunk: steady state reserves no more after the first cycle */
	hc_arena_reset(a);
	CHECK(hc_arena_used(a) == 0, "reset empties the arena");
	size_t r1 = hc_arena_reserved(a);
	for (int cycle = 0; cycle < 5; cycle++) {
		for (int i = 0; i < 20; i++) hc_arena_alloc(a, 100);
		hc_arena_reset(a);
	}
	CHECK(hc_arena_reserved(a) == r1, "steady state allocates nothing new");
	hc_arena_free(a);

	/* ---- slab ---- */
	typedef struct { uint64_t a, b, c; } item;
	hc_slab *sl = hc_slab_new(sizeof(item), 16, 8);
	item *it[40];
	for (int i = 0; i < 40; i++) {
		it[i] = hc_slab_alloc(sl);
		CHECK(it[i] && it[i]->a == 0 && it[i]->c == 0, "blocks come back zeroed");
		it[i]->a = (uint64_t)i + 1000;
	}
	CHECK(hc_slab_used(sl) == 40 && hc_slab_capacity(sl) >= 40, "usage and growth tracked");
	int ok = 1;
	for (int i = 0; i < 40; i++) if (it[i]->a != (uint64_t)i + 1000) ok = 0;
	CHECK(ok, "pointers stay valid across growth");
	item *victim = it[7];
	CHECK(hc_slab_release(sl, victim), "release succeeds");
	CHECK(!hc_slab_release(sl, victim), "double free is refused");
	CHECK(hc_slab_used(sl) == 39, "double free did not corrupt the count");
	item *again = hc_slab_alloc(sl);
	CHECK(again == victim && again->a == 0, "freed block is reused, and re-zeroed");
	CHECK(!hc_slab_release(sl, NULL), "NULL release is a refused no-op");
	size_t cap = hc_slab_capacity(sl);
	hc_slab_reset(sl);
	CHECK(hc_slab_used(sl) == 0 && hc_slab_capacity(sl) == cap, "reset frees every block, keeps memory");
	for (size_t i = 0; i < cap; i++) CHECK(hc_slab_alloc(sl) != NULL, "all blocks allocatable after reset");
	CHECK(hc_slab_capacity(sl) == cap, "no growth needed");
	hc_slab_destroy(sl);

	if (fails) { printf("%d FAILED\n", fails); return 1; }
	printf("test_arena: ok\n");
	return 0;
}
