#ifndef HOLYC_PRIORITY_QUEUE_H_
#define HOLYC_PRIORITY_QUEUE_H_

/* Binary heap priority queue - complements fine_grained.h's Thread_Pool/
 * Task_Graph (flat batch / DAG-level dispatch, no notion of "some jobs
 * matter more than others") for the case where jobs genuinely need to
 * run in priority order, not just batch/level order.
 *
 * Priority convention (explicit, since "bigger number = more important"
 * and its opposite are both common and NOT interchangeable by accident):
 *   1          = highest priority, popped first
 *   2, 3, ...  = each larger number is progressively LOWER priority,
 *                popped later
 *   0          = NO priority at all - always popped dead last, after
 *                every priority>=1 item, regardless of how large those
 *                other numbers get. Meant for "run this only when
 *                nothing else needs to", not "lowest of the numbered
 *                priorities".
 * Negative priorities are treated the same as 0 (no priority) rather
 * than rejected - a caller computing a priority from a subtraction that
 * could go negative gets the safe "runs last" behavior instead of
 * needing its own clamp first.
 *
 * NOT thread-safe on its own - arena-owned growable array, single-owner
 * like event_queue.h, not cross-thread like ring_buffer.h/
 * action_queue.h. A caller wanting cross-thread priority scheduling
 * wraps this with its own mutex (or reaches for action_queue.h instead
 * if plain FIFO ordering is actually enough) rather than this file
 * baking in locking every caller might not need. */

#include "base.h"
#include "arena_alloc.h"

typedef struct {
    i64 id;
    i64 priority; /* the RAW value as pushed - 0 stored as 0, not remapped; see priority_queue_key_ for the actual ordering rule */
} Priority_Queue_Item;

typedef struct {
    Priority_Queue_Item *items;
    size_t count;
    size_t capacity;
} Priority_Queue;

void priority_queue_push(Arena *a, Priority_Queue *q, i64 id, i64 priority);
i32  priority_queue_pop(Priority_Queue *q, Priority_Queue_Item *out); /* 1 if popped (highest priority first, see file comment), 0 if empty */
i32  priority_queue_peek(const Priority_Queue *q, Priority_Queue_Item *out); /* 1 if any, without removing */
void priority_queue_reset(Priority_Queue *q);

#endif /* HOLYC_PRIORITY_QUEUE_H_ */

#ifdef PRIORITY_QUEUE_IMPLEMENTATION
#ifndef PRIORITY_QUEUE_IMPLEMENTATION_
#define PRIORITY_QUEUE_IMPLEMENTATION_

static void priority_queue_swap_(Priority_Queue_Item *a, Priority_Queue_Item *b)
{
    Priority_Queue_Item t = *a;
    *a = *b;
    *b = t;
}

/* the actual ordering key: priority<=0 maps to the largest possible key
 * (sorts dead last, always), priority>=1 maps to itself (smaller number
 * = smaller key = pops first, out of the priority>=1 items) - min-heap
 * over this key gives exactly the "1 highest, larger number lower, 0/
 * negative never until everything else is drained" rule the file
 * comment documents. */
static u64 priority_queue_key_(i64 priority)
{
    if (priority <= 0) return (u64)-1;
    return (u64)priority;
}

static void priority_queue_sift_up_(Priority_Queue *q, size_t i)
{
    while (i > 0) {
        size_t parent = (i - 1) / 2;
        if (priority_queue_key_(q->items[parent].priority) <= priority_queue_key_(q->items[i].priority)) break;
        priority_queue_swap_(&q->items[parent], &q->items[i]);
        i = parent;
    }
}

static void priority_queue_sift_down_(Priority_Queue *q, size_t i)
{
    for (;;) {
        size_t left = 2 * i + 1;
        size_t right = 2 * i + 2;
        size_t smallest = i;

        if (left < q->count && priority_queue_key_(q->items[left].priority) < priority_queue_key_(q->items[smallest].priority)) smallest = left;
        if (right < q->count && priority_queue_key_(q->items[right].priority) < priority_queue_key_(q->items[smallest].priority)) smallest = right;
        if (smallest == i) break;

        priority_queue_swap_(&q->items[i], &q->items[smallest]);
        i = smallest;
    }
}

void priority_queue_push(Arena *a, Priority_Queue *q, i64 id, i64 priority)
{
    Priority_Queue_Item item;
    item.id = id;
    item.priority = priority;
    arena_da_append(a, q, item);
    priority_queue_sift_up_(q, q->count - 1);
}

i32 priority_queue_pop(Priority_Queue *q, Priority_Queue_Item *out)
{
    if (q->count == 0) return 0;
    *out = q->items[0];
    q->count--;
    if (q->count > 0) {
        q->items[0] = q->items[q->count];
        priority_queue_sift_down_(q, 0);
    }
    return 1;
}

i32 priority_queue_peek(const Priority_Queue *q, Priority_Queue_Item *out)
{
    if (q->count == 0) return 0;
    *out = q->items[0];
    return 1;
}

void priority_queue_reset(Priority_Queue *q)
{
    q->count = 0;
}

#endif /* PRIORITY_QUEUE_IMPLEMENTATION_ */
#endif /* PRIORITY_QUEUE_IMPLEMENTATION */
