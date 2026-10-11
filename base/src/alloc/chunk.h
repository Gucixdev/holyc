#ifndef HOLYC_CHUNK_H_
#define HOLYC_CHUNK_H_

/* Chunk: one bounded unit of data in flight through a pipeline - the
 * thing stream.h moves and pipe.h passes between stages. Deliberately
 * NOT thread-safe and NOT lock-free - that is ring_buffer.h's job
 * (src/core/thread/), a genuinely different concern: thread/ is about
 * data moving safely ACROSS threads, dataflow/ is about the SHAPE data
 * takes as it moves, independent of whether threads are involved at
 * all (this split is the user's own, not an inference). A Chunk is
 * meant to be owned by exactly one stage at a time, handed off by
 * plain assignment/pointer-passing, same as any other value here.
 *
 * Backed by a single Arena allocation, so its capacity is fixed for
 * its whole lifetime (no growth - that would need Arena to free/
 * realloc a specific prior allocation, which it deliberately can't).
 * A natural `cap` to pass to chunk_make() is hardscan.h's own
 * recommended_arena_chunk_size, computed from this machine's real
 * cache topology - the two files are meant to be used together
 * (hardscan measures the number, chunk.h is the type that number
 * actually sizes), though chunk_make() takes cap as a plain
 * parameter and has no hardscan.h dependency itself. */

#include "types/base.h"

typedef struct {
    u8 *data;
    size_t cap;   /* total backing capacity in bytes, fixed at chunk_make() */
    size_t len;   /* bytes written so far (0 <= len <= cap) */
    size_t pos;   /* read cursor (0 <= pos <= len) */
} Chunk;

Chunk chunk_make(Arena *a, size_t cap);

/* appends up to n bytes starting at the write end (chunk->len). Returns
 * the number of bytes actually written - less than n once the chunk
 * fills up, 0 once it's completely full. Never overruns cap. */
size_t chunk_write(Chunk *c, const void *src, size_t n);

/* reads up to n bytes starting at the read cursor (chunk->pos),
 * advancing pos by however much was actually read. Returns the number
 * of bytes actually read - less than n (down to 0) once the read
 * cursor catches up to len. Reading never removes/shifts written
 * bytes; chunk_reset() is the only way to reclaim the chunk. */
size_t chunk_read(Chunk *c, void *dst, size_t n);

size_t chunk_remaining_write(const Chunk *c); /* cap - len: room left to write */
size_t chunk_remaining_read(const Chunk *c);   /* len - pos: unread bytes left */

i32 chunk_full(const Chunk *c);      /* len == cap */
i32 chunk_exhausted(const Chunk *c); /* pos == len: nothing left to read, even if not full */

/* rewinds both len and pos to 0, keeping the same backing buffer -
 * the intended way to reuse one Chunk across many trips through a
 * pipeline instead of calling chunk_make() (and burning more Arena
 * memory) every time. */
void chunk_reset(Chunk *c);

#endif /* HOLYC_CHUNK_H_ */

#ifdef CHUNK_IMPLEMENTATION
#ifndef CHUNK_IMPLEMENTATION_
#define CHUNK_IMPLEMENTATION_

/* own local byte-loop copy - project's standing "no cross-
 * IMPLEMENTATION-guard dependency on <string.h>" rule (see str.h,
 * ring_buffer.h's own ring_buffer_memcpy_ for the same pattern). */
static void chunk_memcpy_(void *dst, const void *src, size_t n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    size_t i;
    for (i = 0; i < n; i++) d[i] = s[i];
}

Chunk chunk_make(Arena *a, size_t cap)
{
    Chunk c;
    c.data = (u8 *)arena_alloc(a, cap);
    c.cap = cap;
    c.len = 0;
    c.pos = 0;
    return c;
}

size_t chunk_write(Chunk *c, const void *src, size_t n)
{
    size_t room = c->cap - c->len;
    size_t actual = n < room ? n : room;
    if (actual > 0) {
        chunk_memcpy_(c->data + c->len, src, actual);
        c->len += actual;
    }
    return actual;
}

size_t chunk_read(Chunk *c, void *dst, size_t n)
{
    size_t avail = c->len - c->pos;
    size_t actual = n < avail ? n : avail;
    if (actual > 0) {
        chunk_memcpy_(dst, c->data + c->pos, actual);
        c->pos += actual;
    }
    return actual;
}

size_t chunk_remaining_write(const Chunk *c) { return c->cap - c->len; }
size_t chunk_remaining_read(const Chunk *c) { return c->len - c->pos; }

i32 chunk_full(const Chunk *c) { return c->len == c->cap; }
i32 chunk_exhausted(const Chunk *c) { return c->pos == c->len; }

void chunk_reset(Chunk *c)
{
    c->len = 0;
    c->pos = 0;
}

#endif /* CHUNK_IMPLEMENTATION_ */
#endif /* CHUNK_IMPLEMENTATION */
