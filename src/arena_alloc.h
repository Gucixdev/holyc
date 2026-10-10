/* Bump-pointer arena allocator, trimmed/adapted for this fork from
 * Gucixdev's own singlefile/base/abstract/arena_alloc.h (itself a C89 port
 * of tsoding's arena.h, https://github.com/tsoding/arena). Dependencies on
 * that project's base.h/contracts.h dropped - this copy is self-contained
 * so it builds standalone as part of nnn.c's single translation unit.
 *
 * Used by dentfill() (src/nnn.c) to fix a real O(n^2) cost on huge
 * directories: the old code grew pdents/pnamebuf by a small FIXED
 * increment via realloc, and every time pnamebuf's realloc happened to
 * move memory, it re-walked every entry parsed so far to fix up name
 * pointers into the moved buffer. The FLAT backend below reserves a big
 * chunk of virtual address space up front (committed lazily, page by
 * page, only where actually written) and never moves it, so names can be
 * arena_strdup'd permanently - no fixup loop needed at all - and pdents
 * itself grows geometrically instead of by a fixed step.
 */
#ifndef NNN_ARENA_ALLOC_H_
#define NNN_ARENA_ALLOC_H_

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#ifndef ARENA_ASSERT
#define ARENA_ASSERT assert
#endif

#define ARENA_BACKEND_LIBC_MALLOC 0
#define ARENA_BACKEND_LINUX_MMAP  1
#define ARENA_BACKEND_FLAT        2

#ifndef ARENA_BACKEND
#define ARENA_BACKEND ARENA_BACKEND_FLAT
#endif

typedef struct Arena_Region Arena_Region;

struct Arena_Region {
	Arena_Region *next;
	size_t count;
	size_t capacity;
	uint64_t data[1]; /* struct-hack: real allocation is offsetof(data) + capacity*sizeof(u64) */
};

typedef struct {
	Arena_Region *begin, *end;
} Arena;

#ifndef ARENA_REGION_DEFAULT_CAPACITY
#define ARENA_REGION_DEFAULT_CAPACITY ((size_t)8 * 1024) /* u64 words */
#endif

Arena_Region *new_region(size_t capacity);
void free_region(Arena_Region *r);

void *arena_alloc(Arena *a, size_t size_bytes);
void *arena_realloc(Arena *a, void *oldptr, size_t oldsz, size_t newsz);
char *arena_strdup(Arena *a, const char *cstr);
void arena_reset(Arena *a);
void arena_free(Arena *a);

#endif /* NNN_ARENA_ALLOC_H_ */

#ifdef ARENA_ALLOC_IMPLEMENTATION
#ifndef ARENA_ALLOC_IMPLEMENTATION_
#define ARENA_ALLOC_IMPLEMENTATION_

#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#if ARENA_BACKEND == ARENA_BACKEND_LIBC_MALLOC

Arena_Region *new_region(size_t capacity)
{
	Arena_Region *r;
	size_t size_bytes = offsetof(Arena_Region, data) + sizeof(uint64_t) * capacity;
	r = (Arena_Region *)malloc(size_bytes);
	ARENA_ASSERT(r);
	r->next = NULL;
	r->count = 0;
	r->capacity = capacity;
	return r;
}

void free_region(Arena_Region *r)
{
	free(r);
}

#elif ARENA_BACKEND == ARENA_BACKEND_LINUX_MMAP

