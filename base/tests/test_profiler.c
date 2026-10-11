#include "test_common.h"
#define TIME_UTIL_IMPLEMENTATION
#define PROFILER_IMPLEMENTATION
#include "time_util.h"
#include "profiler.h"

int main(void)
{
    Profiler p;

    print_cstr("test_profiler\n");

    profiler_init(&p);

    TEST_CHECK(profiler_get(&p, "work") == 0, "profiler_get: NULL for a scope never begun");

    profiler_begin(&p, "work");
    time_sleep_seconds(0.005);
    profiler_end(&p, "work");

    {
        const Profiler_Scope *s = profiler_get(&p, "work");
        TEST_CHECK(s != 0, "profiler_get: scope exists after one begin/end");
        TEST_CHECK(s->call_count == 1, "profiler: call_count is 1 after one begin/end");
        TEST_CHECK(s->total_seconds >= 0.004, "profiler: accumulated roughly the slept duration");
        TEST_CHECK(!s->running, "profiler: no longer marked running after end");
    }

    profiler_begin(&p, "work");
    time_sleep_seconds(0.005);
    profiler_end(&p, "work");

    {
        const Profiler_Scope *s = profiler_get(&p, "work");
        TEST_CHECK(s->call_count == 2, "profiler: call_count accumulates across multiple begin/end pairs");
        TEST_CHECK(s->total_seconds >= 0.008, "profiler: total_seconds accumulates, doesn't reset per call");
    }

    /* nesting two DIFFERENT named scopes must both time correctly */
    profiler_begin(&p, "outer");
    profiler_begin(&p, "inner");
    time_sleep_seconds(0.005);
    profiler_end(&p, "inner");
    time_sleep_seconds(0.005);
    profiler_end(&p, "outer");

    {
        const Profiler_Scope *inner = profiler_get(&p, "inner");
        const Profiler_Scope *outer = profiler_get(&p, "outer");
        TEST_CHECK(inner->total_seconds >= 0.004 && inner->total_seconds < 0.012, "profiler: nested inner scope timed independently of outer");
        TEST_CHECK(outer->total_seconds >= 0.009, "profiler: outer scope's total spans both sleeps, since it wraps the inner one");
    }

    /* end() with no matching begin() for that name is a documented no-op, not a crash */
    profiler_end(&p, "never_begun");
    TEST_CHECK(profiler_get(&p, "never_begun") != 0, "profiler_end on an unbegun name still creates the slot (find-or-create), just leaves it non-running");
    TEST_CHECK(!profiler_get(&p, "never_begun")->running, "profiler_end on an unbegun name doesn't mark it running");
    TEST_CHECK(profiler_get(&p, "never_begun")->call_count == 0, "profiler_end on an unbegun name doesn't count a call");

    profiler_report(&p);

    print_flush();
    return test_failures_ != 0;
}
