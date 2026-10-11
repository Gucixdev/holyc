#ifndef HOLYC_PIPE_H_
#define HOLYC_PIPE_H_

/* Pipe: the producer/consumer close-lifecycle protocol on top of
 * stream.h's Stream. A Stream by itself only knows "has bytes or
 * doesn't" - Pipe adds "the producer is DONE, no more bytes are ever
 * coming", the thing a consumer needs to know to tell "temporarily
 * empty, more coming" apart from "empty and staying that way, stop
 * reading". Single-producer/single-consumer, not thread-safe (that is
 * ring_buffer.h's job, per this project's own thread/ vs dataflow/
 * split - see stream.h's own header comment). */

#include "core/base.h"
#include "core/arena_alloc.h"
#define STREAM_IMPLEMENTATION
#include "core/stream.h"

typedef struct {
    Stream stream;
    i32 closed;
} Pipe;

void pipe_init(Pipe *p, size_t chunk_size);

/* writing after pipe_close() is a caller bug (the whole point of
 * closing is "no more writes are coming") - asserts rather than
 * silently accepting or silently dropping the data. */
void pipe_write(Arena *a, Pipe *p, const void *src, size_t n);

size_t pipe_read(Pipe *p, void *dst, size_t n);

void pipe_close(Pipe *p);      /* producer signals it is done writing */
i32 pipe_is_closed(const Pipe *p);

/* closed AND every already-written byte has been read - the real
 * "nothing left, ever" signal a consumer's read loop should check for,
 * since pipe_is_closed() alone can still have buffered bytes left. */
i32 pipe_is_drained(const Pipe *p);

#endif /* HOLYC_PIPE_H_ */

#ifdef PIPE_IMPLEMENTATION
#ifndef PIPE_IMPLEMENTATION_
#define PIPE_IMPLEMENTATION_

void pipe_init(Pipe *p, size_t chunk_size)
{
    stream_init(&p->stream, chunk_size);
    p->closed = 0;
}

void pipe_write(Arena *a, Pipe *p, const void *src, size_t n)
{
    HOLYC_ASSERT(!p->closed);
    stream_write(a, &p->stream, src, n);
}

size_t pipe_read(Pipe *p, void *dst, size_t n)
{
    return stream_read(&p->stream, dst, n);
}

void pipe_close(Pipe *p) { p->closed = 1; }
i32 pipe_is_closed(const Pipe *p) { return p->closed; }
i32 pipe_is_drained(const Pipe *p) { return p->closed && stream_is_empty(&p->stream); }

#endif /* PIPE_IMPLEMENTATION_ */
#endif /* PIPE_IMPLEMENTATION */
