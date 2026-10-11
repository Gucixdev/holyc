#ifndef HOLYC_EVENT_QUEUE_H_
#define HOLYC_EVENT_QUEUE_H_

/* Deterministic replay by recording events: every Event carries
 * a tick so a recorded stream can be replayed in the exact order it
 * happened. Payload is deliberately just two u64 slots instead of a
 * per-type tagged union - add the real per-type shapes when there is an
 * actual second event type that needs one, not before. */

#include "types/base.h"
#include "alloc/arena_alloc.h"

typedef struct {
    u32 type;
    u64 tick;
    u64 data0;
    u64 data1;
} Event;

typedef struct {
    Event *items;
    size_t count;
    size_t capacity;
    size_t head;
} Event_Queue;

void event_queue_push(Arena *a, Event_Queue *q, u32 type, u64 tick, u64 data0, u64 data1);
i32  event_queue_pop(Event_Queue *q, Event *out); /* returns 1 if popped, 0 if empty */
void event_queue_reset(Event_Queue *q);

#endif /* HOLYC_EVENT_QUEUE_H_ */

#ifdef EVENT_QUEUE_IMPLEMENTATION
#ifndef EVENT_QUEUE_IMPLEMENTATION_
#define EVENT_QUEUE_IMPLEMENTATION_

void event_queue_push(Arena *a, Event_Queue *q, u32 type, u64 tick, u64 data0, u64 data1)
{
    Event e;
    e.type = type;
    e.tick = tick;
    e.data0 = data0;
    e.data1 = data1;
    arena_da_append(a, q, e);
}

i32 event_queue_pop(Event_Queue *q, Event *out)
{
    if (q->head >= q->count) return 0;
    *out = q->items[q->head++];
    return 1;
}

void event_queue_reset(Event_Queue *q)
{
    q->head = 0;
    q->count = 0;
}

#endif /* EVENT_QUEUE_IMPLEMENTATION_ */
#endif /* EVENT_QUEUE_IMPLEMENTATION */
