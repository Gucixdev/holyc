#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#define ACTION_QUEUE_IMPLEMENTATION
#include "arena_alloc.h"
#include "action_queue.h"

static i32 counter_ = 0;

static void increment_(void *ctx)
{
    i32 *n = (i32 *)ctx;
    (*n)++;
    counter_++;
}

static void *pop_all_thread_(void *arg)
{
    Action_Queue *q = (Action_Queue *)arg;
    Action act;
    i64 popped = 0;

    while (action_queue_pop(q, &act)) {
        act.fn(act.ctx);
        popped++;
    }
    return (void *)(size_t)popped;
}

int main(void)
{
    Arena a = {0};
    Action_Queue q;
    Action act = {0};
    i32 payload = 0;

    print_cstr("test_action_queue\n");

    action_queue_init(&a, &q, 4);

    TEST_CHECK(action_queue_try_pop(&q, &act) == 0, "try_pop on empty queue returns 0");

    TEST_CHECK(action_queue_try_push(&q, increment_, &payload) == 1, "try_push into a non-full queue succeeds");
    TEST_CHECK(action_queue_try_push(&q, increment_, &payload) == 1, "try_push #2 succeeds");
    TEST_CHECK(action_queue_try_push(&q, increment_, &payload) == 1, "try_push #3 succeeds");
    TEST_CHECK(action_queue_try_push(&q, increment_, &payload) == 1, "try_push #4 fills capacity=4");
    TEST_CHECK(action_queue_try_push(&q, increment_, &payload) == 0, "try_push on a full queue fails (does not block)");

    TEST_CHECK(action_queue_try_pop(&q, &act) == 1, "try_pop from a non-empty queue succeeds");
    act.fn(act.ctx);
    TEST_CHECK(payload == 1, "the popped action actually ran (payload incremented)");

    action_queue_try_push(&q, increment_, &payload);
    {
        i32 n = 0;
        while (action_queue_try_pop(&q, &act)) {
            act.fn(act.ctx);
            n++;
        }
        TEST_CHECK(n == 4, "draining the queue pops exactly the remaining 4 actions");
    }
    TEST_CHECK(payload == 5, "all 5 pushed increments ran exactly once each");

    action_queue_destroy(&q);

    /* real cross-thread exercise: producer pushes N actions, a real
     * pthread consumer blocks on action_queue_pop until shutdown()
     * wakes it and the queue drains - the actual reason this file
     * exists (a plain single-threaded try_push/try_pop loop above
     * doesn't touch the mutex/condvar wait paths at all). */
    {
        pthread_t consumer;
        i32 counters[500];
        i32 i;
        void *ret;

        counter_ = 0;
        action_queue_init(&a, &q, 16);
        pthread_create(&consumer, NULL, pop_all_thread_, &q);

        for (i = 0; i < 500; i++) {
            counters[i] = 0;
            action_queue_push(&q, increment_, &counters[i]);
        }
        action_queue_shutdown(&q);

        pthread_join(consumer, &ret);
        TEST_CHECK((i64)(size_t)ret == 500, "consumer thread popped exactly the 500 pushed actions before shutdown drained it");
        TEST_CHECK(counter_ == 500, "every action actually ran exactly once (shared counter matches)");

        {
            i32 all_incremented = 1;
            for (i = 0; i < 500; i++) if (counters[i] != 1) all_incremented = 0;
            TEST_CHECK(all_incremented, "each per-slot counter was incremented exactly once, not double-run or skipped");
        }

        action_queue_destroy(&q);
    }

    arena_free(&a);
    print_flush();
    return test_failures_ != 0;
}
