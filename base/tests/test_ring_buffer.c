#include "test_common.h"
#define RING_BUFFER_IMPLEMENTATION
#include "ring_buffer.h"
#include <pthread.h>

#define CAP 16

typedef struct {
    Ring_Buffer *rb;
    u32 total;
    u32 volatile produced;
    u32 volatile consumed;
    i32 volatile mismatch;
} Stress_Ctx;

/* deliberately raw pthreads, not fgjob's Thread_Pool: fgjob dispatches
 * short, independent, stateless jobs in FGJOB_CHUNK-sized batches (default
 * 256) via work-stealing - handing it exactly 2 long-running, mutually-
 * dependent jobs (producer/consumer) would very likely have both grabbed
 * into the SAME worker's chunk and run sequentially on one thread, which
 * would deadlock here (the producer's internal spin-wait for the consumer
 * to drain the buffer would just spin forever, since the consumer never
 * gets to run until the producer returns). Two dedicated OS threads that
 * must genuinely run concurrently is a different problem than fgjob
 * solves, so it gets a different tool. */
static void *producer_(void *ctx_)
{
    Stress_Ctx *ctx = (Stress_Ctx *)ctx_;
    u32 next_value = 0;
    while (ctx->produced < ctx->total) {
        void *p;
        u32 remaining = ctx->total - ctx->produced;
        u32 want = 1 + (next_value % 8);
        u32 n, i;
        if (want > remaining) want = remaining;
        n = ring_buffer_map_produce(ctx->rb, want, &p);
        if (n == 0) continue; /* full, consumer hasn't caught up - spin */
        for (i = 0; i < n; i++) { ((u32 *)p)[i] = next_value; next_value++; }
        ring_buffer_unmap_produce(ctx->rb, n);
        ctx->produced += n;
    }
    return NULL;
}

static void *consumer_(void *ctx_)
{
    Stress_Ctx *ctx = (Stress_Ctx *)ctx_;
    u32 expected = 0;
    while (ctx->consumed < ctx->total) {
        void *p;
        u32 n, i;
        n = ring_buffer_map_consume(ctx->rb, 8, &p);
        if (n == 0) continue; /* empty, producer hasn't caught up - spin */
        for (i = 0; i < n; i++) {
            if (((u32 *)p)[i] != expected) ctx->mismatch = 1;
            expected++;
        }
        ring_buffer_unmap_consume(ctx->rb, n);
        ctx->consumed += n;
    }
    return NULL;
}

int main(void)
{
    Ring_Buffer rb;
    u32 backing[CAP * 2];
    void *p;
    u32 n;
    size_t i;

    print_cstr("test_ring_buffer\n");

    ring_buffer_init(CAP, sizeof(u32), 0, backing, &rb);
    TEST_CHECK(ring_buffer_capacity(&rb) == CAP, "init: capacity matches request");
    TEST_CHECK(ring_buffer_length(&rb) == 0, "init: empty buffer has length 0");

    n = ring_buffer_map_produce(&rb, 5, &p);
    TEST_CHECK(n == 5, "map_produce: full request granted when buffer is empty");
    for (i = 0; i < 5; i++) ((u32 *)p)[i] = (u32)(i * 10);
    ring_buffer_unmap_produce(&rb, 5);
    TEST_CHECK(ring_buffer_length(&rb) == 5, "length reflects the 5 produced elements");

    n = ring_buffer_map_consume(&rb, 5, &p);
    TEST_CHECK(n == 5, "map_consume: full request granted when 5 are available");
    {
        i32 ok = 1;
        for (i = 0; i < 5; i++) if (((u32 *)p)[i] != (u32)(i * 10)) ok = 0;
        TEST_CHECK(ok, "consumed data matches what was produced");
    }
    ring_buffer_unmap_consume(&rb, 5);
    TEST_CHECK(ring_buffer_length(&rb) == 0, "length back to 0 after consuming everything");

    n = ring_buffer_map_produce(&rb, CAP + 100, &p);
    TEST_CHECK(n == CAP, "map_produce clamps an oversized request to the actual capacity");
    ring_buffer_unmap_produce(&rb, CAP);

    n = ring_buffer_map_produce(&rb, 1, &p);
    TEST_CHECK(n == 0, "map_produce returns 0 when the buffer is completely full");

    n = ring_buffer_map_consume(&rb, CAP, &p);
    ring_buffer_unmap_consume(&rb, CAP);
    TEST_CHECK(n == CAP && ring_buffer_length(&rb) == 0, "draining a full buffer empties it");

    /* wrap-around: varying batch sizes force different phase alignments
     * against the capacity boundary - the part most likely to have an
     * off-by-one if the port introduced one */
    {
        u32 produced_total = 0;
        u32 consumed_total = 0;
        i32 mismatch = 0;
        size_t round;

        for (round = 0; round < 50; round++) {
            u32 want = 3 + (u32)(round % 7);
            n = ring_buffer_map_produce(&rb, want, &p);
            for (i = 0; i < n; i++) ((u32 *)p)[i] = produced_total + (u32)i;
            ring_buffer_unmap_produce(&rb, n);
            produced_total += n;

            n = ring_buffer_map_consume(&rb, want, &p);
            for (i = 0; i < n; i++) {
                if (((u32 *)p)[i] != consumed_total + (u32)i) mismatch = 1;
            }
            ring_buffer_unmap_consume(&rb, n);
            consumed_total += n;
        }
        TEST_CHECK(!mismatch, "data survives many produce/consume cycles crossing the wrap boundary intact");
        TEST_CHECK(produced_total == consumed_total, "everything produced was eventually consumed");
    }

    {
        Ring_Buffer rb2;
        u32 backing2[64 * 2];
        Stress_Ctx ctx;
        pthread_t producer_thread, consumer_thread;

        ring_buffer_init(64, sizeof(u32), 0, backing2, &rb2);
        ctx.rb = &rb2;
        ctx.total = 200000;
        ctx.produced = 0;
        ctx.consumed = 0;
        ctx.mismatch = 0;

        pthread_create(&consumer_thread, NULL, consumer_, &ctx);
        pthread_create(&producer_thread, NULL, producer_, &ctx);
        pthread_join(producer_thread, NULL);
        pthread_join(consumer_thread, NULL);

        TEST_CHECK(!ctx.mismatch, "concurrent producer/consumer threads: no corruption/reordering across 200000 elements");
        TEST_CHECK(ctx.produced == ctx.total && ctx.consumed == ctx.total, "concurrent stress: every produced element was consumed");
    }

    print_flush();
    return test_failures_ != 0;
}
