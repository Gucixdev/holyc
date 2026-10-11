#ifndef HOLYC_RING_BUFFER_H_
#define HOLYC_RING_BUFFER_H_

/* Ported from c89atomic's extras/c89atomic_ring_buffer.h (David Reid,
 * public domain / MIT-no-attribution dual license) - flagged as a
 * candidate in an earlier review pass and finally ported here. Lock-free
 * single-producer/single-consumer ring buffer, safe cross-thread ONLY
 * under that constraint (multiple producers or consumers need a
 * different structure or their own mutual exclusion). event_queue.h is
 * deliberately single-owner, not cross-thread, so this fills a genuinely
 * different niche - e.g. fgjob workers streaming results back to a
 * collector thread without a lock.
 *
 * Algorithm unchanged from the original (head/tail cursors with a
 * most-significant "loop flag" bit to disambiguate empty vs full when
 * they land on the same slot, cache-line padding between head and tail so
 * the producer's writes to head don't bounce the consumer's cache line
 * for tail and vice versa, map/unmap producer-consumer API so the caller
 * gets a contiguous pointer even across the wrap point). What changed is
 * only the vocabulary: this project uses GCC's `__sync_*` builtins and
 * `volatile` fields (see fgjob.h), not C11-style atomics with per-call
 * explicit memory order - `ring_buffer_barrier_()` below is a full
 * `__sync_synchronize()` fence used at both the acquire and release
 * points the original distinguished. A full barrier is a strictly
 * stronger substitute for either half alone, so this stays correct, just
 * without the (here, unavailable) finer-grained vocabulary to say which
 * half was meant - same tradeoff fgjob.h itself already makes. */

#include "types/base.h"

/* own local memcpy_ - own copy per this project's standing "no cross-
 * IMPLEMENTATION-guard dependency" rule, avoiding a <string.h>
 * dependency for the wrap-point bulk copy. Plain byte loop, no special
 * atomicity concerns here - the actual cross-thread synchronization is
 * ring_buffer_barrier_()'s job, called separately around this, same as
 * it would be around a libc memcpy call. */
