#include "test_common.h"
#include "core/corountines.h"

typedef struct {
    Pt pt;
    i32 n;
} Counter;

static i32 counter_run(Counter *c)
{
    PT_BEGIN(&c->pt);
    for (c->n = 0; c->n < 3; c->n++) {
        PT_YIELD(&c->pt);
    }
    PT_END(&c->pt);
}

typedef struct {
    Pt pt;
    i32 *gate;
    i32 waited_steps;
} Waiter;

static i32 waiter_run(Waiter *w)
{
    PT_BEGIN(&w->pt);
    while (1) {
        PT_WAIT_UNTIL(&w->pt, *w->gate != 0);
        break; /* not part of the protothread state machine - plain C control flow once resumed past the wait */
    }
    PT_END(&w->pt);
}

typedef struct {
    Pt pt;
    i32 exit_now;
    i32 reached_after_exit;
} Exiter;

static i32 exiter_run(Exiter *e)
{
    PT_BEGIN(&e->pt);
    PT_WAIT_UNTIL(&e->pt, e->exit_now);
    PT_EXIT(&e->pt);
    e->reached_after_exit = 1; /* must never execute */
    PT_END(&e->pt);
}

int main(void)
{
    Counter c;
    Waiter w;
    Exiter e;
    i32 gate;
    i32 steps;
    i32 rv;

    print_cstr("test_corountines\n");

    /* yields exactly 3 times (n=0,1,2), then ends on the 4th call */
    pt_init(&c.pt);
    steps = 0;
    while ((rv = counter_run(&c)) == PT_YIELDED) {
        steps++;
        TEST_CHECK(c.n == steps - 1, "counter's local state (c->n) is correct at each yield point");
    }
    TEST_CHECK(steps == 3, "PT_YIELD fires exactly 3 times for a 3-iteration loop");
    TEST_CHECK(rv == PT_ENDED, "coroutine reports PT_ENDED after running off PT_END");

    /* calling an ended coroutine again restarts it cleanly (PT_END calls pt_init) */
    steps = 0;
    while ((rv = counter_run(&c)) == PT_YIELDED) steps++;
    TEST_CHECK(steps == 3, "PT_END resets state - the same Counter runs correctly a second time");

    /* PT_WAIT_UNTIL blocks (returns PT_WAITING) until the condition holds,
     * without losing track of where it's resuming from - polled from
     * outside like any protothread, no OS blocking involved */
    pt_init(&w.pt);
    gate = 0;
    w.gate = &gate;
    steps = 0;
    while ((rv = waiter_run(&w)) == PT_WAITING) {
        steps++;
        if (steps == 5) gate = 1; /* let it through on the 6th poll */
        TEST_CHECK(steps <= 5, "PT_WAIT_UNTIL keeps returning PT_WAITING while the condition is false");
    }
    TEST_CHECK(rv == PT_ENDED, "waiter reaches PT_END once the gate opens");
    TEST_CHECK(steps == 5, "PT_WAIT_UNTIL polled exactly until the gate flipped, no more no less");

    /* PT_EXIT ends the coroutine immediately, skipping anything after it -
     * unlike PT_END (falling off the end), which only happens by running
     * every statement in between */
    pt_init(&e.pt);
    e.exit_now = 0;
    e.reached_after_exit = 0;
    while ((rv = exiter_run(&e)) == PT_WAITING) {
        e.exit_now = 1;
    }
    TEST_CHECK(rv == PT_EXITED, "PT_EXIT reports PT_EXITED, distinct from PT_ENDED");
    TEST_CHECK(!e.reached_after_exit, "PT_EXIT skips code after it - never reaches the line past PT_EXIT");

    print_flush();
    return test_failures_ != 0;
}
