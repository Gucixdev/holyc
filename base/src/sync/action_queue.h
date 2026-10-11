#ifndef HOLYC_ACTION_QUEUE_H_
#define HOLYC_ACTION_QUEUE_H_

/* Bounded multi-producer/multi-consumer queue of {fn, ctx} actions,
 * guarded by a real mutex+condvar pair - fills the general MPMC niche
 * neither sibling in this directory covers: ring_buffer.h is lock-free
 * but strictly single-producer/single-consumer, event_queue.h is
 * single-owner with no cross-thread safety at all. Blocking push/pop
 * park on a condvar instead of spinning; try_push/try_pop give a non-
 * blocking option for callers that need one (e.g. a worker that should
 * do other work rather than block when the queue is momentarily full/
 * empty).
 *
 * shutdown() wakes every blocked push/pop (broadcast, not signal - ALL
 * parked threads need to notice, not just one) and makes further
 * pushes a no-op; a blocked pop then returns 0 once the queue drains,
 * so a consumer loop can tell "queue closed" from "spuriously empty"
 * without polling a separate flag itself. */

#include "types/base.h"
#include "alloc/arena_alloc.h"
#include <pthread.h>

typedef void (*Action_Fn)(void *ctx);

typedef struct {
    Action_Fn fn;
    void *ctx;
} Action;

typedef struct {
    Action *items;      /* arena-owned, fixed-size ring of `capacity` slots */
    size_t capacity;
    size_t head;         /* index of the next slot to pop */
    size_t count;

    pthread_mutex_t mu;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    i32 shut_down;
} Action_Queue;

void action_queue_init(Arena *a, Action_Queue *q, size_t capacity);
void action_queue_destroy(Action_Queue *q); /* releases the mutex/condvars, not the arena-owned items buffer */

void action_queue_push(Action_Queue *q, Action_Fn fn, void *ctx);      /* blocks while full; no-op once shut down */
i32  action_queue_try_push(Action_Queue *q, Action_Fn fn, void *ctx); /* 1 pushed, 0 if full or shut down */

i32  action_queue_pop(Action_Queue *q, Action *out);      /* blocks while empty; 0 once shut down AND drained */
i32  action_queue_try_pop(Action_Queue *q, Action *out); /* 1 popped, 0 if empty */

void action_queue_shutdown(Action_Queue *q);

#endif /* HOLYC_ACTION_QUEUE_H_ */

#ifdef ACTION_QUEUE_IMPLEMENTATION
#ifndef ACTION_QUEUE_IMPLEMENTATION_
#define ACTION_QUEUE_IMPLEMENTATION_

void action_queue_init(Arena *a, Action_Queue *q, size_t capacity)
{
    q->items = (Action *)arena_alloc(a, capacity * sizeof(Action));
    q->capacity = capacity;
    q->head = 0;
    q->count = 0;
    q->shut_down = 0;
    pthread_mutex_init(&q->mu, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
}

void action_queue_destroy(Action_Queue *q)
{
    pthread_mutex_destroy(&q->mu);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
}

void action_queue_push(Action_Queue *q, Action_Fn fn, void *ctx)
{
    pthread_mutex_lock(&q->mu);
    while (q->count == q->capacity && !q->shut_down) {
        pthread_cond_wait(&q->not_full, &q->mu);
    }
    if (!q->shut_down) {
        size_t tail = (q->head + q->count) % q->capacity;
        q->items[tail].fn = fn;
        q->items[tail].ctx = ctx;
        q->count++;
        pthread_cond_signal(&q->not_empty);
    }
    pthread_mutex_unlock(&q->mu);
}

i32 action_queue_try_push(Action_Queue *q, Action_Fn fn, void *ctx)
{
    i32 ok = 0;
    pthread_mutex_lock(&q->mu);
    if (q->count < q->capacity && !q->shut_down) {
        size_t tail = (q->head + q->count) % q->capacity;
        q->items[tail].fn = fn;
        q->items[tail].ctx = ctx;
        q->count++;
        pthread_cond_signal(&q->not_empty);
        ok = 1;
    }
    pthread_mutex_unlock(&q->mu);
    return ok;
}

i32 action_queue_pop(Action_Queue *q, Action *out)
{
    i32 ok;
    pthread_mutex_lock(&q->mu);
    while (q->count == 0 && !q->shut_down) {
        pthread_cond_wait(&q->not_empty, &q->mu);
    }
    if (q->count > 0) {
        *out = q->items[q->head];
        q->head = (q->head + 1) % q->capacity;
        q->count--;
        pthread_cond_signal(&q->not_full);
        ok = 1;
    } else {
        ok = 0; /* shut down and drained */
    }
    pthread_mutex_unlock(&q->mu);
    return ok;
}

i32 action_queue_try_pop(Action_Queue *q, Action *out)
{
    i32 ok = 0;
    pthread_mutex_lock(&q->mu);
    if (q->count > 0) {
        *out = q->items[q->head];
        q->head = (q->head + 1) % q->capacity;
        q->count--;
        pthread_cond_signal(&q->not_full);
        ok = 1;
    }
    pthread_mutex_unlock(&q->mu);
    return ok;
}

void action_queue_shutdown(Action_Queue *q)
{
    pthread_mutex_lock(&q->mu);
    q->shut_down = 1;
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->mu);
}

#endif /* ACTION_QUEUE_IMPLEMENTATION_ */
#endif /* ACTION_QUEUE_IMPLEMENTATION */
