#ifndef HOLYC_FABRIC_H_
#define HOLYC_FABRIC_H_

/* Factory/dispatch pattern, built on the same X-macro LIST_MACRO shape
 * xmacro.h uses (not a hard dependency - FABRIC_DEFINE below stands on
 * its own, doesn't include xmacro.h, since it defines its own expander
 * rather than reusing XM_ENUM/XM_STRINGS). Generates a single switch-
 * based dispatcher from a list of (id, constructor) pairs instead of a
 * hand-written if/else-if chain picking a constructor by tag - exactly
 * the kind of dispatch chain this project's branchless/if-minimization
 * sweep already converts to switch by hand at every call site; this
 * generates it once instead.
 *
 * Every constructor function in the list must share the signature
 * `ReturnType *fn(void *ctx)` - the macro can't enforce that at the
 * call site (it's just text substitution), a mismatched signature is
 * a normal compiler error at the generated switch, not a silent bug.
 *
 * Usage:
 *   #define SHAPE_FACTORY_LIST(X) \
 *       X(SHAPE_CIRCLE, make_circle) \
 *       X(SHAPE_SQUARE, make_square)
 *
 *   FABRIC_DEFINE(Shape, shape_factory, SHAPE_FACTORY_LIST)
 *
 * generates `static Shape *shape_factory_create(i32 id, void *ctx)`,
 * dispatching via switch to whichever make_* function matches `id`,
 * returning NULL for an unrecognized id - a real, deliberate guard
 * (rejecting a malformed/unknown tag), not the kind of value-select
 * conditional the branchless directive targets. */

#include "base.h"

#define FABRIC_EXPAND_CASE_(id, fn) case id: return fn(ctx);

#define FABRIC_DEFINE(ReturnType, prefix, LIST_MACRO)     \
static ReturnType *prefix##_create(i32 id, void *ctx)      \
{                                                            \
    switch (id) {                                            \
        LIST_MACRO(FABRIC_EXPAND_CASE_)                        \
        default: return (ReturnType *)0;                        \
    }                                                              \
}

#endif /* HOLYC_FABRIC_H_ */
