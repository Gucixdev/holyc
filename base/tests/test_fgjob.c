#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#define FGJOB_IMPLEMENTATION
#include "alloc/arena_alloc.h"
#include "sync/fine_grained.h"

static i32 squares_[100000];
static void square_job_(i64 id, void *ctx)
{
    (void)ctx;
    squares_[id] = (i32)(id * id);
}

static i64 volatile seq_counter_ = 0;
static i64 finish_order_[4];
static void diamond_job_(i64 id, void *ctx)
{
    i64 my_seq;
    (void)ctx;
    my_seq = __sync_fetch_and_add(&seq_counter_, 1);
    finish_order_[id] = my_seq;
}

int main(void)
{
    Thread_Pool pool;
    Arena a = {0};
    Task_Graph g;
    i64 j0, j1, j2, j3;
    i64 i;
    i32 ok;

    print_cstr("test_fgjob\n");

    thread_pool_init(&pool, 0);
    print_cstr("  workers detected: "); print_i64(pool.n_workers); print_char('\n');
    TEST_CHECK(pool.n_workers >= 1, "thread_pool_init picks at least 1 worker");

    thread_pool_run(&pool, 100000, square_job_, NULL);
    ok = 1;
    for (i = 0; i < 100000; i++) {
        if (squares_[i] != (i32)(i * i)) { ok = 0; break; }
    }
    TEST_CHECK(ok, "thread_pool_run: 100000 disjoint writes all correct");

    task_graph_init(&g);
    j0 = task_graph_add_job(&a, &g);
    j1 = task_graph_add_job(&a, &g);
    j2 = task_graph_add_job(&a, &g);
    j3 = task_graph_add_job(&a, &g);
    task_graph_add_dep(&a, &g, j2, j0);
    task_graph_add_dep(&a, &g, j2, j1);
    task_graph_add_dep(&a, &g, j3, j2);

    TEST_CHECK(task_graph_seal(&a, &g) == 0, "task_graph_seal: diamond DAG has no cycle");
    TEST_CHECK(g.level_starts.count == 3, "diamond DAG has 3 levels ({j0,j1}, {j2}, {j3})");

    task_graph_execute(&g, &pool, diamond_job_, NULL);

    TEST_CHECK(finish_order_[j0] < finish_order_[j2], "j0 finishes before j2 (its dependent)");
    TEST_CHECK(finish_order_[j1] < finish_order_[j2], "j1 finishes before j2 (its dependent)");
    TEST_CHECK(finish_order_[j2] < finish_order_[j3], "j2 finishes before j3 (its dependent)");

    /* persistent pool stress: many small dispatches reusing the same pool -
     * this is exactly the pattern (many small Task_Graph levels) that made
     * the old spawn-per-call design cost ~190us of pure thread overhead on
     * every call, and it's the shape that would expose a broken
     * active_workers/generation handoff between batches if one existed */
    {
        i32 stress_ok = 1;
        i64 sizes[] = { 1, 2, 3, 7, 16, 100, 1, 5000, 2, 100000 };
        size_t s;

        for (s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
            i64 sn = sizes[s];
            i64 k;
            for (k = 0; k < sn; k++) squares_[k] = -1; /* poison, so a skipped dispatch shows up */
            thread_pool_run(&pool, sn, square_job_, NULL);
            for (k = 0; k < sn; k++) {
                if (squares_[k] != (i32)(k * k)) { stress_ok = 0; break; }
            }
            if (!stress_ok) break;
        }
        TEST_CHECK(stress_ok, "many repeated small dispatches on the same pool stay correct");
    }

    thread_pool_destroy(&pool);

    arena_free(&a);
    print_flush();
    return test_failures_ != 0;
}
