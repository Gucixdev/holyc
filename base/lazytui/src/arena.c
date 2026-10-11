/* arena.c - a bump allocator: allocation is a pointer add, freeing is resetting the whole arena at once. Two uses in the library:
 *  - the frame arena of lt_ui / the scratch stack of the widgets (mark, allocate temporaries, release): no malloc/free pairs in the hot draw paths;
 *  - any caller who builds many small objects with one lifetime (a parse, one frame's worth of layout data).
 * Memory comes in chunks; a request larger than a chunk gets a chunk of its own. Reset keeps the first chunk, so a steady-state frame allocates nothing. */
#include "internal.h"

typedef struct chunk { struct chunk *next; size_t cap, used; } chunk;
struct lt_arena { chunk *head, *first; size_t chunk_size; size_t total; };

#define ALIGN 16u
static size_t up(size_t n) { return (n + (ALIGN - 1)) & ~(size_t)(ALIGN - 1); }
#define HDR up(sizeof(chunk))

static chunk *chunk_new(size_t cap)
{
	chunk *c = malloc(HDR + cap);
	if (!c) return NULL;
	c->next = NULL; c->cap = cap; c->used = 0;
	return c;
}

lt_arena *lt_arena_new(size_t chunk_size)
{
	lt_arena *a = calloc(1, sizeof *a);
	if (!a) return NULL;
	a->chunk_size = chunk_size < 4096 ? 4096 : chunk_size;
	return a;
}

void *lt_arena_alloc(lt_arena *a, size_t n)
{
	n = up(n ? n : 1);
	chunk *c = a->head;
	if (!c || c->cap - c->used < n) {
		size_t cap = n > a->chunk_size ? n : a->chunk_size;
		chunk *nc = chunk_new(cap);
		if (!nc) return NULL;
		nc->next = a->head;                       /* newest first; older chunks stay reachable for release */
		a->head = nc;
		if (!a->first) a->first = nc;
		a->total += cap;
		c = nc;
	}
	void *p = (char *)c + HDR + c->used;
	c->used += n;
	return p;
}

void *lt_arena_calloc(lt_arena *a, size_t n)
{
	void *p = lt_arena_alloc(a, n);
	if (p) memset(p, 0, n);
	return p;
}

char *lt_arena_strdup(lt_arena *a, const char *s)
{
	size_t n = strlen(s) + 1;
	char *p = lt_arena_alloc(a, n);
	if (p) memcpy(p, s, n);
	return p;
}

/* a mark is (chunk, used): releasing it frees every chunk allocated after the mark and rewinds the mark's chunk */
lt_arena_mark lt_arena_save(const lt_arena *a) { return (lt_arena_mark){a->head, a->head ? a->head->used : 0}; }
void lt_arena_restore(lt_arena *a, lt_arena_mark m)
{
	while (a->head && a->head != (chunk *)m.chunk) {
		chunk *dead = a->head;
		a->head = dead->next;
		a->total -= dead->cap;
		if (dead == a->first) a->first = NULL;
		free(dead);
	}
	if (a->head) a->head->used = m.used;
	if (!a->head) a->first = NULL;
}

void lt_arena_reset(lt_arena *a)
{
	chunk *keep = NULL;
	for (chunk *c = a->head; c;) {                /* keep the largest-standing-first chunk: the oldest one, sized for a normal frame */
		chunk *n = c->next;
		if (!n && c->cap >= a->chunk_size) keep = c;
		else { a->total -= c->cap; free(c); }
		c = n;
	}
	a->head = a->first = keep;
	if (keep) keep->used = 0;
}

size_t lt_arena_used(const lt_arena *a) { size_t u = 0; for (chunk *c = a->head; c; c = c->next) u += c->used; return u; }

void lt_arena_free(lt_arena *a)
{
	if (!a) return;
	for (chunk *c = a->head; c;) { chunk *n = c->next; free(c); c = n; }
	free(a);
}

/* ---- the scratch stack used by widgets: one arena for the whole (single-threaded) library ---- */
static lt_arena *scratch;
lt_arena *lt_scratch(void) { if (!scratch) scratch = lt_arena_new(64 * 1024); return scratch; }
