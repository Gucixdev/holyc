#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#define PRIORITY_QUEUE_IMPLEMENTATION
#include "core/arena_alloc.h"
#include "core/priority_queue.h"

int main(void)
{
    Arena a = {0};
    Priority_Queue q = {0};
    Priority_Queue_Item out;

    print_cstr("test_priority_queue\n");

    TEST_CHECK(priority_queue_pop(&q, &out) == 0, "pop on empty queue returns 0");
    TEST_CHECK(priority_queue_peek(&q, &out) == 0, "peek on empty queue returns 0");

    /* 1 = highest priority, popped first; larger numbers are lower
     * priority; 0 = no priority, always last regardless of the other
     * values present. */
    priority_queue_push(&a, &q, 100, 5);
    priority_queue_push(&a, &q, 101, 1);
    priority_queue_push(&a, &q, 102, 9);
    priority_queue_push(&a, &q, 103, 3);
    priority_queue_push(&a, &q, 104, 0);  /* no priority - must come out last */
    priority_queue_push(&a, &q, 105, 1);  /* tie with id 101 at priority 1 */

    TEST_CHECK(priority_queue_peek(&q, &out) == 1 && out.priority == 1, "peek returns priority 1 (highest) first, not the largest number");
    TEST_CHECK(q.count == 6, "peek did not change the count");

    {
        i64 order[6];
        i32 n = 0;
        Priority_Queue_Item item;
        while (priority_queue_pop(&q, &item)) {
            order[n++] = item.priority;
        }
        TEST_CHECK(n == 6, "all 6 pushed items were popped");
        TEST_CHECK(order[0] == 1 && order[1] == 1, "the two priority-1 items come out first (order between ties unspecified but both before anything else)");
        TEST_CHECK(order[2] == 3, "priority 3 comes after both priority-1 items");
        TEST_CHECK(order[3] == 5, "priority 5 comes after priority 3");
        TEST_CHECK(order[4] == 9, "priority 9 (the largest numbered priority) comes after all of 1/1/3/5");
        TEST_CHECK(order[5] == 0, "priority 0 (no priority) comes out dead last, after priority 9 even though 9 > 0 numerically");
    }

    TEST_CHECK(priority_queue_pop(&q, &out) == 0, "queue is empty again after draining");

    /* negative priority is treated the same as 0 - always last, not "highest" via sign */
    priority_queue_push(&a, &q, 200, 1);
    priority_queue_push(&a, &q, 201, -5);
    {
        Priority_Queue_Item item;
        priority_queue_pop(&q, &item);
        TEST_CHECK(item.id == 200, "priority 1 still pops before a negative priority");
        priority_queue_pop(&q, &item);
        TEST_CHECK(item.id == 201, "the negative-priority item pops last, same as priority 0 would");
    }

    /* reset */
    priority_queue_push(&a, &q, 10, 1);
    priority_queue_push(&a, &q, 11, 2);
    priority_queue_reset(&q);
    TEST_CHECK(q.count == 0, "reset clears the queue");
    TEST_CHECK(priority_queue_pop(&q, &out) == 0, "pop after reset returns 0");

    /* larger stress push/pop (priorities 1..200, no zeros) to exercise
     * sift up/down more than the small hand-picked cases above do */
    {
        i32 i;
        i32 ok = 1;
        i64 last = 0;
        Priority_Queue_Item item;
        for (i = 1; i <= 200; i++) {
            priority_queue_push(&a, &q, i, ((i * 2654435761u) % 200) + 1);
        }
        while (priority_queue_pop(&q, &item)) {
            if (item.priority < last) ok = 0;
            last = item.priority;
        }
        TEST_CHECK(ok, "200-item stress: pop order stays non-decreasing (1 first, larger numbers later) throughout");
    }

    arena_free(&a);
    print_flush();
    return test_failures_ != 0;
}
