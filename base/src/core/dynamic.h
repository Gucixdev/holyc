#ifndef HOLYC_DYNAMIC_FLOW_H_
#define HOLYC_DYNAMIC_FLOW_H_

/* Dynamic data flow: the growable counterpart to static.h (same
 * directory) - a thin, explicitly-named wrapper over stream.h's own
 * chunk-chain growth, so "does this flow need to grow at runtime"
 * has its own clear name in this directory rather than every caller
 * needing to know stream.h's own chunk_size parameter is what makes
 * it dynamic. Every write succeeds (allocating more chunks as
 * needed), unlike static_flow_write's own all-or-nothing contract. */

#include "core/base.h"
#include "core/arena_alloc.h"
#define STREAM_IMPLEMENTATION
#include "core/stream.h"

typedef struct {
    Stream stream;
} Dynamic_Flow;

void dynamic_flow_init(Dynamic_Flow *f, size_t chunk_size);
void dynamic_flow_write(Arena *a, Dynamic_Flow *f, const void *data, size_t n); /* always succeeds */
size_t dynamic_flow_read(Dynamic_Flow *f, void *dst, size_t n);
i32 dynamic_flow_is_empty(const Dynamic_Flow *f);

#endif /* HOLYC_DYNAMIC_FLOW_H_ */

#ifdef DYNAMIC_FLOW_IMPLEMENTATION
#ifndef DYNAMIC_FLOW_IMPLEMENTATION_
#define DYNAMIC_FLOW_IMPLEMENTATION_

void dynamic_flow_init(Dynamic_Flow *f, size_t chunk_size)
{
    stream_init(&f->stream, chunk_size);
}

void dynamic_flow_write(Arena *a, Dynamic_Flow *f, const void *data, size_t n)
{
    stream_write(a, &f->stream, data, n);
}

size_t dynamic_flow_read(Dynamic_Flow *f, void *dst, size_t n)
{
    return stream_read(&f->stream, dst, n);
}

i32 dynamic_flow_is_empty(const Dynamic_Flow *f)
{
    return stream_is_empty(&f->stream);
}

#endif /* DYNAMIC_FLOW_IMPLEMENTATION_ */
#endif /* DYNAMIC_FLOW_IMPLEMENTATION */
