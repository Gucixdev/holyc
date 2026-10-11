#include "test_common.h"
#define PUBSUB_IMPLEMENTATION
#include "util/pubsub.h"

static i32 call_log_[16];
static i32 call_log_count_ = 0;

static void handler_a_(void *payload, void *ctx)
{
    (void)ctx;
    call_log_[call_log_count_++] = *(i32 *)payload * 10 + 1;
}

static void handler_b_(void *payload, void *ctx)
{
    (void)ctx;
    call_log_[call_log_count_++] = *(i32 *)payload * 10 + 2;
}

static i32 disconnect_self_count_ = 0;
static Pubsub_Signal *self_disconnect_target_;
static void handler_self_disconnect_(void *payload, void *ctx)
{
    (void)payload;
    disconnect_self_count_++;
    pubsub_signal_disconnect(self_disconnect_target_, handler_self_disconnect_, ctx);
}

int main(void)
{
    Pubsub_Signal s;
    i32 payload = 5;

    print_cstr("test_pubsub\n");

    pubsub_signal_init(&s);
    TEST_CHECK(s.count == 0, "a freshly initialized signal has no subscribers");

    pubsub_signal_connect(&s, handler_a_, NULL);
    pubsub_signal_connect(&s, handler_b_, NULL);
    TEST_CHECK(s.count == 2, "connecting two subscribers brings count to 2");

    call_log_count_ = 0;
    pubsub_signal_emit(&s, &payload);
    TEST_CHECK(call_log_count_ == 2, "emit calls both subscribers exactly once each");
    TEST_CHECK(call_log_[0] == 51, "the first-connected subscriber runs first (handler_a_: 5*10+1)");
    TEST_CHECK(call_log_[1] == 52, "the second-connected subscriber runs second (handler_b_: 5*10+2)");

    TEST_CHECK(pubsub_signal_disconnect(&s, handler_a_, NULL) == 1, "disconnecting a real subscriber returns 1");
    TEST_CHECK(s.count == 1, "count drops to 1 after disconnecting one of two");
    TEST_CHECK(pubsub_signal_disconnect(&s, handler_a_, NULL) == 0, "disconnecting the same (fn,ctx) again returns 0 - already gone");

    call_log_count_ = 0;
    pubsub_signal_emit(&s, &payload);
    TEST_CHECK(call_log_count_ == 1 && call_log_[0] == 52, "after disconnecting handler_a_, only handler_b_ still fires");

    {
        i32 i;
        Pubsub_Signal full;
        pubsub_signal_init(&full);
        for (i = 0; i < PUBSUB_MAX_SUBSCRIBERS; i++) {
            TEST_CHECK(pubsub_signal_connect(&full, handler_a_, (void *)(size_t)i) == 1, "filling up to PUBSUB_MAX_SUBSCRIBERS all succeed");
        }
        TEST_CHECK(pubsub_signal_connect(&full, handler_a_, (void *)(size_t)999) == 0, "connecting one more past capacity fails cleanly, doesn't overflow");
    }

    {
        Pubsub_Signal self_sig;
        pubsub_signal_init(&self_sig);
        self_disconnect_target_ = &self_sig;
        disconnect_self_count_ = 0;
        pubsub_signal_connect(&self_sig, handler_self_disconnect_, NULL);
        pubsub_signal_connect(&self_sig, handler_b_, NULL);
        call_log_count_ = 0;
        pubsub_signal_emit(&self_sig, &payload);
        TEST_CHECK(disconnect_self_count_ == 1, "a subscriber disconnecting itself mid-emit still only runs once for this emit");
        TEST_CHECK(call_log_count_ == 1 && call_log_[0] == 52, "the OTHER subscriber still runs even though the first one disconnected itself mid-emit");
        TEST_CHECK(self_sig.count == 1, "the self-disconnecting subscriber is really gone after the emit completes");
    }

    print_flush();
    return test_failures_ != 0;
}
