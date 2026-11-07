/* test_sched.c - fixed-step accumulator (count, remainder, alpha, spiral cap), phase order, dependency ordering from declared
 * reads/writes, batching of non-conflicting systems, explicit order edges + cycle rejection, enable/disable, time scale / pause /
 * hit-stop, a parallel runner, and event dispatch at sync points. */
#include "ecs_sched.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); fails++; } } while (0)

static char trace[256];
static int tl;
static void mark(void *u, float dt) { (void)dt; trace[tl++] = (char)(intptr_t)u; trace[tl] = 0; }
static int fixed_runs;
static float last_dt;
static void count_fixed(void *u, float dt) { (void)u; fixed_runs++; last_dt = dt; }
static int events_seen;
static void on_ev(const ecs_event *e, void *u) { (void)e; (void)u; events_seen++; }
static ecs_events *bus;
static void emit_in_fixed(void *u, float dt) { (void)u; (void)dt; ecs_ev_entity d = { 1, 2 }; ecs_event_emit(bus, ECS_EV_DEATH, &d); }

static int runner_calls, runner_max_batch;
static void par_runner(void *ctx, const ecs_task *t, int n)
{
	(void)ctx;
	runner_calls++;
	if (n > runner_max_batch) runner_max_batch = n;
	for (int i = n - 1; i >= 0; i--) t[i].fn(t[i].user, t[i].dt);          /* any order is legal inside a batch */
}

#define R(i) (1ull << (i))

