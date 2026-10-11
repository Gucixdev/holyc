#ifndef HOLYC_INPUT_BUFFER_H_
#define HOLYC_INPUT_BUFFER_H_

/* Fixed-size ring of recent input events, for combo/sequence detection
 * (fighting-game-style "was quarter-circle-forward pressed recently?").
 *
 * Deliberately NOT built on `src/job/ring_buffer.h` - checked its API
 * first, and it's a genuinely different niche: a lock-free cross-thread
 * SPSC structure requiring a caller-managed 2x-sized (or virtually
 * mirrored) backing buffer, built for producer/consumer streaming
 * between threads. This is single-threaded, single-owner, fixed
 * capacity, overwrite-oldest-on-full - a much simpler shape, and
 * forcing it through ring_buffer.h's cross-thread machinery would add
 * real complexity (the mirrored-buffer setup alone) for no benefit
 * here. Caller supplies the backing `Input_Event[]` array (arena,
 * static, or stack - this file doesn't allocate).
 *
 * `input_buffer_has_sequence` checks for a CONTIGUOUS run of matching
 * event types in order (not "these types appeared somewhere, in this
 * relative order, possibly with other events between them") - the
 * simpler, more predictable semantic, and the one real fighting-game
 * input parsers actually want for exact-motion detection. */

#include "base.h"

typedef struct {
    i32 type;      /* caller-defined event type/button id */
    i32 timestamp; /* caller-defined - frame number, ms, whatever */
} Input_Event;

typedef struct {
    Input_Event *slots; /* caller-owned, `capacity` elements */
    size_t capacity;
    size_t count;   /* number of valid events currently stored, <= capacity */
    size_t head;    /* index one past the most recently pushed event */
} Input_Buffer;

void input_buffer_init(Input_Buffer *buf, Input_Event *slots, size_t capacity);
void input_buffer_push(Input_Buffer *buf, i32 type, i32 timestamp);
size_t input_buffer_length(const Input_Buffer *buf);

/* returns 1 if the last `n` pushed events (oldest to newest) have
 * types EXACTLY matching seq[0..n-1] in order, 0 otherwise (including
 * when fewer than n events have ever been pushed). */
i32 input_buffer_has_sequence(const Input_Buffer *buf, const i32 *seq, size_t n);

#endif /* HOLYC_INPUT_BUFFER_H_ */

#ifdef INPUT_BUFFER_IMPLEMENTATION
#ifndef INPUT_BUFFER_IMPLEMENTATION_
#define INPUT_BUFFER_IMPLEMENTATION_

void input_buffer_init(Input_Buffer *buf, Input_Event *slots, size_t capacity)
{
    buf->slots = slots;
    buf->capacity = capacity;
    buf->count = 0;
    buf->head = 0;
}

void input_buffer_push(Input_Buffer *buf, i32 type, i32 timestamp)
{
    buf->slots[buf->head].type = type;
    buf->slots[buf->head].timestamp = timestamp;
    buf->head = (buf->head + 1) % buf->capacity;
    if (buf->count < buf->capacity) buf->count++;
}

size_t input_buffer_length(const Input_Buffer *buf)
{
    return buf->count;
}

i32 input_buffer_has_sequence(const Input_Buffer *buf, const i32 *seq, size_t n)
{
    size_t i;
    size_t start;

    if (n == 0 || n > buf->count) return 0;

    /* the n most-recently-pushed events start at (head - n) mod capacity */
    start = (buf->head + buf->capacity - n) % buf->capacity;

    for (i = 0; i < n; i++) {
        size_t idx = (start + i) % buf->capacity;
        if (buf->slots[idx].type != seq[i]) return 0;
    }
    return 1;
}

#endif /* INPUT_BUFFER_IMPLEMENTATION_ */
#endif /* INPUT_BUFFER_IMPLEMENTATION */
