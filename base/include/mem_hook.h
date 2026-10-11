/* mem.h - the one place every allocation in this library goes through, so a host (Godot's memalloc, a game's arena, a leak tracker)
 * can take it over with a single call instead of patching every module. Defaults to libc. Set the hooks BEFORE allocating anything/table
 * and don't change them while anything is alive - memory is freed with the same hook set it was allocated with.
 *
 * Aligned allocations (columns that SIMD code walks) go through hc_malloc_aligned/hc_free_aligned, because not every host allocator
 * can free an aligned block with its plain free. */
#ifndef HC_MEM_H
#define HC_MEM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hc_allocator {
	void *(*alloc)(size_t size, void *user);
	void *(*realloc)(void *p, size_t size, void *user);
	void (*free)(void *p, void *user);
	void *(*alloc_aligned)(size_t size, size_t align, void *user);   /* align is a power of two */
	void (*free_aligned)(void *p, void *user);
	void *user;
} hc_allocator;

/* NULL restores the libc defaults. The struct is copied. */
void hc_set_allocator(const hc_allocator *a);

void *hc_malloc(size_t size);
void *hc_calloc(size_t n, size_t size);
void *hc_realloc(void *p, size_t size);
void hc_free(void *p);
void *hc_malloc_aligned(size_t size, size_t align);
void hc_free_aligned(void *p);

#ifdef __cplusplus
}
#endif
#endif

#ifdef MEM_HOOK_IMPLEMENTATION
#ifndef MEM_HOOK_IMPLEMENTATION_DONE_
#define MEM_HOOK_IMPLEMENTATION_DONE_
/* hc_mem.c - see mem.h. */

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

static hc_allocator g_a = { d_alloc, d_realloc, d_free, d_alloc_aligned, d_free_aligned, NULL };

void hc_set_allocator(const hc_allocator *a)
{
	if (!a) {
		g_a = (hc_allocator){ d_alloc, d_realloc, d_free, d_alloc_aligned, d_free_aligned, NULL };
		return;
	}
	g_a = *a;
	if (!g_a.alloc) g_a.alloc = d_alloc;
	if (!g_a.realloc) g_a.realloc = d_realloc;
	if (!g_a.free) g_a.free = d_free;
	if (!g_a.alloc_aligned) g_a.alloc_aligned = d_alloc_aligned;
	if (!g_a.free_aligned) g_a.free_aligned = d_free_aligned;
}

void *hc_malloc(size_t size) { return g_a.alloc(size, g_a.user); }
void *hc_calloc(size_t n, size_t size)
{
	size_t total;
	void *p;
	if (size && n > (size_t)-1 / size) return NULL;
	total = n * size;
	p = g_a.alloc(total ? total : 1, g_a.user);
	if (p) memset(p, 0, total ? total : 1);
	return p;
}
void *hc_realloc(void *p, size_t size) { return g_a.realloc(p, size, g_a.user); }
void hc_free(void *p) { if (p) g_a.free(p, g_a.user); }
void *hc_malloc_aligned(size_t size, size_t align) { return g_a.alloc_aligned(size, align, g_a.user); }
void hc_free_aligned(void *p) { if (p) g_a.free_aligned(p, g_a.user); }
#endif
#endif /* MEM_HOOK_IMPLEMENTATION */
