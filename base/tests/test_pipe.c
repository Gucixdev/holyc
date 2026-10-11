#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#include "arena_alloc.h"
#define PIPE_IMPLEMENTATION
#include "pipe.h"

int main(void)
{
    Arena a = {0};
    Pipe p;

    print_cstr("test_pipe\n");

    pipe_init(&p, 8);
    TEST_CHECK(!pipe_is_closed(&p), "a freshly initialized pipe is not closed");
    TEST_CHECK(!pipe_is_drained(&p), "an open empty pipe is not drained (more could still come)");

    pipe_write(&a, &p, "data", 4);
    {
        char out[4];
        size_t got = pipe_read(&p, out, 4);
        TEST_CHECK(got == 4, "read gets back everything written");
    }
    TEST_CHECK(!pipe_is_drained(&p), "an open pipe with no buffered bytes is STILL not drained until closed");

    pipe_close(&p);
    TEST_CHECK(pipe_is_closed(&p), "close sets the closed flag");
    TEST_CHECK(pipe_is_drained(&p), "closed with nothing buffered left counts as fully drained");

    print_flush();
    return test_failures_ != 0;
}
