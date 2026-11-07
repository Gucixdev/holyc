/* ecs_sched.h - the tick/system scheduler: registers systems, orders them from what they declare they touch, and runs a frame as
 * one call with a fixed-timestep simulation loop.
 *
 * FRAME SHAPE (what ecs_sched_update does)
 *      PRE_UPDATE   once per frame, dt = frame time (input, streaming tick, AI sensing)
 *      FIXED_UPDATE 0..max_steps times per frame at a constant fixed_dt (gameplay/physics/combat - deterministic, stats ticks here)
 *      UPDATE       once per frame (animation drivers, camera)
 *      POST_UPDATE  once per frame (late binding: write state out to the engine)
 *  Events are dispatched after every fixed step and at the end of the frame, if a bus is attached.
 *
 * WHY FIXED STEPS: simulation that depends on frame time is not reproducible (replays, netcode, tuning "regen = X per second" and
 * having it feel the same at 30 and 144 fps). The accumulator runs FIXED_UPDATE as many times as the elapsed time covers, capped at
 * max_steps so a stall can't snowball into a spiral of death; excess time is dropped. alpha (0..1) is how far into the next step
 * you are, for rendering interpolation.
 *
 * ORDERING: each system declares which resources it reads and writes as 64-bit masks (bits mean whatever the host decides: component
 * ids, "the stats table", "physics world"). Two systems conflict if one writes what the other reads or writes; conflicting systems
 * keep registration order, non-conflicting ones may share a BATCH. ecs_sched_order adds explicit edges. Batches are the unit a
 * parallel runner can fan out: set a runner (Godot's WorkerThreadPool, a job system) and a batch runs concurrently; the default
 * runs it serially. A conflict-free batch is what makes that safe - the scheduler never starts two conflicting systems together.
 *
 * TIME CONTROL: time_scale (slow-mo), pause, and hit-stop (freeze for N seconds, then resume) apply to all of it.
 * Single-threaded unless you set a runner. */
#ifndef ECS_SCHED_H
#define ECS_SCHED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ecs_event.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ecs_phase {
	ECS_PHASE_PRE_UPDATE,
	ECS_PHASE_FIXED_UPDATE,
	ECS_PHASE_UPDATE,
	ECS_PHASE_POST_UPDATE,
	ECS_PHASE_COUNT
} ecs_phase;

typedef void (*ecs_system_fn)(void *user, float dt);

typedef struct ecs_sched_config {
	float fixed_dt;          /* seconds per simulation step (default 1/60) */
	int max_steps;           /* cap per frame (default 5) */
} ecs_sched_config;

/* One unit of work handed to a runner. A runner must complete all `n` tasks before returning; it may run them in parallel. */
typedef struct ecs_task { ecs_system_fn fn; void *user; float dt; } ecs_task;
typedef void (*ecs_batch_runner)(void *ctx, const ecs_task *tasks, int n);

typedef struct ecs_sched ecs_sched;

ecs_sched_config ecs_sched_config_default(void);
ecs_sched *ecs_sched_new(const ecs_sched_config *cfg);
void ecs_sched_free(ecs_sched *s);

/* Registers a system. Returns its id (>= 0), -1 on failure. Call ecs_sched_build after adding/ordering/enabling. */
int ecs_sched_add(ecs_sched *s, const char *name, ecs_phase phase, ecs_system_fn fn, void *user, uint64_t reads, uint64_t writes);
bool ecs_sched_order(ecs_sched *s, int before, int after);         /* `before` runs before `after`; same phase only */
void ecs_sched_set_enabled(ecs_sched *s, int id, bool enabled);
bool ecs_sched_build(ecs_sched *s);                                 /* false on an ordering cycle (previous plan is kept) */

void ecs_sched_set_events(ecs_sched *s, ecs_events *ev);            /* dispatched at the sync points; NULL detaches */
void ecs_sched_set_runner(ecs_sched *s, ecs_batch_runner fn, void *ctx);   /* NULL = serial */

/* Runs one frame. Returns the number of fixed steps executed. Builds the plan first if it is stale. */
int ecs_sched_update(ecs_sched *s, float frame_dt);

void ecs_sched_set_time_scale(ecs_sched *s, float scale);           /* 0 pauses simulation time; default 1 */
void ecs_sched_set_paused(ecs_sched *s, bool paused);
void ecs_sched_hit_stop(ecs_sched *s, float seconds);               /* freeze simulation for `seconds` of real time */

uint64_t ecs_sched_fixed_ticks(const ecs_sched *s);                 /* fixed steps executed so far: the simulation clock */
uint64_t ecs_sched_frames(const ecs_sched *s);
float ecs_sched_alpha(const ecs_sched *s);                          /* 0..1 progress toward the next fixed step */
float ecs_sched_fixed_dt(const ecs_sched *s);

/* Introspection for tests/tools. */
int ecs_sched_batch_count(const ecs_sched *s, ecs_phase phase);
int ecs_sched_batch_of(const ecs_sched *s, int id);                 /* batch index within its phase, -1 if disabled/unbuilt */

#ifdef __cplusplus
}
#endif
#endif
