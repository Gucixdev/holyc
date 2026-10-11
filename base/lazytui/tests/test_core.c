/* test_core.c - the arena and the priority queue. */
#include "lazytui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int fails;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); fails++; } } while (0)

int main(void)
{
	/* arena */
	lt_arena *a = lt_arena_new(4096);
	char *s = lt_arena_strdup(a, "hello");
	int *v = lt_arena_alloc(a, 100 * sizeof *v);
	for (int i = 0; i < 100; i++) v[i] = i;
	CHECK(!strcmp(s, "hello") && v[99] == 99 && ((uintptr_t)v % 16) == 0, "allocations are intact and aligned");
	char *big = lt_arena_alloc(a, 100000);
	memset(big, 7, 100000);
	CHECK(big[99999] == 7 && !strcmp(s, "hello"), "a request larger than a chunk gets its own chunk");
	size_t before = lt_arena_used(a);
	lt_arena_mark m = lt_arena_save(a);
	double *tmp = lt_arena_calloc(a, 5000 * sizeof *tmp);
	CHECK(tmp[4999] == 0, "calloc is zeroed");
	lt_arena_restore(a, m);
	CHECK(lt_arena_used(a) == before, "restore rewinds to the mark");
	CHECK(!strcmp(s, "hello") && v[50] == 50, "older allocations survive a restore");
	lt_arena_reset(a);
	CHECK(lt_arena_used(a) == 0, "reset empties the arena");
	int *w = lt_arena_alloc(a, 64);
	w[0] = 1;
	for (int i = 0; i < 1000; i++) { lt_arena_mark k = lt_arena_save(lt_scratch()); char *t = lt_arena_alloc(lt_scratch(), 1 + (size_t)i * 37); t[0] = 1; lt_arena_restore(lt_scratch(), k); }
	CHECK(lt_arena_used(lt_scratch()) == 0, "the scratch arena is balanced after mark/restore loops");
	lt_arena_free(a);

	/* priority queue: order, FIFO ties, remove, growth */
	lt_pq *q = lt_pq_new();
	unsigned rng = 5;
	for (int i = 0; i < 2000; i++) { rng = rng * 1664525u + 1013904223u; lt_pq_push(q, (double)((rng >> 8) % 500), i, NULL); }
	CHECK(lt_pq_len(q) == 2000, "2000 entries");
	double last = -1;
	int bad = 0;
	for (int i = 0; i < 2000; i++) { double k; if (!lt_pq_pop(q, &k, NULL, NULL) || k < last) bad++; last = k; }
	CHECK(!bad && lt_pq_len(q) == 0, "pops come out in key order");
	CHECK(!lt_pq_pop(q, NULL, NULL, NULL) && !lt_pq_peek(q, NULL, NULL, NULL), "empty queue");
	for (int i = 0; i < 5; i++) lt_pq_push(q, 1.0, i, NULL);
	int id, order_ok = 1;
	for (int i = 0; i < 5; i++) { lt_pq_pop(q, NULL, &id, NULL); if (id != i) order_ok = 0; }
	CHECK(order_ok, "equal keys are FIFO");
	int payload = 42;
	lt_pq_push(q, 3, 10, &payload); lt_pq_push(q, 1, 11, NULL); lt_pq_push(q, 2, 12, NULL);
	CHECK(lt_pq_remove(q, 11) && !lt_pq_remove(q, 99), "remove by id");
	void *p;
	double k;
	lt_pq_peek(q, &k, &id, &p);
	CHECK(k == 2 && id == 12, "the removed entry is gone, the next is on top");
	lt_pq_pop(q, NULL, NULL, NULL); lt_pq_pop(q, &k, &id, &p);
	CHECK(id == 10 && p == &payload, "the pointer travels with the entry");
	CHECK(!lt_pq_push(q, 0.0 / 0.0, 1, NULL), "NaN keys are refused");
	/* remove from the middle of a big heap keeps the heap valid */
	for (int i = 0; i < 300; i++) { rng = rng * 1664525u + 1013904223u; lt_pq_push(q, (double)((rng >> 8) % 1000), 1000 + i, NULL); }
	for (int i = 0; i < 300; i += 3) lt_pq_remove(q, 1000 + i);
	last = -1; bad = 0;
	while (lt_pq_pop(q, &k, NULL, NULL)) { if (k < last) bad++; last = k; }
	CHECK(!bad, "the heap stays ordered after removals");
	lt_pq_free(q);

	printf(fails ? "core: %d failed\n" : "core: ok\n", fails);
	return fails != 0;
}
