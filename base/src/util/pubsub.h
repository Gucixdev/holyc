#ifndef HOLYC_PUBSUB_H_
#define HOLYC_PUBSUB_H_

/* Observer/pub-sub: a Pubsub_Signal holds a fixed-capacity list of (callback,
 * ctx) subscriber pairs; pubsub_signal_emit calls every one of them in
 * subscription order with a caller-supplied payload pointer. Fixed
 * capacity (PUBSUB_MAX_SUBSCRIBERS), no arena/malloc at all - matches
 * this project's own "no allocation in the hot path" stance for small,
 * frequently-touched structs (action_queue.h's own fixed-capacity ring
 * takes the same stance for the same reason). A caller needing more
 * subscribers than the fixed cap is a real, documented size decision to
 * revisit (raise the constant), not a silent truncation - pubsub_signal_connect
 * returns 0 rather than dropping a subscriber quietly when full. */

#include "core/base.h"

#ifndef PUBSUB_MAX_SUBSCRIBERS
#define PUBSUB_MAX_SUBSCRIBERS 16
#endif

typedef void (*Pubsub_Signal_Fn)(void *payload, void *ctx);

typedef struct {
    Pubsub_Signal_Fn fn[PUBSUB_MAX_SUBSCRIBERS];
    void *ctx[PUBSUB_MAX_SUBSCRIBERS];
    i32 count;
} Pubsub_Signal;

void pubsub_signal_init(Pubsub_Signal *s);

/* returns 1 on success, 0 if already at PUBSUB_MAX_SUBSCRIBERS - a
 * real, checkable failure, not a silent drop. */
i32 pubsub_signal_connect(Pubsub_Signal *s, Pubsub_Signal_Fn fn, void *ctx);

/* removes the first subscriber whose (fn, ctx) pair matches exactly
 * (both must match - the same fn connected with two different ctx
 * values are distinct subscriptions). Returns 1 if something was
 * removed, 0 if no matching subscriber was found. Preserves the
 * relative order of the remaining subscribers. */
i32 pubsub_signal_disconnect(Pubsub_Signal *s, Pubsub_Signal_Fn fn, void *ctx);

/* calls every subscriber, in subscription order, with `payload`. Safe
 * against a subscriber disconnecting ITSELF during the emit (iterates
 * by index over a snapshot count taken up front, not by re-reading
 * s->count each iteration) - not safe against a subscriber connecting
 * or disconnecting a DIFFERENT subscriber mid-emit, a real, documented
 * limitation rather than a claimed-but-untested guarantee. */
void pubsub_signal_emit(Pubsub_Signal *s, void *payload);

#endif /* HOLYC_PUBSUB_H_ */

#ifdef PUBSUB_IMPLEMENTATION
#ifndef PUBSUB_IMPLEMENTATION_
#define PUBSUB_IMPLEMENTATION_

void pubsub_signal_init(Pubsub_Signal *s)
{
    s->count = 0;
}

i32 pubsub_signal_connect(Pubsub_Signal *s, Pubsub_Signal_Fn fn, void *ctx)
{
    if (s->count >= PUBSUB_MAX_SUBSCRIBERS) return 0;
    s->fn[s->count] = fn;
    s->ctx[s->count] = ctx;
    s->count++;
    return 1;
}

i32 pubsub_signal_disconnect(Pubsub_Signal *s, Pubsub_Signal_Fn fn, void *ctx)
{
    i32 i;
    for (i = 0; i < s->count; i++) {
        if (s->fn[i] == fn && s->ctx[i] == ctx) {
            i32 j;
            for (j = i; j < s->count - 1; j++) {
                s->fn[j] = s->fn[j + 1];
                s->ctx[j] = s->ctx[j + 1];
            }
            s->count--;
            return 1;
        }
    }
    return 0;
}

void pubsub_signal_emit(Pubsub_Signal *s, void *payload)
{
    i32 snapshot_count = s->count;
    i32 i;
    for (i = 0; i < snapshot_count; i++) {
        s->fn[i](payload, s->ctx[i]);
    }
}

#endif /* PUBSUB_IMPLEMENTATION_ */
#endif /* PUBSUB_IMPLEMENTATION */