Arena_Region *new_region(size_t capacity)
{
	Arena_Region *r;
	size_t size_bytes = offsetof(Arena_Region, data) + sizeof(uint64_t) * capacity;
	r = (Arena_Region *)mmap(NULL, size_bytes, PROT_READ | PROT_WRITE,
			MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	ARENA_ASSERT(r != MAP_FAILED);
	r->next = NULL;
	r->count = 0;
	r->capacity = capacity;
	return r;
}

void free_region(Arena_Region *r)
{
	size_t size_bytes = offsetof(Arena_Region, data) + sizeof(uint64_t) * r->capacity;
	int ret = munmap(r, size_bytes);
	ARENA_ASSERT(ret == 0);
}

#elif ARENA_BACKEND == ARENA_BACKEND_FLAT

/* Reserve one big chunk of VIRTUAL address space up front (MAP_NORESERVE -
 * the kernel backs it with real RAM/swap lazily, page by page, only where
 * something actually gets written), then never mmap() again in the
 * overwhelmingly common case. A directory with millions of entries would
 * need to exceed this reservation before a second region is ever created. */

#ifndef ARENA_FLAT_RESERVE_BYTES
#define ARENA_FLAT_RESERVE_BYTES ((size_t)1 << 28) /* 256MB of address space, reserved not committed */
#endif

Arena_Region *new_region(size_t capacity)
{
	Arena_Region *r;
	size_t requested_bytes = offsetof(Arena_Region, data) + sizeof(uint64_t) * capacity;
	size_t size_bytes = requested_bytes > ARENA_FLAT_RESERVE_BYTES ? requested_bytes : ARENA_FLAT_RESERVE_BYTES;

	r = (Arena_Region *)mmap(NULL, size_bytes, PROT_READ | PROT_WRITE,
			MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
	if (r == MAP_FAILED) {
		/* over-reservation itself failed (tight memory cgroup, or
		 * overcommit_memory=2) - retry for exactly what this call
		 * needs before giving up */
		size_bytes = requested_bytes;
		r = (Arena_Region *)mmap(NULL, size_bytes, PROT_READ | PROT_WRITE,
				MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
	}
	ARENA_ASSERT(r != MAP_FAILED);
	r->next = NULL;
	r->count = 0;
	r->capacity = (size_bytes - offsetof(Arena_Region, data)) / sizeof(uint64_t);
	return r;
}

void free_region(Arena_Region *r)
{
	size_t size_bytes = offsetof(Arena_Region, data) + sizeof(uint64_t) * r->capacity;
	int ret = munmap(r, size_bytes);
	ARENA_ASSERT(ret == 0);
}

#else
#error "Unknown ARENA_BACKEND"
#endif

void *arena_alloc(Arena *a, size_t size_bytes)
{
	size_t size = (size_bytes + sizeof(uint64_t) - 1) / sizeof(uint64_t);
	void *result;

	if (a->end == NULL) {
		size_t capacity = ARENA_REGION_DEFAULT_CAPACITY;
		ARENA_ASSERT(a->begin == NULL);
		if (capacity < size) capacity = size;
		a->end = new_region(capacity);
		a->begin = a->end;
	}

	while (a->end->count + size > a->end->capacity && a->end->next != NULL)
		a->end = a->end->next;

	if (a->end->count + size > a->end->capacity) {
		size_t capacity = ARENA_REGION_DEFAULT_CAPACITY;
		ARENA_ASSERT(a->end->next == NULL);
		if (capacity < size) capacity = size;
		a->end->next = new_region(capacity);
		a->end = a->end->next;
	}

	result = &a->end->data[a->end->count];
	a->end->count += size;
	ARENA_ASSERT(a->end->count <= a->end->capacity);
	return result;
}

void *arena_realloc(Arena *a, void *oldptr, size_t oldsz, size_t newsz)
{
	char *newptr;
	if (newsz <= oldsz) return oldptr;
	newptr = (char *)arena_alloc(a, newsz);
	memcpy(newptr, oldptr, oldsz);
	return newptr;
}

char *arena_strdup(Arena *a, const char *cstr)
{
	size_t n = strlen(cstr);
	char *dup = (char *)arena_alloc(a, n + 1);
	memcpy(dup, cstr, n + 1);
	return dup;
}

void arena_reset(Arena *a)
{
	Arena_Region *r;
	for (r = a->begin; r != NULL; r = r->next)
		r->count = 0;
	a->end = a->begin;
}

void arena_free(Arena *a)
{
	Arena_Region *r = a->begin;
	while (r) {
		Arena_Region *r0 = r;
		r = r->next;
		free_region(r0);
	}
	a->begin = NULL;
	a->end = NULL;
}

#endif /* ARENA_ALLOC_IMPLEMENTATION_ */
#endif /* ARENA_ALLOC_IMPLEMENTATION */