static void *ring_buffer_memcpy_(void *dst, const void *src, size_t n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    size_t i;
    for (i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

#ifndef RING_BUFFER_ASSERT
#define RING_BUFFER_ASSERT HOLYC_ASSERT
#endif

#ifndef RING_BUFFER_CACHE_LINE_SIZE
#define RING_BUFFER_CACHE_LINE_SIZE 64
#endif

#define RING_BUFFER_FLAG_MIRRORED (1u << 0) /* buffer is mirrored at capacity*stride, see ring_buffer_init */

typedef struct {
    u32 volatile head;     /* MSB = loop flag. head == tail (incl. flag) means empty. */
    u8 pad0_[RING_BUFFER_CACHE_LINE_SIZE > 4 ? RING_BUFFER_CACHE_LINE_SIZE - 4 : 1];
    u32 volatile tail;     /* MSB = loop flag */
    u8 pad1_[RING_BUFFER_CACHE_LINE_SIZE > 4 ? RING_BUFFER_CACHE_LINE_SIZE - 4 : 1];
    u32 capacity;           /* elements */
    u32 stride;             /* bytes/element */
    u32 flags;
    void *buffer;            /* caller-owned, must be 2 * capacity * stride bytes (or mirrored-mapped, see RING_BUFFER_FLAG_MIRRORED) */
} Ring_Buffer;

/* buffer must be caller-allocated, 2*capacity*stride bytes (the second
 * half is either a plain overflow scratch area the map calls copy into/
 * out of at the wrap point, or - if RING_BUFFER_FLAG_MIRRORED is set - a
 * second virtual mapping of the SAME physical memory as the first half,
 * which the caller must set up itself via mmap/MapViewOfFile2; this
 * header doesn't do that mapping for you, it only honors the flag).
 * capacity must be <= 0x7FFFFFFF (the top bit is the loop flag) and
 * capacity*stride must fit in 32 bits (kept a hard 32-bit limit rather
 * than size_t so behavior doesn't differ between 32/64-bit builds). */
void ring_buffer_init(u32 capacity, u32 stride, u32 flags, void *buffer, Ring_Buffer *rb);

/* producer side: map up to `count` elements for writing, returns how many
 * were actually mapped (less than requested means the buffer doesn't have
 * that much room) into *out_ptr. Write your data, then unmap. */
u32 ring_buffer_map_produce(Ring_Buffer *rb, u32 count, void **out_ptr);
void ring_buffer_unmap_produce(Ring_Buffer *rb, u32 count);

/* consumer side: same map/unmap shape. */
u32 ring_buffer_map_consume(Ring_Buffer *rb, u32 count, void **out_ptr);
void ring_buffer_unmap_consume(Ring_Buffer *rb, u32 count);

/* approximate - only well-defined when called from the producer or
 * consumer thread itself, per the original's own documented caveat (a
 * third thread can observe a torn/stale value mid-map). */
u32 ring_buffer_length(const Ring_Buffer *rb);
u32 ring_buffer_capacity(const Ring_Buffer *rb);

#endif /* HOLYC_RING_BUFFER_H_ */

#ifdef RING_BUFFER_IMPLEMENTATION
#ifndef RING_BUFFER_IMPLEMENTATION_
#define RING_BUFFER_IMPLEMENTATION_


static void ring_buffer_barrier_(void)
{
    __sync_synchronize();
}

void ring_buffer_init(u32 capacity, u32 stride, u32 flags, void *buffer, Ring_Buffer *rb)
{
    rb->head = 0;
    rb->tail = 0;
    rb->capacity = 0;
    rb->stride = 0;
    rb->flags = 0;
    rb->buffer = NULL;

    RING_BUFFER_ASSERT(buffer != NULL && stride > 0 && capacity > 0);
    RING_BUFFER_ASSERT(capacity <= 0x7FFFFFFFu);
    RING_BUFFER_ASSERT(capacity <= (0xFFFFFFFFu / stride));

    rb->capacity = capacity;
    rb->stride = stride;
    rb->flags = flags;
    rb->buffer = buffer;
}

static u32 ring_buffer_calc_length_(u32 head, u32 tail, u32 capacity)
{
    u32 loop_flag = ((head & 0x80000000u) ^ (tail & 0x80000000u)) >> 31;
    return ((head & 0x7FFFFFFFu) + capacity * loop_flag) - (tail & 0x7FFFFFFFu);
}

static u32 ring_buffer_calc_remaining_(u32 head, u32 tail, u32 capacity)
{
    return capacity - ring_buffer_calc_length_(head, tail, capacity);
}

u32 ring_buffer_map_produce(Ring_Buffer *rb, u32 count, void **out_ptr)
{
    u32 head, tail, remaining;

    *out_ptr = NULL;

    head = rb->head; /* producer-only field, relaxed is fine */
    ring_buffer_barrier_(); /* acquire, matches the consumer's release on unmap_consume */
    tail = rb->tail;

    remaining = ring_buffer_calc_remaining_(head, tail, rb->capacity);
    if (count > remaining) count = remaining;

    if (count > 0) {
        *out_ptr = (u8 *)rb->buffer + (size_t)(head & 0x7FFFFFFFu) * rb->stride;
    }
    return count;
}

void ring_buffer_unmap_produce(Ring_Buffer *rb, u32 count)
{
    u32 head, tail;

    RING_BUFFER_ASSERT(count <= rb->capacity);

    head = rb->head;
    tail = rb->tail;
    RING_BUFFER_ASSERT(count <= ring_buffer_calc_remaining_(head, tail, rb->capacity));
    (void)tail;

    if ((rb->flags & RING_BUFFER_FLAG_MIRRORED) == 0) {
        u32 new_head = (head & 0x7FFFFFFFu) + count;
        if (new_head > rb->capacity) {
            new_head -= rb->capacity;
            ring_buffer_memcpy_(rb->buffer, (u8 *)rb->buffer + (size_t)rb->capacity * rb->stride, (size_t)new_head * rb->stride);
        }
    }

    head += count;
    if ((head & 0x7FFFFFFFu) >= rb->capacity) {
        head -= rb->capacity;
        head ^= 0x80000000u;
    }

    ring_buffer_barrier_(); /* release: consumer must not see the advanced head before our writes land */
    rb->head = head;
}

u32 ring_buffer_map_consume(Ring_Buffer *rb, u32 count, void **out_ptr)
{
    u32 head, tail, length;

    *out_ptr = NULL;

    ring_buffer_barrier_(); /* acquire, matches the producer's release on unmap_produce */
    head = rb->head;
    tail = rb->tail; /* consumer-only field, relaxed is fine */

    length = ring_buffer_calc_length_(head, tail, rb->capacity);
    if (count > length) count = length;

    if (count > 0) {
        *out_ptr = (u8 *)rb->buffer + (size_t)(tail & 0x7FFFFFFFu) * rb->stride;

        if ((rb->flags & RING_BUFFER_FLAG_MIRRORED) == 0) {
            u32 new_tail = (tail & 0x7FFFFFFFu) + count;
            if (new_tail > rb->capacity) {
                new_tail -= rb->capacity;
                ring_buffer_memcpy_((u8 *)rb->buffer + (size_t)rb->capacity * rb->stride, rb->buffer, (size_t)new_tail * rb->stride);
            }
        }
    }
    return count;
}

void ring_buffer_unmap_consume(Ring_Buffer *rb, u32 count)
{
    u32 head, tail;

    RING_BUFFER_ASSERT(count <= rb->capacity);

    head = rb->head;
    tail = rb->tail;
    RING_BUFFER_ASSERT(count <= ring_buffer_calc_length_(head, tail, rb->capacity));
    (void)head;

    tail += count;
    if ((tail & 0x7FFFFFFFu) >= rb->capacity) {
        tail -= rb->capacity;
        tail ^= 0x80000000u;
    }

    ring_buffer_barrier_(); /* release: producer must not overwrite data before we've finished reading it */
    rb->tail = tail;
}

u32 ring_buffer_length(const Ring_Buffer *rb)
{
    u32 head = rb->head;
    u32 tail = rb->tail;
    u32 length = ring_buffer_calc_length_(head, tail, rb->capacity);
    if (length > rb->capacity) length = rb->capacity; /* clamp: caller-misuse guard, see header doc */
    return length;
}

u32 ring_buffer_capacity(const Ring_Buffer *rb)
{
    return rb->capacity;
}

#endif /* RING_BUFFER_IMPLEMENTATION_ */
#endif /* RING_BUFFER_IMPLEMENTATION */
