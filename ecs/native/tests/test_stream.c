/* test_stream.c - the streaming policy against a simulated host: nearest-first ordering, per-tick throttling, memory budget with LRU
 * eviction, hysteresis (no thrash on a border), shared-asset dedupe, failure backoff, per-asset max_dist, cell READY/RELEASED events,
 * and a long randomized walk checking invariants every tick. */
#include "ecs_stream.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); fails++; } } while (0)

/* A host that completes every load/unload at the start of the next tick. */
static void host_complete(ecs_streamer *s, const ecs_stream_request *rq, int n)
{
	for (int i = 0; i < n; i++) {
		if (rq[i].kind == ECS_STREAM_LOAD) ecs_stream_asset_loaded(s, rq[i].asset);
		else ecs_stream_asset_unloaded(s, rq[i].asset);
	}
}

static ecs_stream_config cfg(void)
{
	ecs_stream_config c = ecs_stream_config_default();
	c.cell_size = 10.0f;
	c.load_radius = 25.0f;
	c.unload_radius = 45.0f;
	c.max_requests_per_tick = 0;
	c.max_in_flight = 0;
	c.unload_grace_ticks = 0;
	c.retry_ticks = 5;
	return c;
}

int main(void)
{
	ecs_stream_request rq[256];
	float ox[2], oz[2];

	/* ---- nearest first + throttle ---- */
	{
		ecs_stream_config c = cfg();
		c.max_requests_per_tick = 3;
		ecs_streamer *s = ecs_stream_new(&c);
		ecs_asset_id ids[8];
		for (int i = 0; i < 8; i++) {                       /* one asset per cell along +x, cell i is i*10 away */
			ids[i] = ecs_stream_asset_add(s, 100, (uint32_t)i);
			ecs_stream_cell_add(s, i, 0, ids[i], 0.0f);
		}
		ox[0] = 5.0f; oz[0] = 5.0f;
		int n = ecs_stream_tick(s, ox, oz, 1, rq, 256);
		CHECK(n == 3, "max_requests_per_tick throttles new loads");
		CHECK(rq[0].user == 0 && rq[1].user == 1 && rq[2].user == 2, "nearest cells are requested first");
		CHECK(rq[0].distance <= rq[1].distance && rq[1].distance <= rq[2].distance, "request distances are non-decreasing");
		host_complete(s, rq, n);
		n = ecs_stream_tick(s, ox, oz, 1, rq, 256);
		CHECK(n >= 1 && rq[0].user == 3, "the next tick continues down the list, nothing re-requested");
		for (int i = 0; i < n; i++) CHECK(rq[i].user >= 3, "already-loaded assets are not requested again");
		ecs_stream_free(s);
	}

	/* ---- budget + LRU eviction ---- */
	{
		ecs_stream_config c = cfg();
		c.budget_bytes = 300;                               /* room for 3 assets of 100 */
		c.unload_radius = c.load_radius;                    /* no hysteresis margin: leaving range makes them evictable at once */
		ecs_streamer *s = ecs_stream_new(&c);
		ecs_asset_id ids[8];
		for (int i = 0; i < 8; i++) { ids[i] = ecs_stream_asset_add(s, 100, (uint32_t)i); ecs_stream_cell_add(s, i * 4, 0, ids[i], 0.0f); }
		ox[0] = 0.5f; oz[0] = 0.5f;                         /* sees cells 0 (and 4: 40 away is out of 25) -> only asset 0 */
		for (int t = 0; t < 4; t++) { int n = ecs_stream_tick(s, ox, oz, 1, rq, 256); host_complete(s, rq, n); }
		/* walk along: each stop has one asset in range; the budget forces older ones out */
		for (int stop = 1; stop < 8; stop++) {
			ox[0] = (float)(stop * 40) + 0.5f;
			for (int t = 0; t < 6; t++) {
				int n = ecs_stream_tick(s, ox, oz, 1, rq, 256);
				host_complete(s, rq, n);
				ecs_stream_stats st = ecs_stream_get_stats(s);
				CHECK(st.resident_bytes <= 300, "resident bytes never exceed the budget");
			}
			CHECK(ecs_stream_asset_state(s, ids[stop]) == ECS_ASSET_READY, "the asset in range is resident");
		}
		ecs_stream_free(s);
	}

	/* ---- hysteresis: no thrash on a border ---- */
	{
		ecs_stream_config c = cfg();
		ecs_streamer *s = ecs_stream_new(&c);
		ecs_asset_id a = ecs_stream_asset_add(s, 10, 1);
		ecs_stream_cell_add(s, 0, 0, a, 0.0f);              /* cell spans x in [0,10] */
		ox[0] = 0.0f; oz[0] = 0.0f;
		int n = ecs_stream_tick(s, ox, oz, 1, rq, 256);
		CHECK(n == 1 && rq[0].kind == ECS_STREAM_LOAD, "loads when inside the load radius");
		host_complete(s, rq, n);
		int unloads = 0, loads = 0;
		for (int i = 0; i < 100; i++) {                     /* wobble between 30 and 40 away: outside load (25), inside keep (45) */
			ox[0] = 40.0f + 5.0f * sinf((float)i);
			n = ecs_stream_tick(s, ox, oz, 1, rq, 256);
			for (int k = 0; k < n; k++) { if (rq[k].kind == ECS_STREAM_UNLOAD) unloads++; else loads++; }
			host_complete(s, rq, n);
		}
		CHECK(unloads == 0 && loads == 0, "no unload/reload while hovering between the load and unload radius");
		ox[0] = 200.0f;
		n = ecs_stream_tick(s, ox, oz, 1, rq, 256);
		CHECK(n == 1 && rq[0].kind == ECS_STREAM_UNLOAD, "released once beyond the unload radius");
		host_complete(s, rq, n);
		CHECK(ecs_stream_asset_state(s, a) == ECS_ASSET_UNLOADED, "and confirmed unloaded");
		ecs_stream_free(s);
	}

	/* ---- grace ticks absorb a jitter across the edge ---- */
	{
		ecs_stream_config c = cfg();
		c.unload_grace_ticks = 10;
		ecs_streamer *s = ecs_stream_new(&c);
		ecs_asset_id a = ecs_stream_asset_add(s, 10, 1);
		ecs_stream_cell_add(s, 0, 0, a, 0.0f);
		ox[0] = 0.0f; oz[0] = 0.0f;
		host_complete(s, rq, ecs_stream_tick(s, ox, oz, 1, rq, 256));
		ox[0] = 500.0f;
		int unload_tick = -1;
		for (int t = 1; t <= 20 && unload_tick < 0; t++) {
			int n = ecs_stream_tick(s, ox, oz, 1, rq, 256);
			if (n == 1 && rq[0].kind == ECS_STREAM_UNLOAD) unload_tick = t;
		}
		CHECK(unload_tick >= 10, "unload waits out the grace period");
		ecs_stream_free(s);
	}

	/* ---- shared asset: one load, kept while any referencing cell is in range ---- */
	{
		ecs_stream_config c = cfg();
		ecs_streamer *s = ecs_stream_new(&c);
		ecs_asset_id rock = ecs_stream_asset_add(s, 50, 7);
		for (int i = 0; i < 5; i++) ecs_stream_cell_add(s, i * 20, 0, rock, 0.0f);     /* referenced by 5 far-apart cells */
		ox[0] = 0.0f; oz[0] = 0.0f;
		int n = ecs_stream_tick(s, ox, oz, 1, rq, 256);
		CHECK(n == 1 && rq[0].asset == rock, "a shared asset is loaded exactly once");
		host_complete(s, rq, n);
		ox[0] = 405.0f;                                     /* cells sit at x = 0, 200, 400, 600, 800: #3 (x=400) is now in range */
		n = ecs_stream_tick(s, ox, oz, 1, rq, 256);
		CHECK(n == 0, "kept while ANY referencing cell is still in range");
		ox[0] = 5000.0f;
		n = ecs_stream_tick(s, ox, oz, 1, rq, 256);
		CHECK(n == 1 && rq[0].kind == ECS_STREAM_UNLOAD, "released once no referencing cell is in range");
		ecs_stream_free(s);
	}

	/* ---- failure backoff ---- */
	{
		ecs_stream_config c = cfg();
		ecs_streamer *s = ecs_stream_new(&c);
		ecs_asset_id a = ecs_stream_asset_add(s, 10, 1);
		ecs_stream_cell_add(s, 0, 0, a, 0.0f);
		ox[0] = 0.0f; oz[0] = 0.0f;
		int n = ecs_stream_tick(s, ox, oz, 1, rq, 256);
		CHECK(n == 1, "requested");
		ecs_stream_asset_failed(s, a);
		CHECK(ecs_stream_asset_state(s, a) == ECS_ASSET_FAILED, "failed state recorded");
		int first_retry = -1;
		for (int t = 1; t <= 12 && first_retry < 0; t++) if (ecs_stream_tick(s, ox, oz, 1, rq, 256) == 1) first_retry = t;
		CHECK(first_retry >= 5, "no retry before retry_ticks");
		CHECK(first_retry > 0, "but it is retried afterwards");
		ecs_stream_free(s);
	}

	/* ---- per-asset max_dist ---- */
	{
		ecs_stream_config c = cfg();
		ecs_streamer *s = ecs_stream_new(&c);
		ecs_asset_id big = ecs_stream_asset_add(s, 10, 1), small = ecs_stream_asset_add(s, 10, 2);
		ecs_stream_cell_add(s, 2, 0, big, 0.0f);            /* landmark: whole load radius */
		ecs_stream_cell_add(s, 2, 0, small, 8.0f);          /* prop: only within 8 units of the cell */
		ox[0] = 0.0f; oz[0] = 5.0f;                         /* the cell is 20 away */
		int n = ecs_stream_tick(s, ox, oz, 1, rq, 256);
		int got_big = 0, got_small = 0;
		for (int i = 0; i < n; i++) { if (rq[i].asset == big) got_big = 1; if (rq[i].asset == small) got_small = 1; }
		CHECK(got_big && !got_small, "a short-range asset is not loaded from far away");
		ecs_stream_free(s);
	}

	/* ---- cell events ---- */
	{
		ecs_stream_config c = cfg();
		ecs_streamer *s = ecs_stream_new(&c);
		ecs_asset_id a = ecs_stream_asset_add(s, 10, 1), b2 = ecs_stream_asset_add(s, 10, 2);
		ecs_stream_cell_add(s, 0, 0, a, 0.0f);
		ecs_stream_cell_add(s, 0, 0, b2, 0.0f);
		ecs_stream_event ev;
		ox[0] = 0.0f; oz[0] = 0.0f;
		int n = ecs_stream_tick(s, ox, oz, 1, rq, 256);
		CHECK(n == 2 && ecs_stream_poll(s, &ev, 1) == 0, "no CELL_READY before the assets are in");
		ecs_stream_asset_loaded(s, rq[0].asset);
		ecs_stream_tick(s, ox, oz, 1, rq, 256);
		CHECK(ecs_stream_poll(s, &ev, 1) == 0, "still not ready with one of two loaded");
		ecs_stream_asset_loaded(s, rq[1].asset);
		ecs_stream_tick(s, ox, oz, 1, rq, 256);
		CHECK(ecs_stream_poll(s, &ev, 1) == 1 && ev.kind == ECS_STREAM_CELL_READY && ev.cx == 0 && ev.cz == 0, "CELL_READY once complete");
		ecs_stream_tick(s, ox, oz, 1, rq, 256);
		CHECK(ecs_stream_poll(s, &ev, 1) == 0, "CELL_READY is emitted once, not every tick");
		ox[0] = 1000.0f;
		ecs_stream_tick(s, ox, oz, 1, rq, 256);
		CHECK(ecs_stream_poll(s, &ev, 1) == 1 && ev.kind == ECS_STREAM_CELL_RELEASED, "CELL_RELEASED when the cell leaves the keep radius");
		ecs_stream_free(s);
	}

	/* ---- two observers: union of their ranges ---- */
	{
		ecs_stream_config c = cfg();
		ecs_streamer *s = ecs_stream_new(&c);
		ecs_asset_id a = ecs_stream_asset_add(s, 10, 1), b2 = ecs_stream_asset_add(s, 10, 2);
		ecs_stream_cell_add(s, -50, 0, a, 0.0f);
		ecs_stream_cell_add(s, 50, 0, b2, 0.0f);
		ox[0] = -500.0f; oz[0] = 0.0f; ox[1] = 500.0f; oz[1] = 0.0f;
		int n = ecs_stream_tick(s, ox, oz, 2, rq, 256);
		CHECK(n == 2, "both players' neighbourhoods stream in");
		ecs_stream_free(s);
	}

	/* ---- randomized walk, invariants every tick ---- */
	{
		ecs_stream_config c = cfg();
		c.budget_bytes = 4000;
		c.max_requests_per_tick = 6;
		c.max_in_flight = 10;
		c.unload_grace_ticks = 3;
		ecs_streamer *s = ecs_stream_new(&c);
		enum { G = 40, NA = G * G / 2 };
		unsigned r = 4242;
		ecs_asset_id ids[NA];
		for (int i = 0; i < NA; i++) { r = r * 1664525u + 1013904223u; ids[i] = ecs_stream_asset_add(s, 50 + (r >> 8) % 200, (uint32_t)i); }
		for (int z = 0; z < G; z++)
			for (int x = 0; x < G; x++) {
				r = r * 1664525u + 1013904223u; ecs_stream_cell_add(s, x, z, ids[(r >> 8) % NA], 0.0f);
				r = r * 1664525u + 1013904223u; if ((r >> 8) % 3 == 0) ecs_stream_cell_add(s, x, z, ids[(r >> 12) % NA], 0.0f);
			}
		int bad_budget = 0, bad_inflight = 0, bad_dup = 0, pending_loads[NA + 1], pending_unloads[NA + 1];
		memset(pending_loads, 0, sizeof pending_loads);
		memset(pending_unloads, 0, sizeof pending_unloads);
		float px = 200.0f, pz = 200.0f;
		for (int t = 0; t < 600; t++) {
			r = r * 1664525u + 1013904223u; px += (float)((int)((r >> 8) % 21) - 10);
			r = r * 1664525u + 1013904223u; pz += (float)((int)((r >> 8) % 21) - 10);
			if (px < 0 || px > 400) px = 200;
			if (pz < 0 || pz > 400) pz = 200;
			ox[0] = px; oz[0] = pz;
			int n = ecs_stream_tick(s, ox, oz, 1, rq, 256);
			for (int i = 0; i < n; i++) {
				if (rq[i].kind == ECS_STREAM_LOAD) { if (pending_loads[rq[i].asset]) bad_dup++; pending_loads[rq[i].asset] = 1; }
				else { if (pending_unloads[rq[i].asset]) bad_dup++; pending_unloads[rq[i].asset] = 1; }
			}
			ecs_stream_stats st = ecs_stream_get_stats(s);
			if (st.resident_bytes > c.budget_bytes) bad_budget++;
			if (st.loading > c.max_in_flight) bad_inflight++;
			/* complete a random subset of pending work, like a real async loader */
			for (int i = 1; i <= NA; i++) {
				r = r * 1664525u + 1013904223u;
				if (pending_loads[i] && (r >> 8) % 3 == 0) { ecs_stream_asset_loaded(s, (ecs_asset_id)i); pending_loads[i] = 0; }
				else if (pending_unloads[i] && (r >> 8) % 3 == 0) { ecs_stream_asset_unloaded(s, (ecs_asset_id)i); pending_unloads[i] = 0; }
			}
		}
		CHECK(bad_budget == 0, "random walk: budget never exceeded");
		CHECK(bad_inflight == 0, "random walk: in-flight cap never exceeded");
		CHECK(bad_dup == 0, "random walk: an asset is never double-requested while its request is pending");
		ecs_stream_free(s);
	}

	if (fails) { printf("%d FAILED\n", fails); return 1; }
	printf("test_stream: ok\n");
	return 0;
}
