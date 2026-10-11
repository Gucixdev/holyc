#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#define FSM_IMPLEMENTATION
#include "alloc/arena_alloc.h"
#include "sync/fsm.h"

#define STATE_IDLE 0
#define STATE_WALK 1
#define STATE_RUN  2
#define STATE_COUNT 3

static i32 enter_calls_[STATE_COUNT];
static i32 exit_calls_[STATE_COUNT];

static void on_enter_(i32 state, void *ctx) { (void)ctx; enter_calls_[state]++; }
static void on_exit_(i32 state, void *ctx) { (void)ctx; exit_calls_[state]++; }

int main(void)
{
    Arena a = {0};
    Fsm fsm;
    i32 i;

    print_cstr("test_fsm\n");

    for (i = 0; i < STATE_COUNT; i++) { enter_calls_[i] = 0; exit_calls_[i] = 0; }

    fsm_init(&a, &fsm, STATE_COUNT, STATE_IDLE, 0);
    fsm_set_enter(&fsm, STATE_IDLE, on_enter_);
    fsm_set_enter(&fsm, STATE_WALK, on_enter_);
    fsm_set_enter(&fsm, STATE_RUN, on_enter_);
    fsm_set_exit(&fsm, STATE_IDLE, on_exit_);
    fsm_set_exit(&fsm, STATE_WALK, on_exit_);
    fsm_set_exit(&fsm, STATE_RUN, on_exit_);

    TEST_CHECK(fsm_current(&fsm) == STATE_IDLE, "fsm starts in the initial state");
    TEST_CHECK(enter_calls_[STATE_IDLE] == 0, "no enter callback fires just from fsm_init (only on an actual transition)");

    fsm_transition(&fsm, STATE_WALK);
    TEST_CHECK(fsm_current(&fsm) == STATE_WALK, "fsm_transition moves to the new state");
    TEST_CHECK(exit_calls_[STATE_IDLE] == 1, "exit callback fired exactly once for the old state (idle)");
    TEST_CHECK(enter_calls_[STATE_WALK] == 1, "enter callback fired exactly once for the new state (walk)");
    TEST_CHECK(enter_calls_[STATE_IDLE] == 0, "idle's own enter callback did not fire on the transition AWAY from it");

    fsm_transition(&fsm, STATE_RUN);
    TEST_CHECK(fsm_current(&fsm) == STATE_RUN, "second transition moves to run");
    TEST_CHECK(exit_calls_[STATE_WALK] == 1, "walk's exit callback fired on transitioning away from it");
    TEST_CHECK(enter_calls_[STATE_RUN] == 1, "run's enter callback fired");

    /* transitioning to the SAME state is a no-op: no enter/exit fire again */
    fsm_transition(&fsm, STATE_RUN);
    TEST_CHECK(exit_calls_[STATE_RUN] == 0, "transitioning to the current state again does not fire exit");
    TEST_CHECK(enter_calls_[STATE_RUN] == 1, "transitioning to the current state again does not fire enter a second time");

    print_flush();
    return test_failures_ != 0;
}
