#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#include "arena_alloc.h"
#define DYNAMIC_FLOW_IMPLEMENTATION
#include "dynamic.h"

int main(void)
{
    Arena a = {0};
    Dynamic_Flow f;

    print_cstr("test_dynamic\n");

    dynamic_flow_init(&f, 4); /* tiny chunks, forces real multi-chunk growth */
    TEST_CHECK(dynamic_flow_is_empty(&f), "a fresh dynamic flow is empty");

    dynamic_flow_write(&a, &f, "hello world", 11); /* spans multiple 4-byte chunks */
    TEST_CHECK(!dynamic_flow_is_empty(&f), "a flow with written data is not empty");

    {
        char out[16];
        size_t got = dynamic_flow_read(&f, out, 11);
        TEST_CHECK(got == 11, "read recovers everything written, seamlessly across chunk growth");
        TEST_CHECK(out[0]=='h' && out[10]=='d', "the recovered bytes are exactly what was written, in order");
    }
    TEST_CHECK(dynamic_flow_is_empty(&f), "the flow is empty again once everything is read back");

    /* unlike static_flow_write, a write here never fails regardless
     * of how much data goes through */
    {
        i32 i;
        for (i = 0; i < 100; i++) {
            dynamic_flow_write(&a, &f, "more-data", 9);
        }
        TEST_CHECK(1, "900 bytes written through 4-byte chunks without any capacity failure (a dynamic flow always grows)");
    }

    print_flush();
    return test_failures_ != 0;
}
