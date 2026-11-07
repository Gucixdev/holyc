/* ecs_stream.h - distance-driven asset/cell streaming POLICY for large worlds: what to load, in what order, how much may be resident,
 * what to evict - with no I/O of its own.
 *
 * WHY THIS EXISTS
 *  An engine's resource loader answers "load this path, tell me when it's done". It does not answer the questions a big streamed
 *  world needs: which of 50,000 placed assets should be resident for these N players/cameras right now, in what priority, within a
 *  memory budget, without thrashing at a cell border, without loading a shared rock mesh 300 times, and when is a region actually
 *  safe to spawn gameplay into. This module is exactly that decision layer; the host does the loading.
 *
 * HOW A HOST DRIVES IT (the loop is the same in any engine)
 *    s = ecs_stream_new(&cfg);
 *    id = ecs_stream_asset_add(s, bytes, my_path_index);            // once per distinct asset; `user` is yours (index into a path table)
 *    ecs_stream_cell_add(s, cx, cz, id, max_dist);                  // which cell references it (shared assets are deduplicated by id)
 *    each frame / every few frames:
 *        n = ecs_stream_tick(s, obs_x, obs_z, n_obs, requests, cap);
 *        for each request: LOAD   -> start an async load of asset's `user`           (Godot: ResourceLoader.load_threaded_request)
 *                          UNLOAD -> drop it, then ecs_stream_asset_unloaded()
 *        when a load finishes:  ecs_stream_asset_loaded(s, id)   (or ecs_stream_asset_failed)
 *        while (ecs_stream_poll(s, &ev, 1)) ...                    CELL_READY -> spawn that cell's entities; CELL_RELEASED -> free them
 *
 * RULES IT ENFORCES
 *  - Nearest first: loads go out in order of distance to the closest observer.
 *  - Budgeted: resident+loading bytes never exceed budget_bytes; over budget, the least-recently-needed assets that nothing wants are
 *    evicted first. An asset that cannot fit is simply not requested this tick.
 *  - Hysteresis: assets load inside load_radius but are only released beyond unload_radius (>= load_radius), so standing on a cell
 *    border doesn't thrash. A further unload_grace_ticks delay absorbs a camera jittering across the edge.
 *  - Throttled: at most max_requests_per_tick new loads per tick and max_in_flight concurrent loads, so a teleport doesn't stall.
 *  - Deduplicated: an asset referenced by many cells is one load, kept while ANY referencing cell is in range.
 *  - Failure-safe: a failed load backs off retry_ticks before it is requested again; nothing spins.
 *
 * The world is a grid on the XZ plane (Y ignored - streaming is a horizontal concern; vertical LOD belongs to the renderer).
 * Single-threaded and deterministic: same inputs, same request order. Call everything from one thread (Godot: the main thread, where
 * the threaded loader's completion is also polled). */
#ifndef ECS_STREAM_H
#define ECS_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t ecs_asset_id;       /* 0 is never valid */

typedef struct ecs_stream_config {
	float cell_size;                 /* world units per cell edge (> 0) */
	float load_radius;               /* assets in cells this close to ANY observer are wanted */
	float unload_radius;             /* ... and are only released beyond this (>= load_radius) */
	uint64_t budget_bytes;           /* cap on loading+resident bytes (0 = unlimited) */
	int max_requests_per_tick;       /* new loads started per tick (0 = unlimited) */
	int max_in_flight;               /* concurrent loads (0 = unlimited) */
	int unload_grace_ticks;          /* ticks an asset must stay out of range before it is released */
	int retry_ticks;                 /* backoff after a failed load */
} ecs_stream_config;

typedef enum ecs_asset_state {
	ECS_ASSET_UNLOADED,
	ECS_ASSET_LOADING,
	ECS_ASSET_READY,
	ECS_ASSET_UNLOADING,
	ECS_ASSET_FAILED
} ecs_asset_state;

typedef enum ecs_stream_req_kind { ECS_STREAM_LOAD, ECS_STREAM_UNLOAD } ecs_stream_req_kind;

typedef struct ecs_stream_request {
	ecs_stream_req_kind kind;
	ecs_asset_id asset;
	uint32_t user;                   /* the `user` value given to ecs_stream_asset_add */
	float distance;                  /* to the nearest observer at request time (LOAD only; 0 for UNLOAD) */
} ecs_stream_request;

typedef enum ecs_stream_event_kind {
	ECS_STREAM_CELL_READY,           /* every asset this cell needs at its current range is resident: safe to spawn into */
	ECS_STREAM_CELL_RELEASED         /* the cell left the keep radius: despawn what you spawned for it */
} ecs_stream_event_kind;

typedef struct ecs_stream_event {
	ecs_stream_event_kind kind;
	int32_t cx, cz;
} ecs_stream_event;

typedef struct ecs_stream_stats {
	uint32_t tick;
	uint64_t resident_bytes;         /* loading + ready + unloading */
	int loading, ready, unloading, failed;
	int wanted;                      /* assets inside the load radius this tick */
	int cells_ready;
} ecs_stream_stats;

typedef struct ecs_streamer ecs_streamer;

ecs_stream_config ecs_stream_config_default(void);
ecs_streamer *ecs_stream_new(const ecs_stream_config *cfg);   /* cfg copied; NULL = defaults */
void ecs_stream_free(ecs_streamer *s);

/* ---- world description ------------------------------------------------------------------------------------------------------- */
ecs_asset_id ecs_stream_asset_add(ecs_streamer *s, uint64_t bytes, uint32_t user);   /* 0 on OOM */
/* Makes `asset` part of cell (cx,cz). max_dist limits when it is wanted: only while an observer is within that distance of the cell
 * (<= 0: the load radius). Use it to keep small props from loading as far out as landmarks. */
bool ecs_stream_cell_add(ecs_streamer *s, int32_t cx, int32_t cz, ecs_asset_id asset, float max_dist);
int32_t ecs_stream_cell_of(const ecs_streamer *s, float world_coord);                /* floor(coord / cell_size) */

/* ---- per-tick ---------------------------------------------------------------------------------------------------------------- */
/* Writes up to cap requests and returns how many were written. Requests beyond cap are not lost: the assets stay un-requested and
 * come out on a later tick. Observers are positions on the XZ plane (cameras, players). */
int ecs_stream_tick(ecs_streamer *s, const float *obs_x, const float *obs_z, int n_obs, ecs_stream_request *out, int cap);

/* Host completion callbacks. Unknown ids / wrong-state calls are ignored (the host may report late or twice). */
void ecs_stream_asset_loaded(ecs_streamer *s, ecs_asset_id a);
void ecs_stream_asset_failed(ecs_streamer *s, ecs_asset_id a);
void ecs_stream_asset_unloaded(ecs_streamer *s, ecs_asset_id a);

/* Pops up to max pending cell events (FIFO). Returns how many. */
int ecs_stream_poll(ecs_streamer *s, ecs_stream_event *out, int max);

/* ---- introspection ----------------------------------------------------------------------------------------------------------- */
ecs_asset_state ecs_stream_asset_state(const ecs_streamer *s, ecs_asset_id a);
ecs_stream_stats ecs_stream_get_stats(const ecs_streamer *s);

#ifdef __cplusplus
}
#endif
#endif
