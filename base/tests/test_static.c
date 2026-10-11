#include "test_common.h"
#define STATIC_FLOW_IMPLEMENTATION
#include "static.h"

int main(void)
{
    u8 buf[8];
    Static_Flow f;

    print_cstr("test_static\n");

    static_flow_init(&f, buf, 8);
    TEST_CHECK(static_flow_remaining(&f) == 8, "a fresh static flow reports its full capacity as remaining");

    TEST_CHECK(static_flow_write(&f, "hello", 5), "a write within capacity succeeds");
    TEST_CHECK(static_flow_remaining(&f) == 3, "remaining shrinks by exactly what was written");

    TEST_CHECK(!static_flow_write(&f, "WORLD!!", 7), "a write that would overflow capacity fails outright");
    TEST_CHECK(static_flow_remaining(&f) == 3, "a failed write leaves the flow completely untouched (all-or-nothing, unlike chunk_write's partial contract)");

    TEST_CHECK(static_flow_write(&f, "xyz", 3), "a write that exactly fills the remaining capacity succeeds");
    TEST_CHECK(static_flow_remaining(&f) == 0, "capacity is now exactly exhausted");
    TEST_CHECK(!static_flow_write(&f, "a", 1), "a full flow rejects even a single more byte");

    static_flow_reset(&f);
    TEST_CHECK(static_flow_remaining(&f) == 8, "reset restores the full original capacity");
    TEST_CHECK(static_flow_write(&f, "reused", 6), "the same flow accepts fresh writes after reset");

    print_flush();
    return test_failures_ != 0;
}
