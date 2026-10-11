#ifndef HOLYC_RAII_H_
#define HOLYC_RAII_H_

/* RAII-shaped scoped cleanup, via GCC/Clang's `__attribute__((cleanup))`
 * extension - the standard, well-known way to get destructor-like
 * behavior in plain C (no C++ needed), already in the same family of
 * compiler extensions this project leans on elsewhere (__builtin_trap
 * in base.h's HOLYC_ASSERT, __sync_synchronize in ring_buffer.h). Not
 * for Arena-owned memory (Arena frees everything together on purpose,
 * nothing to individually clean up - see arena_alloc.h) - this is for
 * non-memory resources with a real release step: a FILE*, a mutex
 * held, a socket, anything gc.c's tracing model or Arena's bump
 * model don't apply to.
 *
 * Generic over WHAT is being cleaned up via a plain finalizer function
 * pointer + void* resource, same "caller supplies the type-specific
 * bit" shape as gc.c's Gc_Trace_Fn / react.h's Signal_Fn. */

#include "core/base.h"

typedef void (*Raii_Finalizer)(void *resource);

typedef struct {
    void *resource;         /* NULL means "nothing to release" - safe default-initialized state */
    Raii_Finalizer finalizer;
} Raii_Guard;

static void raii_release_(Raii_Guard *g)
{
    if (g->resource && g->finalizer) g->finalizer(g->resource);
}

/* attach to a Raii_Guard local variable declaration:
 *   Raii_Guard g RAII_SCOPED = { my_resource, my_finalizer };
 * my_finalizer(my_resource) then runs automatically at the end of the
 * enclosing scope, on every exit path (normal fall-through, early
 * `return`, `break`, `goto` out of the block - not on longjmp past it,
 * same caveat any stack-unwinding-based cleanup has). */
#define RAII_SCOPED __attribute__((cleanup(raii_release_)))

#endif /* HOLYC_RAII_H_ */
