/* pq.c - a binary min-heap priority queue keyed by a double (a due time, a cost, a score), ties broken by insertion order so equal keys
 * come out FIFO. Entries carry an int id and a pointer; lt_pq_remove cancels by id. Used by the event loop's timer scheduler and by
 * anything that needs "the next thing due" / "the best K" without sorting. */
#include "internal.h"

typedef struct { double key; unsigned long long seq; int id; void *ptr; } ent;
struct lt_pq { ent *e; size_t n, cap; unsigned long long seq; };

lt_pq *lt_pq_new(void) { return calloc(1, sizeof(lt_pq)); }
void lt_pq_free(lt_pq *q) { if (q) { free(q->e); free(q); } }
size_t lt_pq_len(const lt_pq *q) { return q->n; }
void lt_pq_clear(lt_pq *q) { q->n = 0; }

static bool less(const ent *a, const ent *b) { return a->key < b->key || (a->key == b->key && a->seq < b->seq); }

static void up(lt_pq *q, size_t i)
{
	ent x = q->e[i];
	while (i > 0) {
		size_t p = (i - 1) / 2;
		if (!less(&x, &q->e[p])) break;
		q->e[i] = q->e[p];
		i = p;
	}
	q->e[i] = x;
}
static void down(lt_pq *q, size_t i)
{
	ent x = q->e[i];
	for (;;) {
		size_t l = 2 * i + 1, r = l + 1, m = i;
		const ent *best = &x;
		if (l < q->n && less(&q->e[l], best)) { m = l; best = &q->e[l]; }
		if (r < q->n && less(&q->e[r], best)) m = r;
		if (m == i) break;
		q->e[i] = q->e[m];
		i = m;
	}
	q->e[i] = x;
}

bool lt_pq_push(lt_pq *q, double key, int id, void *ptr)
{
	if (key != key) return false;                                   /* NaN would break the order */
	if (q->n == q->cap) {
		size_t nc = q->cap ? q->cap * 2 : 16;
		ent *ne = realloc(q->e, nc * sizeof *ne);
		if (!ne) return false;
		q->e = ne; q->cap = nc;
	}
	q->e[q->n] = (ent){key, q->seq++, id, ptr};
	up(q, q->n++);
	return true;
}

bool lt_pq_peek(const lt_pq *q, double *key, int *id, void **ptr)
{
	if (!q->n) return false;
	if (key) *key = q->e[0].key;
	if (id) *id = q->e[0].id;
	if (ptr) *ptr = q->e[0].ptr;
	return true;
}

bool lt_pq_pop(lt_pq *q, double *key, int *id, void **ptr)
{
	if (!lt_pq_peek(q, key, id, ptr)) return false;
	q->e[0] = q->e[--q->n];
	if (q->n) down(q, 0);
	return true;
}

bool lt_pq_remove(lt_pq *q, int id)
{
	for (size_t i = 0; i < q->n; i++)
		if (q->e[i].id == id) {
			q->e[i] = q->e[--q->n];
			if (i < q->n) { up(q, i); down(q, i); }
			return true;
		}
	return false;
}
