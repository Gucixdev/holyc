/* hc_arena.c - see arena.h. */
#include "arena.h"

#include <stdint.h>
#include <string.h>

#include "mem.h"

#define ALIGN16(n) (((n) + 15u) & ~(size_t)15u)

typedef struct chunk { struct chunk *next; size_t cap, used; } chunk;
#define CHUNK_HDR ALIGN16(sizeof(chunk))
#define CHUNK_DATA(c) ((unsigned char *)(c) + CHUNK_HDR)

struct hc_arena {
	chunk *head;             /* newest first */
	chunk *first;            /* the one reset keeps */
	size_t chunk_size;
};

static chunk *chunk_new(size_t cap)
{
	chunk *c = hc_malloc_aligned(CHUNK_HDR + cap, 32);
	if (!c) return NULL;
	c->next = NULL;
	c->cap = cap;
	c->used = 0;
	return c;
}

hc_arena *hc_arena_new(size_t chunk_size)
{
	hc_arena *a = hc_calloc(1, sizeof *a);
	if (!a) return NULL;
	a->chunk_size = chunk_size < 4096 ? 4096 : chunk_size;
	return a;
}

void hc_arena_free(hc_arena *a)
{
	if (!a) return;
	for (chunk *c = a->head; c;) {
		chunk *n = c->next;
		hc_free_aligned(c);
		c = n;
	}
	hc_free(a);
}

void *hc_arena_alloc_aligned(hc_arena *a, size_t n, size_t align)
{
	if (!a) return NULL;
	if (n == 0) n = 1;
	if (align < 16) align = 16;
	if (align & (align - 1)) return NULL;
	chunk *c = a->head;
	for (int attempt = 0; attempt < 2; attempt++) {
		if (c) {
			uintptr_t base = (uintptr_t)(CHUNK_DATA(c) + c->used);
			uintptr_t aligned = (base + (align - 1)) & ~(uintptr_t)(align - 1);
			size_t pad = (size_t)(aligned - base);
			if (c->cap - c->used >= pad && c->cap - c->used - pad >= n) {
				c->used += pad + n;
				return (void *)aligned;
			}
		}
		size_t cap = n + align > a->chunk_size ? n + align : a->chunk_size;
		chunk *nc = chunk_new(cap);
		if (!nc) return NULL;
		nc->next = a->head;
		a->head = nc;
		if (!a->first) a->first = nc;
		c = nc;
	}
	return NULL;
}

void *hc_arena_alloc(hc_arena *a, size_t n) { return hc_arena_alloc_aligned(a, ALIGN16(n ? n : 1), 16); }

void *hc_arena_calloc(hc_arena *a, size_t n)
{
	void *p = hc_arena_alloc(a, n);
	if (p) memset(p, 0, n);
	return p;
}

char *hc_arena_strdup(hc_arena *a, const char *s)
{
	size_t n = strlen(s) + 1;
	char *p = hc_arena_alloc(a, n);
	if (p) memcpy(p, s, n);
	return p;
}

hc_arena_mark hc_arena_mark_get(const hc_arena *a)
{
	hc_arena_mark m = { a->head, a->head ? a->head->used : 0 };
	return m;
}

void hc_arena_rewind(hc_arena *a, hc_arena_mark m)
{
	while (a->head && a->head != m.chunk) {              /* drop chunks created after the mark */
		chunk *c = a->head;
		a->head = c->next;
		if (c == a->first) a->first = NULL;
		hc_free_aligned(c);
	}
	if (a->head) a->head->used = m.used;
	if (!a->head) a->first = NULL;
}

void hc_arena_reset(hc_arena *a)
{
	chunk *keep = NULL;
	for (chunk *c = a->head; c;) {
		chunk *n = c->next;
		if (c == a->first && !keep) { keep = c; keep->next = NULL; keep->used = 0; }
		else hc_free_aligned(c);
		c = n;
	}
	a->head = keep;
	a->first = keep;
}

size_t hc_arena_used(const hc_arena *a)
{
	size_t u = 0;
	for (chunk *c = a->head; c; c = c->next) u += c->used;
	return u;
}

size_t hc_arena_reserved(const hc_arena *a)
{
	size_t r = 0;
	for (chunk *c = a->head; c; c = c->next) r += c->cap + CHUNK_HDR;
	return r;
}

/* ---- slab ------------------------------------------------------------------------------------------------------------------- */

#define FREE_MAGIC 0xFEEDFACECAFEBEEFULL

typedef struct slab_chunk { struct slab_chunk *next; } slab_chunk;
typedef struct free_block { struct free_block *next; uint64_t magic; } free_block;

struct hc_slab {
	size_t block, align, per_chunk;
	slab_chunk *chunks;
	free_block *free_list;
	size_t used, capacity;
};

static size_t slab_hdr(const hc_slab *p) { return (sizeof(slab_chunk) + p->align - 1) & ~(p->align - 1); }

static bool slab_grow(hc_slab *p)
{
	size_t hdr = slab_hdr(p);
	slab_chunk *c = hc_malloc_aligned(hdr + p->block * p->per_chunk, p->align);
	if (!c) return false;
	c->next = p->chunks;
	p->chunks = c;
	unsigned char *base = (unsigned char *)c + hdr;
	for (size_t i = p->per_chunk; i-- > 0;) {                /* push in reverse so allocation walks the chunk forward */
		free_block *b = (free_block *)(base + i * p->block);
		b->magic = FREE_MAGIC;
		b->next = p->free_list;
		p->free_list = b;
	}
	p->capacity += p->per_chunk;
	return true;
}

hc_slab *hc_slab_new(size_t block_size, size_t align, size_t blocks_per_chunk)
{
	if (align < 16) align = 16;
	if (align & (align - 1)) return NULL;
	if (block_size < sizeof(free_block)) block_size = sizeof(free_block);
	hc_slab *p = hc_calloc(1, sizeof *p);
	if (!p) return NULL;
	p->align = align;
	p->block = (block_size + align - 1) & ~(align - 1);
	p->per_chunk = blocks_per_chunk ? blocks_per_chunk : 64;
	return p;
}

void hc_slab_destroy(hc_slab *p)
{
	if (!p) return;
	for (slab_chunk *c = p->chunks; c;) {
		slab_chunk *n = c->next;
		hc_free_aligned(c);
		c = n;
	}
	hc_free(p);
}

void *hc_slab_alloc(hc_slab *p)
{
	if (!p->free_list && !slab_grow(p)) return NULL;
	free_block *b = p->free_list;
	p->free_list = b->next;
	memset(b, 0, p->block);
	p->used++;
	return b;
}

bool hc_slab_release(hc_slab *p, void *block)
{
	if (!p || !block) return false;
	free_block *b = block;
	if (b->magic == FREE_MAGIC) return false;                /* already free: a double free, refuse */
	b->magic = FREE_MAGIC;
	b->next = p->free_list;
	p->free_list = b;
	p->used--;
	return true;
}

void hc_slab_reset(hc_slab *p)
{
	size_t hdr = slab_hdr(p);
	p->free_list = NULL;
	for (slab_chunk *c = p->chunks; c; c = c->next) {
		unsigned char *base = (unsigned char *)c + hdr;
		for (size_t i = p->per_chunk; i-- > 0;) {
			free_block *b = (free_block *)(base + i * p->block);
			b->magic = FREE_MAGIC;
			b->next = p->free_list;
			p->free_list = b;
		}
	}
	p->used = 0;
}

size_t hc_slab_used(const hc_slab *p) { return p->used; }
size_t hc_slab_capacity(const hc_slab *p) { return p->capacity; }
