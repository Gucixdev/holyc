/* ecs_mem.c - see ecs_mem.h. */
#include "ecs_mem.h"

#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <malloc.h>   /* _aligned_malloc/_aligned_free - MinGW/MSVC have no posix_memalign */
#endif

static void *d_alloc(size_t n, void *u) { (void)u; return malloc(n); }
static void *d_realloc(void *p, size_t n, void *u) { (void)u; return realloc(p, n); }
static void d_free(void *p, void *u) { (void)u; free(p); }
#ifdef _WIN32
static void *d_alloc_aligned(size_t n, size_t align, void *u)
{
	(void)u;
	if (align < sizeof(void *)) align = sizeof(void *);
	return _aligned_malloc(n ? n : 1, align);
}
static void d_free_aligned(void *p, void *u) { (void)u; _aligned_free(p); }
#else
static void *d_alloc_aligned(size_t n, size_t align, void *u)
{
	void *p = NULL;
	(void)u;
	if (align < sizeof(void *)) align = sizeof(void *);
	return posix_memalign(&p, align, n ? n : 1) == 0 ? p : NULL;
}
static void d_free_aligned(void *p, void *u) { (void)u; free(p); }
#endif

static ecs_allocator g_a = { d_alloc, d_realloc, d_free, d_alloc_aligned, d_free_aligned, NULL };

void ecs_set_allocator(const ecs_allocator *a)
{
	if (!a) {
		g_a = (ecs_allocator){ d_alloc, d_realloc, d_free, d_alloc_aligned, d_free_aligned, NULL };
		return;
	}
	g_a = *a;
	if (!g_a.alloc) g_a.alloc = d_alloc;
	if (!g_a.realloc) g_a.realloc = d_realloc;
	if (!g_a.free) g_a.free = d_free;
	if (!g_a.alloc_aligned) g_a.alloc_aligned = d_alloc_aligned;
	if (!g_a.free_aligned) g_a.free_aligned = d_free_aligned;
}

void *ecs_malloc(size_t size) { return g_a.alloc(size, g_a.user); }
void *ecs_calloc(size_t n, size_t size)
{
	size_t total;
	void *p;
	if (size && n > (size_t)-1 / size) return NULL;
	total = n * size;
	p = g_a.alloc(total ? total : 1, g_a.user);
	if (p) memset(p, 0, total ? total : 1);
	return p;
}
void *ecs_realloc(void *p, size_t size) { return g_a.realloc(p, size, g_a.user); }
void ecs_free(void *p) { if (p) g_a.free(p, g_a.user); }
void *ecs_malloc_aligned(size_t size, size_t align) { return g_a.alloc_aligned(size, align, g_a.user); }
void ecs_free_aligned(void *p) { if (p) g_a.free_aligned(p, g_a.user); }
