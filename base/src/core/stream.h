#ifndef HOLYC_STREAM_H_
#define HOLYC_STREAM_H_

/* Stream: an unbounded byte sequence built from a linked chain of
 * bounded chunk.h Chunks - the "grows as needed" counterpart to a
 * single fixed-size Chunk, same relationship a rope has to a single
 * buffer. Writes append to the tail chunk, allocating a fresh one from
 * the Arena once it fills; reads consume from the head chunk, moving
 * on to the next once it's exhausted. Dropped head chunks are simply
 * never touched again - consistent with this project's Arena model
 * (bump-allocator, no per-allocation free; see arena_alloc.h), not a
 * leak in the sense this project cares about.
 *
 * pipe.h (same directory) is a thin producer/consumer close-lifecycle
 * wrapper BUILT ON TOP of this - this file itself has no notion of
 * "closed", only of "currently has bytes or doesn't". */

#include "core/base.h"
#include "core/arena_alloc.h"
#define CHUNK_IMPLEMENTATION
#include "core/chunk.h"

typedef struct Stream_Node Stream_Node;
struct Stream_Node {
    Chunk chunk;
    Stream_Node *next;
};

typedef struct {
    Stream_Node *head; /* oldest chunk with unread data, or NULL if empty */
    Stream_Node *tail;   /* newest chunk, where writes land */
    size_t chunk_size;    /* capacity given to each newly allocated Chunk */
} Stream;

void stream_init(Stream *st, size_t chunk_size);

/* always writes the full n bytes, allocating as many new chunks as
 * needed - unlike chunk_write, this never partially fails. */
void stream_write(Arena *a, Stream *st, const void *src, size_t n);

/* reads up to n bytes, advancing across chunk boundaries transparently.
 * Returns the number of bytes actually read - less than n once the
 * stream runs out of buffered data. */
size_t stream_read(Stream *st, void *dst, size_t n);

i32 stream_is_empty(const Stream *st);

#endif /* HOLYC_STREAM_H_ */

#ifdef STREAM_IMPLEMENTATION
#ifndef STREAM_IMPLEMENTATION_
#define STREAM_IMPLEMENTATION_

void stream_init(Stream *st, size_t chunk_size)
{
    st->head = 0;
    st->tail = 0;
    st->chunk_size = chunk_size;
}

static Stream_Node *stream_new_node_(Arena *a, size_t chunk_size)
{
    Stream_Node *node = (Stream_Node *)arena_alloc(a, sizeof(Stream_Node));
    node->chunk = chunk_make(a, chunk_size);
    node->next = 0;
    return node;
}

void stream_write(Arena *a, Stream *st, const void *src, size_t n)
{
    const u8 *bytes = (const u8 *)src;
    size_t written = 0;

    if (!st->tail) {
        st->head = st->tail = stream_new_node_(a, st->chunk_size);
    }

    while (written < n) {
        size_t got = chunk_write(&st->tail->chunk, bytes + written, n - written);
        written += got;
        if (written < n) {
            Stream_Node *node = stream_new_node_(a, st->chunk_size);
            st->tail->next = node;
            st->tail = node;
        }
    }
}

size_t stream_read(Stream *st, void *dst, size_t n)
{
    u8 *out = (u8 *)dst;
    size_t total = 0;

    while (total < n && st->head) {
        size_t got = chunk_read(&st->head->chunk, out + total, n - total);
        total += got;
        if (chunk_exhausted(&st->head->chunk)) {
            if (st->head->next) {
                st->head = st->head->next;
            } else {
                /* head is both exhausted and the last node - stream is
                 * fully drained; leave head/tail pointing at it so
                 * future writes can still append right where they are */
                break;
            }
        }
    }
    return total;
}

i32 stream_is_empty(const Stream *st)
{
    return st->head == 0 || (chunk_exhausted(&st->head->chunk) && st->head->next == 0);
}

#endif /* STREAM_IMPLEMENTATION_ */
#endif /* STREAM_IMPLEMENTATION */
