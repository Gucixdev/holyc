#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#define EVENT_QUEUE_IMPLEMENTATION
#include "core/arena_alloc.h"
#include "core/event_queue.h"

int main(void)
{
    Arena a = {0};
    Event_Queue eq = {0};
    Event out = {0};

    print_cstr("test_event_queue\n");

    event_queue_push(&a, &eq, 1, 100, 42, 0);
    event_queue_push(&a, &eq, 2, 101, 7, 0);

    TEST_CHECK(event_queue_pop(&eq, &out), "first pop succeeds");
    TEST_CHECK(out.type == 1 && out.tick == 100 && out.data0 == 42, "first pop returns event pushed first (FIFO)");

    TEST_CHECK(event_queue_pop(&eq, &out), "second pop succeeds");
    TEST_CHECK(out.type == 2 && out.tick == 101 && out.data0 == 7, "second pop returns event pushed second");

    TEST_CHECK(!event_queue_pop(&eq, &out), "pop on empty queue returns 0");

    event_queue_push(&a, &eq, 9, 200, 0, 0);
    event_queue_reset(&eq);
    TEST_CHECK(!event_queue_pop(&eq, &out), "reset empties the queue");

    arena_free(&a);
    print_flush();
    return test_failures_ != 0;
}
