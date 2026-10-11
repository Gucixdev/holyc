#ifndef HOLYC_STATIC_FLOW_H_
#define HOLYC_STATIC_FLOW_H_

/* Static data flow: a fixed-capacity buffer that a data flow can move
 * through - "static" in the sense that its capacity is fixed for its
 * whole lifetime, never grows, and a write that would overflow it
 * fails OUTRIGHT (all-or-nothing) rather than chunk.h's own
 * partial-write-what-fits contract. Paired with dynamic.h (same
 * directory)'s growable equivalent - together they're this
 * directory's answer to "should this particular data flow be allowed
 * to grow at runtime, or must it stay a known, fixed size" (confirmed
 * scope, see src/core/questions.md's own dataflow/static.h entry). */

#include "base.h"

typedef struct {
    u8 *data;
    size_t cap;
    size_t len;
} Static_Flow;

void static_flow_init(Static_Flow *f, u8 *buf, size_t cap); /* buf is caller-owned (stack, arena, static array - anything), never freed by this file */

/* writes ALL of `n` bytes, or none at all - returns 0 (leaving the
 * flow completely untouched) if `n` would exceed the remaining
 * capacity, unlike chunk_write's own "write what fits" partial
 * contract. */
i32 static_flow_write(Static_Flow *f, const void *data, size_t n);

size_t static_flow_remaining(const Static_Flow *f);
void static_flow_reset(Static_Flow *f); /* len = 0, same backing buffer reused */

#endif /* HOLYC_STATIC_FLOW_H_ */

#ifdef STATIC_FLOW_IMPLEMENTATION
#ifndef STATIC_FLOW_IMPLEMENTATION_
#define STATIC_FLOW_IMPLEMENTATION_

void static_flow_init(Static_Flow *f, u8 *buf, size_t cap)
{
    f->data = buf;
    f->cap = cap;
    f->len = 0;
}

i32 static_flow_write(Static_Flow *f, const void *data, size_t n)
{
    const u8 *src = (const u8 *)data;
    size_t i;

    if (f->len + n > f->cap) return 0;

    for (i = 0; i < n; i++) f->data[f->len + i] = src[i];
    f->len += n;
    return 1;
}

size_t static_flow_remaining(const Static_Flow *f) { return f->cap - f->len; }
void static_flow_reset(Static_Flow *f) { f->len = 0; }

#endif /* STATIC_FLOW_IMPLEMENTATION_ */
#endif /* STATIC_FLOW_IMPLEMENTATION */
