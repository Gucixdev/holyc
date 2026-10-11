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
