#include "test_common.h"
#define BRAIN_IMPLEMENTATION
#include "brain.c"

static i32 think_calls_ = 0;
static f32 last_elapsed_ = -1.0f;

static void think_(void *ctx, f32 elapsed)
{
    i32 *counter = (i32 *)ctx;
    (*counter)++;
    think_calls_++;
    last_elapsed_ = elapsed;
}

int main(void)
{
    Brain b;
    i32 my_counter = 0;

    print_cstr("test_brain\n");

    brain_init(&b, think_, &my_counter, 0.5f);

    TEST_CHECK(brain_update(&b, 0.2f) == 0, "a small dt below think_interval does not trigger think()");
    TEST_CHECK(think_calls_ == 0, "think really was not called yet");

    TEST_CHECK(brain_update(&b, 0.2f) == 0, "still below the 0.5s threshold after accumulating 0.4s total");

    TEST_CHECK(brain_update(&b, 0.2f) == 1, "crossing the 0.5s threshold (0.6s total) triggers think()");
    TEST_CHECK(think_calls_ == 1, "think was called exactly once");
    TEST_CHECK(my_counter == 1, "think's own ctx-mutating side effect actually ran");
    TEST_CHECK(last_elapsed_ > 0.59f && last_elapsed_ < 0.61f, "think receives the real accumulated elapsed time, not just think_interval");

    TEST_CHECK(brain_update(&b, 0.1f) == 0, "the accumulator resets after firing - a small dt right after does not immediately fire again");

    {
        i32 i;
        for (i = 0; i < 4; i++) brain_update(&b, 0.1f); /* 0.1 (leftover) + 0.4 = 0.5 total */
        TEST_CHECK(think_calls_ == 2, "accumulating back up to the threshold across several small updates fires think() again");
    }

    print_flush();
    return test_failures_ != 0;
}
