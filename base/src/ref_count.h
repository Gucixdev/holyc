#ifndef HOLYC_REF_COUNT_H_
#define HOLYC_REF_COUNT_H_

/* Manual reference counting - the memory-management "yin" to arena/pool's
 * "yang". Deliberately NOT a replacement for either: arena/pool answer
 * WHERE memory comes from (a growing bump region / a fixed-size slab, see
 * arena_alloc.h and pool_alloc.c); this answers WHO decides WHEN a shared
 * resource is done, for the cases where a single explicit owner isn't the
 * right model (a chunk with more than one live reference to it, a handle
 * shared across subsystems). Compose them: retain/release around a
 * pool-allocated chunk, firing pool_dealloc() as the on_zero callback.
 *
 * No RAII, on purpose - a deliberate scope decision, and rightly: RAII means the compiler inserts the release call
 * for you at scope exit, hidden control flow the reader can't see at the
 * call site. rc_retain()/rc_release() are just functions. You call them
 * by hand, same as arena_alloc()/pool_dealloc() are just functions. If you
 * forget a release, that's a bug to find, not a bug that couldn't exist -
 * same tradeoff as manual malloc/free, and the whole point of this header
 * existing at all is that this project already opted into manual over
 * automatic everywhere else.
 *
 * Atomic by default (not gated behind a "threaded" build option): fgjob.h
 * makes this project genuinely multi-threaded, and a shared reference
 * count that ISN'T safe to retain/release from two worker threads at once
 * would be a footgun sitting right next to the thing that makes it likely
 * to get hit. Same __sync_* builtins fgjob.h already uses for its
 * work-stealing counter. */

#include "base.h"

typedef void (*Rc_On_Zero_Fn)(void *ctx);

typedef struct {
    i32 volatile count;
    Rc_On_Zero_Fn on_zero; /* may be NULL - a release that hits zero with no callback just does nothing further */
    void *ctx;
} Ref_Count;

/* count starts at 1 - the creator's own reference. Nobody has to
 * rc_retain() immediately after creating one, same as nobody has to
 * malloc() twice to "own" what they just allocated. */
void rc_init(Ref_Count *rc, Rc_On_Zero_Fn on_zero, void *ctx);

void rc_retain(Ref_Count *rc);

/* returns 1 if this specific call brought the count to zero (and ran
 * on_zero), 0 if other references are still live. Releasing more times
 * than retained is a caller bug - REF_COUNT_ASSERT catches it in debug
 * builds the same way ARENA_ASSERT catches arena misuse, silently
 * continuing on a negative count in a release build is not a recovery
 * worth pretending to offer. */
i32 rc_release(Ref_Count *rc);

i32 rc_count(const Ref_Count *rc);

#endif /* HOLYC_REF_COUNT_H_ */

#ifdef REF_COUNT_IMPLEMENTATION
#ifndef REF_COUNT_IMPLEMENTATION_
#define REF_COUNT_IMPLEMENTATION_

#ifndef REF_COUNT_ASSERT
#define REF_COUNT_ASSERT HOLYC_ASSERT
#endif

/* on_zero==NULL is stored as this no-op instead - rc_release used to
 * null-check on_zero on every single call, even though the pointer never
 * changes after rc_init. Paying a one-time branch here (init is rare)
 * instead of a per-release one (potentially the hottest path this struct
 * has) is a real trade, not just moving the check around for its own
 * sake. */
static void rc_noop_(void *ctx) { (void)ctx; }

void rc_init(Ref_Count *rc, Rc_On_Zero_Fn on_zero, void *ctx)
{
    rc->count = 1;
    rc->on_zero = on_zero ? on_zero : rc_noop_;
    rc->ctx = ctx;
}

void rc_retain(Ref_Count *rc)
{
    __sync_fetch_and_add(&rc->count, 1);
}

i32 rc_release(Ref_Count *rc)
{
    i32 new_count = __sync_sub_and_fetch(&rc->count, 1);
    i32 hit_zero = (new_count == 0);
    REF_COUNT_ASSERT(new_count >= 0 && "rc_release: released more times than retained");
    if (hit_zero) rc->on_zero(rc->ctx);
    return hit_zero;
}

i32 rc_count(const Ref_Count *rc)
{
    return rc->count;
}

#endif /* REF_COUNT_IMPLEMENTATION_ */
#endif /* REF_COUNT_IMPLEMENTATION */
