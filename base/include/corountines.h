#ifndef HOLYC_CORROUTINES_H_
#define HOLYC_CORROUTINES_H_

/* Stackless coroutines via protothreads (Adam Dunkels, public domain
 * technique - http://dunkels.com/adam/pt/, this is a from-scratch
 * reimplementation of the same trick, not a copy of his code). Chosen over
 * a stackful setjmp/longjmp-plus-own-stack design because it maps directly onto a
 * switch/case used as a jump table: a coroutine here literally IS a
 * switch statement resuming at a saved case label. Not OOP, not RAII, not
 * virtual dispatch - the things the design rules actually reject have nothing
 * to do with this. Zero heap allocation, zero separate stack, one
 * `unsigned short`'s worth of state per coroutine.
 *
 * The trick: __LINE__ is a distinct integer at every PT_WAIT_UNTIL/
 * PT_YIELD call site within one function, so each one becomes a distinct
 * `case` label. Resuming a coroutine re-enters the function and
 * `switch (pt->lc)` jumps straight back to wherever it last returned from,
 * skipping everything before that line - the entire "state machine" is
 * the coroutine function's own C control flow, nothing hand-encoded.
 *
 * Known limitation, inherent to the technique (not a corner we cut): a
 * PT_WAIT_UNTIL/PT_YIELD inside a nested block (a for-loop, an if) works,
 * but local variables do NOT survive a yield across separate calls to the
 * coroutine function - the stack frame is torn down and rebuilt each call,
 * only `pt->lc` persists. State that needs to survive a yield belongs in
 * the caller's own struct, passed in like any other argument. If that
 * limitation ever actually bites, that is the point to reach for a
 * stackful design (setjmp/longjmp over a stack sliced from an Arena/Pool)
 * as a second, heavier option - not a reason to build it preemptively now.
 *
 * Usage:
 *     typedef struct { Pt pt; i32 n; } Counter;
 *
 *     i32 counter_run(Counter *c)
 *     {
 *         PT_BEGIN(&c->pt);
 *         for (c->n = 0; c->n < 3; c->n++) {
 *             PT_YIELD(&c->pt);
 *         }
 *         PT_END(&c->pt);
 *     }
 *
 *     Counter c; pt_init(&c.pt);
 *     while (counter_run(&c) == PT_YIELDED) { ... }
 */

#include "base.h"

typedef struct {
    u16 lc; /* "local continuation" - the resume point, 0 = not started */
} Pt;

#define PT_WAITING 0
#define PT_YIELDED 1
#define PT_EXITED  2
#define PT_ENDED   3

#define pt_init(pt_) ((pt_)->lc = 0)

/* every PT_* macro below expects a `Pt *` expression named exactly as
 * passed to PT_BEGIN, referred to again in PT_END - conventionally the
 * same `pt` argument throughout one function, matching the usage example
 * above. */

#define PT_BEGIN(pt_) \
    switch ((pt_)->lc) { \
    case 0:

#define PT_END(pt_) \
    } \
    pt_init(pt_); \
    return PT_ENDED;

/* blocks (returns PT_WAITING to the caller, to be called again later)
 * until `cond` holds, without losing its place */
#define PT_WAIT_UNTIL(pt_, cond) \
    do { \
        (pt_)->lc = (u16)__LINE__; \
        /* fallthrough */ \
    case __LINE__: \
        if (!(cond)) return PT_WAITING; \
    } while (0)

#define PT_WAIT_WHILE(pt_, cond) PT_WAIT_UNTIL(pt_, !(cond))

/* returns PT_YIELDED to the caller now; next call resumes right after
 * this line */
#define PT_YIELD(pt_) \
    do { \
        (pt_)->lc = (u16)__LINE__; \
        return PT_YIELDED; \
    case __LINE__: \
        ; \
    } while (0)

/* ends the coroutine immediately (as opposed to running off the end via
 * PT_END) - also resets it, same as PT_END does, so re-running from
 * pt_init state needs an explicit pt_init() call by the caller, not an
 * implicit "just call it again" */
#define PT_EXIT(pt_) \
    do { \
        pt_init(pt_); \
        return PT_EXITED; \
    } while (0)

#endif /* HOLYC_CORROUTINES_H_ */
