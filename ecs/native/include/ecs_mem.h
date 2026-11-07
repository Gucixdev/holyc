/* ecs_mem.h - the one place every allocation in this library goes through, so a host (Godot's memalloc, a game's arena, a leak tracker)
 * can take it over with a single call instead of patching every module. Defaults to libc. Set the hooks BEFORE creating any world/table
 * and don't change them while anything is alive - memory is freed with the same hook set it was allocated with.
 *
 * Aligned allocations (columns that SIMD code walks) go through ecs_malloc_aligned/ecs_free_aligned, because not every host allocator
 * can free an aligned block with its plain free. */
#ifndef ECS_MEM_H
#define ECS_MEM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ecs_allocator {
	void *(*alloc)(size_t size, void *user);
	void *(*realloc)(void *p, size_t size, void *user);
	void (*free)(void *p, void *user);
	void *(*alloc_aligned)(size_t size, size_t align, void *user);   /* align is a power of two */
	void (*free_aligned)(void *p, void *user);
	void *user;
} ecs_allocator;

/* NULL restores the libc defaults. The struct is copied. */
void ecs_set_allocator(const ecs_allocator *a);

void *ecs_malloc(size_t size);
void *ecs_calloc(size_t n, size_t size);
void *ecs_realloc(void *p, size_t size);
void ecs_free(void *p);
void *ecs_malloc_aligned(size_t size, size_t align);
void ecs_free_aligned(void *p);

#ifdef __cplusplus
}
#endif
#endif
