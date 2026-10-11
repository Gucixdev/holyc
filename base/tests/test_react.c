#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#include "arena_alloc.h"
#define REACT_IMPLEMENTATION
#include "react.h"

static i32 calls_ = 0;
static i32 last_value_ = 0;

static void on_event_(void *userdata, void *event_data)
{
    i32 *counter = (i32 *)userdata;
    (*counter)++;
    calls_++;
    last_value_ = *(i32 *)event_data;
}

int main(void)
{
    Arena a = {0};
    Signal s;
    i32 counter_a = 0;
    i32 counter_b = 0;
    i32 payload = 42;

    print_cstr("test_react\n");

    signal_init(&s);
    TEST_CHECK(s.count == 0, "signal_init starts with no observers");

    signal_subscribe(&a, &s, on_event_, &counter_a);
    signal_subscribe(&a, &s, on_event_, &counter_b);
    TEST_CHECK(s.count == 2, "subscribe grows the observer count");

    signal_emit(&s, &payload);
    TEST_CHECK(counter_a == 1 && counter_b == 1, "emit calls every subscribed observer");
    TEST_CHECK(calls_ == 2 && last_value_ == 42, "each observer receives the emitted event_data");

    TEST_CHECK(signal_unsubscribe(&s, on_event_, &counter_a) == 1, "unsubscribe removes a matching (fn, userdata) pair");
    TEST_CHECK(s.count == 1, "unsubscribe shrinks the observer count");

    signal_emit(&s, &payload);
    TEST_CHECK(counter_a == 1 && counter_b == 2, "after unsubscribing, only the remaining observer fires again");

    TEST_CHECK(signal_unsubscribe(&s, on_event_, &counter_a) == 0, "unsubscribing an already-removed pair reports failure");

    print_flush();
    return test_failures_ != 0;
}