int main(void)
{
	/* ---- fixed-step accumulator ---- */
	{
		ecs_sched_config c = { 0.01f, 5 };
		ecs_sched *s = ecs_sched_new(&c);
		fixed_runs = 0;
		ecs_sched_add(s, "tick", ECS_PHASE_FIXED_UPDATE, count_fixed, NULL, 0, 0);
		CHECK(ecs_sched_update(s, 0.035f) == 3 && fixed_runs == 3, "35ms at 10ms steps = 3 steps");
		CHECK(fabsf(ecs_sched_alpha(s) - 0.5f) < 1e-3f, "5ms remainder -> alpha 0.5");
		CHECK(fabsf(last_dt - 0.01f) < 1e-6f, "fixed systems always see fixed_dt");
		CHECK(ecs_sched_update(s, 0.006f) == 1, "remainder carries into the next frame");
		CHECK(ecs_sched_fixed_ticks(s) == 4, "simulation clock counts steps");
		fixed_runs = 0;
		CHECK(ecs_sched_update(s, 5.0f) == 5 && fixed_runs == 5, "a long stall is capped at max_steps");
		CHECK(ecs_sched_alpha(s) < 1.0f, "and the backlog is dropped, not carried");
		CHECK(ecs_sched_update(s, 0.0f) == 0, "zero time, zero steps");
		ecs_sched_free(s);
	}

	/* ---- phases + dependency order ---- */
	{
		ecs_sched *s = ecs_sched_new(NULL);
		tl = 0;
		/* registered out of phase order on purpose */
		ecs_sched_add(s, "post", ECS_PHASE_POST_UPDATE, mark, (void *)'P', 0, 0);
		ecs_sched_add(s, "upd", ECS_PHASE_UPDATE, mark, (void *)'U', 0, 0);
		int fx = ecs_sched_add(s, "fix", ECS_PHASE_FIXED_UPDATE, mark, (void *)'F', 0, 0);
		ecs_sched_add(s, "pre", ECS_PHASE_PRE_UPDATE, mark, (void *)'R', 0, 0);
		(void)fx;
		ecs_sched_update(s, 1.0f / 60.0f);
		CHECK(strcmp(trace, "RFUP") == 0, "pre, fixed, update, post regardless of registration order");
		ecs_sched_free(s);
	}
	{
		ecs_sched *s = ecs_sched_new(NULL);
		tl = 0;
		/* A writes R0; B reads R0 (must follow A); C touches only R1 (independent); D writes R0 (after A and B). */
		int a = ecs_sched_add(s, "A", ECS_PHASE_FIXED_UPDATE, mark, (void *)'A', 0, R(0));
		int b = ecs_sched_add(s, "B", ECS_PHASE_FIXED_UPDATE, mark, (void *)'B', R(0), 0);
		int c = ecs_sched_add(s, "C", ECS_PHASE_FIXED_UPDATE, mark, (void *)'C', R(1), 0);
		int d = ecs_sched_add(s, "D", ECS_PHASE_FIXED_UPDATE, mark, (void *)'D', 0, R(0));
		CHECK(ecs_sched_build(s), "builds");
		CHECK(ecs_sched_batch_of(s, a) == 0 && ecs_sched_batch_of(s, c) == 0, "A and C share batch 0 (no conflict)");
		CHECK(ecs_sched_batch_of(s, b) == 1, "B reads what A writes -> later batch");
		CHECK(ecs_sched_batch_of(s, d) == 2, "D writes what A wrote and B reads -> after both");
		CHECK(ecs_sched_batch_count(s, ECS_PHASE_FIXED_UPDATE) == 3, "three batches");

		runner_calls = runner_max_batch = 0;
		ecs_sched_set_runner(s, par_runner, NULL);
		ecs_sched_update(s, 1.0f / 60.0f);
		CHECK(runner_max_batch == 2, "the runner is handed the conflict-free pair together");
		CHECK(strchr(trace, 'A') < strchr(trace, 'B') && strchr(trace, 'B') < strchr(trace, 'D'), "writer -> reader -> writer ordering holds");
		ecs_sched_free(s);
	}
	/* two readers of the same resource never conflict */
	{
		ecs_sched *s = ecs_sched_new(NULL);
		int a = ecs_sched_add(s, "r1", ECS_PHASE_UPDATE, mark, (void *)'1', R(3), 0);
		int b = ecs_sched_add(s, "r2", ECS_PHASE_UPDATE, mark, (void *)'2', R(3), 0);
		ecs_sched_build(s);
		CHECK(ecs_sched_batch_of(s, a) == ecs_sched_batch_of(s, b), "readers share a batch");
		ecs_sched_free(s);
	}
	/* explicit edges, cycle rejection, disable */
	{
		ecs_sched *s = ecs_sched_new(NULL);
		tl = 0;
		int a = ecs_sched_add(s, "a", ECS_PHASE_UPDATE, mark, (void *)'a', 0, 0);
		int b = ecs_sched_add(s, "b", ECS_PHASE_UPDATE, mark, (void *)'b', 0, 0);
		CHECK(ecs_sched_order(s, b, a), "explicit order b -> a");
		ecs_sched_update(s, 0.016f);
		CHECK(strcmp(trace, "ba") == 0, "explicit edge overrides registration order");
		CHECK(ecs_sched_order(s, a, b), "adding the reverse edge is accepted as data...");
		CHECK(!ecs_sched_build(s), "...but the build detects the cycle");
		tl = 0; trace[0] = 0;
		ecs_sched_update(s, 0.016f);                              /* keeps running the last good plan */
		CHECK(ecs_sched_order(s, a, a) == false, "self edge refused");
		ecs_sched_free(s);
	}
	{
		ecs_sched *s = ecs_sched_new(NULL);
		tl = 0;
		int a = ecs_sched_add(s, "a", ECS_PHASE_UPDATE, mark, (void *)'a', 0, 0);
		ecs_sched_add(s, "b", ECS_PHASE_UPDATE, mark, (void *)'b', 0, 0);
		ecs_sched_set_enabled(s, a, false);
		ecs_sched_update(s, 0.016f);
		CHECK(strcmp(trace, "b") == 0, "a disabled system is skipped");
		ecs_sched_free(s);
	}

	/* ---- time control ---- */
	{
		ecs_sched_config c = { 0.01f, 100 };
		ecs_sched *s = ecs_sched_new(&c);
		ecs_sched_add(s, "tick", ECS_PHASE_FIXED_UPDATE, count_fixed, NULL, 0, 0);
		fixed_runs = 0;
		ecs_sched_set_time_scale(s, 0.5f);
		ecs_sched_update(s, 0.1f);
		CHECK(fixed_runs == 5, "half speed: 100ms of real time = 50ms of simulation");
		ecs_sched_set_time_scale(s, 1.0f);
		ecs_sched_set_paused(s, true);
		fixed_runs = 0;
		ecs_sched_update(s, 1.0f);
		CHECK(fixed_runs == 0, "paused: no simulation");
		ecs_sched_set_paused(s, false);
		ecs_sched_hit_stop(s, 0.05f);
		ecs_sched_update(s, 0.03f);
		CHECK(fixed_runs == 0, "hit-stop freezes the first 30ms");
		ecs_sched_update(s, 0.05f);
		CHECK(fixed_runs == 3, "...the remaining 20ms of the stop is eaten, then 30ms simulate");
		ecs_sched_free(s);
	}

	/* ---- events dispatched at sync points ---- */
	{
		bus = ecs_events_new();
		ecs_event_subscribe(bus, ECS_EV_DEATH, on_ev, NULL);
		ecs_sched_config c = { 0.01f, 10 };
		ecs_sched *s = ecs_sched_new(&c);
		ecs_sched_set_events(s, bus);
		ecs_sched_add(s, "emit", ECS_PHASE_FIXED_UPDATE, emit_in_fixed, NULL, 0, 0);
		events_seen = 0;
		ecs_sched_update(s, 0.03f);
		CHECK(events_seen == 3, "events from each fixed step are dispatched at that step's boundary");
		ecs_sched_free(s);
		ecs_events_free(bus);
	}

	if (fails) { printf("%d FAILED\n", fails); return 1; }
	printf("test_sched: ok\n");
	return 0;
}
