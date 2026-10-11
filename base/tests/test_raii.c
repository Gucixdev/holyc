#include "test_common.h"
#include "util/raii.h"

static i32 released_count_ = 0;

static void mark_released_(void *resource)
{
    i32 *counter = (i32 *)resource;
    (*counter)++;
}

static void scope_with_early_return_(i32 take_early_exit)
{
    Raii_Guard g RAII_SCOPED = { &released_count_, mark_released_ };
    (void)g;
    if (take_early_exit) return;
}

int main(void)
{
    print_cstr("test_raii\n");

    TEST_CHECK(released_count_ == 0, "nothing released before any scope has run");

    {
        Raii_Guard g RAII_SCOPED = { &released_count_, mark_released_ };
        (void)g;
        TEST_CHECK(released_count_ == 0, "the finalizer has not run yet while still inside the scope");
    }
    TEST_CHECK(released_count_ == 1, "the finalizer runs automatically at the end of a normal scope exit");

    scope_with_early_return_(1);
    TEST_CHECK(released_count_ == 2, "the finalizer also runs on an early return out of the scope");

    {
        Raii_Guard empty RAII_SCOPED;
        empty.resource = NULL;
        empty.finalizer = mark_released_;
    }
    TEST_CHECK(released_count_ == 2, "a guard with a NULL resource does not call its finalizer");

    print_flush();
    return test_failures_ != 0;
}
