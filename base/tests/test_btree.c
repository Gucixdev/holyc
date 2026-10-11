#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#define BTREE_IMPLEMENTATION
#include "arena_alloc.h"
#include "btree.h"

static i32 call_count_ = 0;

static Bt_Status always_success_(void *ctx) { (void)ctx; call_count_++; return BT_SUCCESS; }
static Bt_Status always_failure_(void *ctx) { (void)ctx; call_count_++; return BT_FAILURE; }

/* runs BT_RUNNING the first N calls, then BT_SUCCESS - a real "in
   progress" action, like an animation or a timed wait */
static i32 running_calls_left_ = 0;
static Bt_Status runs_then_succeeds_(void *ctx)
{
    (void)ctx;
    call_count_++;
    if (running_calls_left_ > 0) {
        running_calls_left_--;
        return BT_RUNNING;
    }
    return BT_SUCCESS;
}

int main(void)
{
    Arena a = {0};

    print_cstr("test_btree\n");

    /* Sequence: all children succeed -> sequence succeeds */
    {
        Bt_Node *leaf1 = bt_leaf(&a, always_success_);
        Bt_Node *leaf2 = bt_leaf(&a, always_success_);
        Bt_Node *children[2];
        Bt_Node *seq;
        Bt_Status status;

        children[0] = leaf1; children[1] = leaf2;
        seq = bt_sequence(&a, children, 2);

        call_count_ = 0;
        status = bt_tick(seq, 0);
        TEST_CHECK(status == BT_SUCCESS, "sequence of 2 succeeding leaves returns SUCCESS");
        TEST_CHECK(call_count_ == 2, "sequence ran BOTH children when the first one succeeded");
    }

    /* Sequence: first child fails -> sequence fails immediately, second child never runs */
    {
        Bt_Node *leaf1 = bt_leaf(&a, always_failure_);
        Bt_Node *leaf2 = bt_leaf(&a, always_success_);
        Bt_Node *children[2];
        Bt_Node *seq;
        Bt_Status status;

        children[0] = leaf1; children[1] = leaf2;
        seq = bt_sequence(&a, children, 2);

        call_count_ = 0;
        status = bt_tick(seq, 0);
        TEST_CHECK(status == BT_FAILURE, "sequence with a failing first child returns FAILURE");
        TEST_CHECK(call_count_ == 1, "sequence stopped after the failing child - second child never ran");
    }

    /* Selector: first child fails, second succeeds -> selector succeeds, tried both */
    {
        Bt_Node *leaf1 = bt_leaf(&a, always_failure_);
        Bt_Node *leaf2 = bt_leaf(&a, always_success_);
        Bt_Node *children[2];
        Bt_Node *sel;
        Bt_Status status;

        children[0] = leaf1; children[1] = leaf2;
        sel = bt_selector(&a, children, 2);

        call_count_ = 0;
        status = bt_tick(sel, 0);
        TEST_CHECK(status == BT_SUCCESS, "selector with a failing then succeeding child returns SUCCESS");
        TEST_CHECK(call_count_ == 2, "selector tried both children (first failed, fell through to second)");
    }

    /* Selector: all children fail -> selector fails */
    {
        Bt_Node *leaf1 = bt_leaf(&a, always_failure_);
        Bt_Node *leaf2 = bt_leaf(&a, always_failure_);
        Bt_Node *children[2];
        Bt_Node *sel;
        Bt_Status status;

        children[0] = leaf1; children[1] = leaf2;
        sel = bt_selector(&a, children, 2);

        status = bt_tick(sel, 0);
        TEST_CHECK(status == BT_FAILURE, "selector with all children failing returns FAILURE");
    }

    /* RUNNING resumability: a sequence's second child returns RUNNING for
       2 ticks, then SUCCESS - the sequence must resume from that same
       child on the next tick rather than re-running the first child */
    {
        Bt_Node *leaf1 = bt_leaf(&a, always_success_);
        Bt_Node *leaf2 = bt_leaf(&a, runs_then_succeeds_);
        Bt_Node *children[2];
        Bt_Node *seq;

        children[0] = leaf1; children[1] = leaf2;
        seq = bt_sequence(&a, children, 2);

        running_calls_left_ = 2;
        call_count_ = 0;

        TEST_CHECK(bt_tick(seq, 0) == BT_RUNNING, "tick 1: sequence returns RUNNING (leaf2 still running)");
        TEST_CHECK(call_count_ == 2, "tick 1: both leaf1 (success) and leaf2 (running) were called");

        call_count_ = 0;
        TEST_CHECK(bt_tick(seq, 0) == BT_RUNNING, "tick 2: sequence still RUNNING");
        TEST_CHECK(call_count_ == 1, "tick 2: sequence resumed at leaf2 directly - leaf1 was NOT re-run");

        call_count_ = 0;
        TEST_CHECK(bt_tick(seq, 0) == BT_SUCCESS, "tick 3: leaf2 finally succeeds, sequence succeeds");
        TEST_CHECK(call_count_ == 1, "tick 3: sequence still resumed at leaf2 directly, not leaf1");

        /* after completing, the NEXT tick starts over from leaf1 again */
        call_count_ = 0;
        running_calls_left_ = 0;
        TEST_CHECK(bt_tick(seq, 0) == BT_SUCCESS, "tick 4: a fresh run after completion starts over from leaf1");
        TEST_CHECK(call_count_ == 2, "tick 4: both children ran again on the fresh run");
    }

    print_flush();
    return test_failures_ != 0;
}
