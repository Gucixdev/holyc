/* ecs_arena.c - see ecs_arena.h. */
#include "ecs_arena.h"

#include <stdint.h>
#include <string.h>

#include "ecs_mem.h"

#define ALIGN16(n) (((n) + 15u) & ~(size_t)15u)

typedef struct chunk { struct chunk *next; size_t cap, used; } chunk;
#define CHUNK_HDR ALIGN16(sizeof(chunk))
#define CHUNK_DATA(c) ((unsigned char *)(c) + CHUNK_HDR)

struct ecs_arena {
	chunk *head;             /* newest first */
	chunk *first;            /* the one reset keeps */
	size_t chunk_size;
};

static chunk *chunk_new(size_t cap)
{
	chunk *c = ecs_malloc_aligned(CHUNK_HDR + cap, 32);
	if (!c) return NULL;
	c->next = NULL;
	c->cap = cap;
	c->used = 0;
	return c;
}

ecs_arena *ecs_arena_new(size_t chunk_size)
{
	ecs_arena *a = ecs_calloc(1, sizeof *a);
	if (!a) return NULL;
	a->chunk_size = chunk_size < 4096 ? 4096 : chunk_size;
	return a;
}

void ecs_arena_free(ecs_arena *a)
{
	if (!a) return;
	for (chunk *c = a->head; c;) {
		chunk *n = c->next;
		ecs_free_aligned(c);
		c = n;
	}
	ecs_free(a);
}

void *ecs_arena_alloc_aligned(ecs_arena *a, size_t n, size_t align)
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

void *ecs_arena_alloc(ecs_arena *a, size_t n) { return ecs_arena_alloc_aligned(a, ALIGN16(n ? n : 1), 16); }

void *ecs_arena_calloc(ecs_arena *a, size_t n)
{
	void *p = ecs_arena_alloc(a, n);
	if (p) memset(p, 0, n);
	return p;
}

char *ecs_arena_strdup(ecs_arena *a, const char *s)
{
	size_t n = strlen(s) + 1;
	char *p = ecs_arena_alloc(a, n);
	if (p) memcpy(p, s, n);
	return p;
}

ecs_arena_mark ecs_arena_mark_get(const ecs_arena *a)
{
	ecs_arena_mark m = { a->head, a->head ? a->head->used : 0 };
	return m;
}

void ecs_arena_rewind(ecs_arena *a, ecs_arena_mark m)
{
	while (a->head && a->head != m.chunk) {              /* drop chunks created after the mark */
		chunk *c = a->head;
		a->head = c->next;
		if (c == a->first) a->first = NULL;
		ecs_free_aligned(c);
	}
	if (a->head) a->head->used = m.used;
	if (!a->head) a->first = NULL;
}

void ecs_arena_reset(ecs_arena *a)
{
	chunk *keep = NULL;
	for (chunk *c = a->head; c;) {
		chunk *n = c->next;
		if (c == a->first && !keep) { keep = c; keep->next = NULL; keep->used = 0; }
		else ecs_free_aligned(c);
		c = n;
	}
	a->head = keep;
	a->first = keep;
}

size_t ecs_arena_used(const ecs_arena *a)
{
	size_t u = 0;
	for (chunk *c = a->head; c; c = c->next) u += c->used;
	return u;
}

size_t ecs_arena_reserved(const ecs_arena *a)
{
	size_t r = 0;
	for (chunk *c = a->head; c; c = c->next) r += c->cap + CHUNK_HDR;
	return r;
}

/* ---- slab ------------------------------------------------------------------------------------------------------------------- */

#define FREE_MAGIC 0xFEEDFACECAFEBEEFULL

typedef struct slab_chunk { struct slab_chunk *next; } slab_chunk;
typedef struct free_block { struct free_block *next; uint64_t magic; } free_block;

struct ecs_slab {
	size_t block, align, per_chunk;
	slab_chunk *chunks;
	free_block *free_list;
	size_t used, capacity;
};

static size_t slab_hdr(const ecs_slab *p) { return (sizeof(slab_chunk) + p->align - 1) & ~(p->align - 1); }

static bool slab_grow(ecs_slab *p)
{
	size_t hdr = slab_hdr(p);
	slab_chunk *c = ecs_malloc_aligned(hdr + p->block * p->per_chunk, p->align);
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

ecs_slab *ecs_slab_new(size_t block_size, size_t align, size_t blocks_per_chunk)
{
	if (align < 16) align = 16;
	if (align & (align - 1)) return NULL;
	if (block_size < sizeof(free_block)) block_size = sizeof(free_block);
	ecs_slab *p = ecs_calloc(1, sizeof *p);
	if (!p) return NULL;
	p->align = align;
	p->block = (block_size + align - 1) & ~(align - 1);
	p->per_chunk = blocks_per_chunk ? blocks_per_chunk : 64;
	return p;
}

void ecs_slab_destroy(ecs_slab *p)
{
	if (!p) return;
	for (slab_chunk *c = p->chunks; c;) {
		slab_chunk *n = c->next;
		ecs_free_aligned(c);
		c = n;
	}
	ecs_free(p);
}

void *ecs_slab_alloc(ecs_slab *p)
{
	if (!p->free_list && !slab_grow(p)) return NULL;
	free_block *b = p->free_list;
	p->free_list = b->next;
	memset(b, 0, p->block);
	p->used++;
	return b;
}

bool ecs_slab_release(ecs_slab *p, void *block)
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

void ecs_slab_reset(ecs_slab *p)
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

size_t ecs_slab_used(const ecs_slab *p) { return p->used; }
size_t ecs_slab_capacity(const ecs_slab *p) { return p->capacity; }
