#include "test_common.h"
#define INPUT_BUFFER_IMPLEMENTATION
#include "input_buffer.h"

int main(void)
{
    Input_Buffer buf;
    Input_Event slots[8];

    print_cstr("test_input_buffer\n");

    input_buffer_init(&buf, slots, 8);
    TEST_CHECK(input_buffer_length(&buf) == 0, "a fresh buffer starts empty");

    input_buffer_push(&buf, 1, 100);
    input_buffer_push(&buf, 2, 101);
    input_buffer_push(&buf, 3, 102);
    TEST_CHECK(input_buffer_length(&buf) == 3, "length reflects 3 pushed events");

    /* has_sequence checks the LAST n pushed events specifically (not "this
       subsequence appears somewhere") - with [1,2,3] pushed, the last 2
       events are [2,3], not [1,2] */
    {
        i32 seq_last_two[2] = { 2, 3 };
        i32 seq_wrong[2] = { 3, 2 };
        i32 seq_stale[2] = { 1, 2 };
        i32 seq_too_long[4] = { 1, 2, 3, 9 };
        TEST_CHECK(input_buffer_has_sequence(&buf, seq_last_two, 2) == 1,
                   "sequence [2,3] matches the actual last 2 pushed events");
        TEST_CHECK(input_buffer_has_sequence(&buf, seq_wrong, 2) == 0,
                   "sequence [3,2] (wrong order) does NOT match the last 2 events");
        TEST_CHECK(input_buffer_has_sequence(&buf, seq_stale, 2) == 0,
                   "sequence [1,2] does NOT match - it's the last 2 EXCLUDING the newest push, not what has_sequence checks");
        TEST_CHECK(input_buffer_has_sequence(&buf, seq_too_long, 4) == 0,
                   "a sequence longer than what's actually in history is not found");
    }

    /* fill past capacity - oldest events should be overwritten */
    {
        i32 i;
        for (i = 0; i < 8; i++) input_buffer_push(&buf, 100 + i, 200 + i);
        TEST_CHECK(input_buffer_length(&buf) == 8, "length caps at buffer capacity, not unbounded growth");
        {
            i32 seq_gone[1] = { 1 }; /* the very first event pushed, now overwritten */
            TEST_CHECK(input_buffer_has_sequence(&buf, seq_gone, 1) == 0,
                       "an event pushed out by capacity overflow is no longer found");
        }
        {
            i32 seq_recent[2] = { 106, 107 };
            TEST_CHECK(input_buffer_has_sequence(&buf, seq_recent, 2) == 1,
                       "the most recently pushed events are still found");
        }
    }

    print_flush();
    return test_failures_ != 0;
}
