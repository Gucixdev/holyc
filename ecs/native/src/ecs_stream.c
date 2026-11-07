/* ecs_stream.c - see ecs_stream.h. */
#include "ecs_stream.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "ecs_mem.h"

typedef struct asset {
	uint64_t bytes;
	uint32_t user;
	uint8_t state;
	uint32_t want_stamp;     /* last tick inside the load range (0 = never) */
	uint32_t keep_stamp;     /* last tick inside the keep range */
	uint32_t touch_stamp;    /* last tick this asset entered the touched list (dedupe) */
	uint32_t retry_at;       /* FAILED: not before this tick */
	float want_d2;           /* nearest observer distance^2 on want_stamp */
} asset;

typedef struct entry {
	ecs_asset_id asset;
	float max_dist;          /* <= 0: load radius */
	int32_t next;            /* next entry of the same cell, -1 end */
} entry;

typedef struct cell {
	int32_t cx, cz;
	int32_t first;           /* entries list head */
	uint32_t want_stamp, keep_stamp;
	bool ready;              /* CELL_READY emitted, CELL_RELEASED not yet */
} cell;

struct ecs_streamer {
	ecs_stream_config cfg;
	uint32_t tick;

	asset *assets;           /* index = id (slot 0 unused) */
	int n_assets, cap_assets;

	entry *entries;
	int n_entries, cap_entries;

	cell *cells;
	int n_cells, cap_cells;
	int32_t *map;            /* open-addressing: cell index or -1 */
	int map_cap;

	ecs_asset_id *touched;   /* assets inside the keep range this tick */
	int n_touched, cap_touched;
	int32_t *active_cells;   /* cells inside the keep range this tick */
	int n_active, cap_active;
	int32_t *ready_cells;    /* cells with ready == true */
	int n_ready, cap_ready;

	ecs_stream_event *events;
	int n_events, cap_events;
	int ev_head;

	uint64_t resident_bytes;
	int n_wanted;
};

ecs_stream_config ecs_stream_config_default(void)
{
	ecs_stream_config c;
	memset(&c, 0, sizeof c);
	c.cell_size = 64.0f;
	c.load_radius = 192.0f;
	c.unload_radius = 256.0f;
	c.budget_bytes = 0;
	c.max_requests_per_tick = 8;
	c.max_in_flight = 16;
	c.unload_grace_ticks = 30;
	c.retry_ticks = 300;
	return c;
}

#define GROW(arr, n, cap, T)                                                                                       \
	do {                                                                                                       \
		if ((n) >= (cap)) {                                                                                \
			int nc = (cap) ? (cap) * 2 : 16;                                                           \
			T *np = ecs_realloc((arr), (size_t)nc * sizeof(T));                                        \
			if (!np) return false;                                                                     \
			(arr) = np;                                                                                \
			(cap) = nc;                                                                                \
		}                                                                                                  \
	} while (0)

static bool push_i32(int32_t **arr, int *n, int *cap, int32_t v)
{
	GROW(*arr, *n, *cap, int32_t);
	(*arr)[(*n)++] = v;
	return true;
}

ecs_streamer *ecs_stream_new(const ecs_stream_config *cfg)
{
	ecs_streamer *s = ecs_calloc(1, sizeof *s);
	if (!s) return NULL;
	s->cfg = cfg ? *cfg : ecs_stream_config_default();
	if (s->cfg.cell_size <= 0.0f) s->cfg.cell_size = 64.0f;
	if (s->cfg.unload_radius < s->cfg.load_radius) s->cfg.unload_radius = s->cfg.load_radius;
	s->assets = ecs_calloc(16, sizeof(asset));           /* slot 0 stays zeroed: id 0 is "none" */
	if (!s->assets) {
		ecs_free(s);
		return NULL;
	}
	s->cap_assets = 16;
	s->n_assets = 1;
	return s;
}

void ecs_stream_free(ecs_streamer *s)
{
	if (!s) return;
	ecs_free(s->assets); ecs_free(s->entries); ecs_free(s->cells); ecs_free(s->map);
	ecs_free(s->touched); ecs_free(s->active_cells); ecs_free(s->ready_cells); ecs_free(s->events);
	ecs_free(s);
}

/* ---- cell hash (open addressing, linear probe, power-of-two) ---------------------------------------------------------------- */

static uint32_t hash_cell(int32_t cx, int32_t cz)
{
	uint64_t k = ((uint64_t)(uint32_t)cx << 32) | (uint32_t)cz;
	k ^= k >> 33; k *= 0xff51afd7ed558ccdULL; k ^= k >> 33; k *= 0xc4ceb9fe1a85ec53ULL; k ^= k >> 33;
	return (uint32_t)k;
}

static int cell_find(const ecs_streamer *s, int32_t cx, int32_t cz)
{
	if (!s->map_cap) return -1;
	uint32_t m = (uint32_t)s->map_cap - 1, i = hash_cell(cx, cz) & m;
	for (;;) {
		int32_t c = s->map[i];
		if (c < 0) return -1;
		if (s->cells[c].cx == cx && s->cells[c].cz == cz) return c;
		i = (i + 1) & m;
	}
}

static bool map_rebuild(ecs_streamer *s, int ncap)
{
	int32_t *m = ecs_malloc((size_t)ncap * sizeof *m);
	if (!m) return false;
	for (int i = 0; i < ncap; i++) m[i] = -1;
	for (int c = 0; c < s->n_cells; c++) {
		uint32_t mask = (uint32_t)ncap - 1, i = hash_cell(s->cells[c].cx, s->cells[c].cz) & mask;
		while (m[i] >= 0) i = (i + 1) & mask;
		m[i] = c;
	}
	ecs_free(s->map);
	s->map = m;
	s->map_cap = ncap;
	return true;
}

static int cell_get_or_add(ecs_streamer *s, int32_t cx, int32_t cz)
{
	int c = cell_find(s, cx, cz);
	if (c >= 0) return c;
	if ((s->n_cells + 1) * 2 > s->map_cap && !map_rebuild(s, s->map_cap ? s->map_cap * 2 : 64)) return -1;
	GROW(s->cells, s->n_cells, s->cap_cells, cell);
	c = s->n_cells++;
	s->cells[c] = (cell){ cx, cz, -1, 0, 0, false };
	uint32_t mask = (uint32_t)s->map_cap - 1, i = hash_cell(cx, cz) & mask;
	while (s->map[i] >= 0) i = (i + 1) & mask;
	s->map[i] = c;
	return c;
}

/* ---- description ------------------------------------------------------------------------------------------------------------ */

ecs_asset_id ecs_stream_asset_add(ecs_streamer *s, uint64_t bytes, uint32_t user)
{
	if (!s) return 0;
	GROW(s->assets, s->n_assets, s->cap_assets, asset);
	asset *a = &s->assets[s->n_assets];
	memset(a, 0, sizeof *a);
	a->bytes = bytes;
	a->user = user;
	a->state = ECS_ASSET_UNLOADED;
	return (ecs_asset_id)s->n_assets++;
}

bool ecs_stream_cell_add(ecs_streamer *s, int32_t cx, int32_t cz, ecs_asset_id asset_id, float max_dist)
{
	if (!s || asset_id == 0 || (int)asset_id >= s->n_assets) return false;
	int c = cell_get_or_add(s, cx, cz);
	if (c < 0) return false;
	GROW(s->entries, s->n_entries, s->cap_entries, entry);
	int e = s->n_entries++;
	s->entries[e] = (entry){ asset_id, max_dist, s->cells[c].first };
	s->cells[c].first = e;
	return true;
}

int32_t ecs_stream_cell_of(const ecs_streamer *s, float v) { return (int32_t)floorf(v / s->cfg.cell_size); }

/* ---- tick ------------------------------------------------------------------------------------------------------------------- */

static void push_event(ecs_streamer *s, ecs_stream_event_kind k, int32_t cx, int32_t cz)
{
	if (s->ev_head > 0 && s->ev_head == s->n_events) s->ev_head = s->n_events = 0;       /* drained: reuse from the start */
	if (s->n_events >= s->cap_events) {
		int nc = s->cap_events ? s->cap_events * 2 : 32;
		ecs_stream_event *p = ecs_realloc(s->events, (size_t)nc * sizeof *p);
		if (!p) return;                                                                  /* drop the event rather than corrupt */
		s->events = p;
		s->cap_events = nc;
	}
	s->events[s->n_events++] = (ecs_stream_event){ k, cx, cz };
}

static float rect_dist2(float px, float pz, int32_t cx, int32_t cz, float cs)
{
	float x0 = (float)cx * cs, z0 = (float)cz * cs;
	float dx = px < x0 ? x0 - px : (px > x0 + cs ? px - (x0 + cs) : 0.0f);
	float dz = pz < z0 ? z0 - pz : (pz > z0 + cs ? pz - (z0 + cs) : 0.0f);
	return dx * dx + dz * dz;
}

typedef struct cand { ecs_asset_id id; float d2; } cand;
static int cand_cmp(const void *a, const void *b)
{
	const cand *x = a, *y = b;
	if (x->d2 != y->d2) return x->d2 < y->d2 ? -1 : 1;
	return x->id < y->id ? -1 : (x->id > y->id);          /* stable, deterministic order on ties */
}

static bool resident_state(uint8_t st) { return st == ECS_ASSET_LOADING || st == ECS_ASSET_READY || st == ECS_ASSET_UNLOADING; }

int ecs_stream_tick(ecs_streamer *s, const float *ox, const float *oz, int n_obs, ecs_stream_request *out, int cap)
{
	if (!s) return 0;
	s->tick++;
	const uint32_t now = s->tick;
	const float cs = s->cfg.cell_size;
	const float load_r = s->cfg.load_radius, keep_r = s->cfg.unload_radius;
	const float margin = keep_r - load_r;
	const int rc = (int)ceilf(keep_r / cs) + 1;
	s->n_touched = 0;
	s->n_active = 0;
	s->n_wanted = 0;

	/* 1. Mark what is wanted / kept: walk only the cells near each observer (cost ~ observers * (2r+1)^2 hash lookups, independent of
	 *    how big the whole registered world is). */
	for (int o = 0; o < n_obs; o++) {
		int32_t ocx = (int32_t)floorf(ox[o] / cs), ocz = (int32_t)floorf(oz[o] / cs);
		for (int dz = -rc; dz <= rc; dz++) {
			for (int dx = -rc; dx <= rc; dx++) {
				int c = cell_find(s, ocx + dx, ocz + dz);
				if (c < 0) continue;
				cell *cl = &s->cells[c];
				float d2 = rect_dist2(ox[o], oz[o], cl->cx, cl->cz, cs);
				if (d2 > keep_r * keep_r) continue;
				if (cl->keep_stamp != now) {
					cl->keep_stamp = now;
					if (!push_i32(&s->active_cells, &s->n_active, &s->cap_active, c)) continue;
				}
				if (d2 <= load_r * load_r) cl->want_stamp = now;
				for (int32_t e = cl->first; e >= 0; e = s->entries[e].next) {
					entry *en = &s->entries[e];
					asset *a = &s->assets[en->asset];
					float lr = (en->max_dist > 0.0f && en->max_dist < load_r) ? en->max_dist : load_r;
					bool want = d2 <= lr * lr;
					bool keep = d2 <= (lr + margin) * (lr + margin);
					if (!keep) continue;
					if (a->touch_stamp != now) {
						a->touch_stamp = now;
						if (!push_i32((int32_t **)&s->touched, &s->n_touched, &s->cap_touched, (int32_t)en->asset)) continue;
					}
					a->keep_stamp = now;
					if (want) {
						if (a->want_stamp != now) { a->want_stamp = now; a->want_d2 = d2; s->n_wanted++; }
						else if (d2 < a->want_d2) a->want_d2 = d2;
					}
				}
			}
		}
	}

	int n_out = 0, in_flight = 0;
	uint64_t resident = 0;
	for (int i = 1; i < s->n_assets; i++) {
		if (s->assets[i].state == ECS_ASSET_LOADING) in_flight++;
		if (resident_state(s->assets[i].state)) resident += s->assets[i].bytes;
	}

	/* 2. Release: READY assets outside the keep range for the grace period. */
	for (int i = 1; i < s->n_assets && n_out < cap; i++) {
		asset *a = &s->assets[i];
		if (a->state != ECS_ASSET_READY || a->keep_stamp == now) continue;
		if (now - a->keep_stamp < (uint32_t)s->cfg.unload_grace_ticks) continue;
		a->state = ECS_ASSET_UNLOADING;
		out[n_out++] = (ecs_stream_request){ ECS_STREAM_UNLOAD, (ecs_asset_id)i, a->user, 0.0f };
	}

	/* 3. Loads: nearest first, throttled, budgeted (evicting least-recently-kept, un-wanted assets to make room). */
	int nc = 0;
	cand *cands = n_obs > 0 && s->n_touched > 0 ? ecs_malloc((size_t)s->n_touched * sizeof *cands) : NULL;
	for (int t = 0; cands && t < s->n_touched; t++) {
		ecs_asset_id id = (ecs_asset_id)s->touched[t];
		asset *a = &s->assets[id];
		if (a->want_stamp != now) continue;
		if (a->state == ECS_ASSET_UNLOADED || (a->state == ECS_ASSET_FAILED && now >= a->retry_at))
			cands[nc++] = (cand){ id, a->want_d2 };
	}
	if (nc > 1) qsort(cands, (size_t)nc, sizeof *cands, cand_cmp);

	int started = 0;
	for (int k = 0; k < nc && n_out < cap; k++) {
		asset *a = &s->assets[cands[k].id];
		if (s->cfg.max_requests_per_tick > 0 && started >= s->cfg.max_requests_per_tick) break;
		if (s->cfg.max_in_flight > 0 && in_flight >= s->cfg.max_in_flight) break;
		if (s->cfg.budget_bytes > 0) {
			if (resident + a->bytes > s->cfg.budget_bytes) {
				/* evict the READY asset nobody wants with the oldest keep_stamp (LRU) */
				int victim = 0;
				for (int i = 1; i < s->n_assets; i++) {
					asset *v = &s->assets[i];
					if (v->state != ECS_ASSET_READY || v->want_stamp == now) continue;
					if (!victim || v->keep_stamp < s->assets[victim].keep_stamp) victim = i;
				}
				if (victim) {
					s->assets[victim].state = ECS_ASSET_UNLOADING;
					out[n_out++] = (ecs_stream_request){ ECS_STREAM_UNLOAD, (ecs_asset_id)victim, s->assets[victim].user, 0.0f };
				}
				/* its bytes stay counted until the host confirms: we only get room once ecs_stream_asset_unloaded() lands */
			}
			if (resident + a->bytes > s->cfg.budget_bytes) continue;      /* no room yet: try a smaller/later one */
		}
		if (n_out >= cap) break;
		a->state = ECS_ASSET_LOADING;
		resident += a->bytes;
		in_flight++;
		started++;
		out[n_out++] = (ecs_stream_request){ ECS_STREAM_LOAD, cands[k].id, a->user, sqrtf(cands[k].d2) };
	}
	ecs_free(cands);

	/* 4. Cell events. A cell becomes ready once every asset it needs at the current range is resident; it is released once it falls
	 *    out of the keep range. */
	for (int i = 0; i < s->n_active; i++) {
		cell *cl = &s->cells[s->active_cells[i]];
		if (cl->ready || cl->want_stamp != now) continue;
		bool all = true;
		for (int32_t e = cl->first; e >= 0 && all; e = s->entries[e].next) {
			const entry *en = &s->entries[e];
			asset *a = &s->assets[en->asset];
			if (a->want_stamp != now) continue;              /* not needed at this cell's range right now */
			all = a->state == ECS_ASSET_READY;
		}
		if (all) {
			cl->ready = true;
			push_event(s, ECS_STREAM_CELL_READY, cl->cx, cl->cz);
			push_i32(&s->ready_cells, &s->n_ready, &s->cap_ready, s->active_cells[i]);
		}
	}
	for (int i = 0; i < s->n_ready;) {
		cell *cl = &s->cells[s->ready_cells[i]];
		if (cl->keep_stamp != now) {
			cl->ready = false;
			push_event(s, ECS_STREAM_CELL_RELEASED, cl->cx, cl->cz);
			s->ready_cells[i] = s->ready_cells[--s->n_ready];
		} else i++;
	}

	s->resident_bytes = resident;
	return n_out;
}

/* ---- host callbacks --------------------------------------------------------------------------------------------------------- */

static asset *get(ecs_streamer *s, ecs_asset_id a) { return (s && a > 0 && (int)a < s->n_assets) ? &s->assets[a] : NULL; }

void ecs_stream_asset_loaded(ecs_streamer *s, ecs_asset_id id)
{
	asset *a = get(s, id);
	if (a && a->state == ECS_ASSET_LOADING) a->state = ECS_ASSET_READY;
}

void ecs_stream_asset_failed(ecs_streamer *s, ecs_asset_id id)
{
	asset *a = get(s, id);
	if (a && a->state == ECS_ASSET_LOADING) {
		a->state = ECS_ASSET_FAILED;
		a->retry_at = s->tick + (uint32_t)s->cfg.retry_ticks;
	}
}

void ecs_stream_asset_unloaded(ecs_streamer *s, ecs_asset_id id)
{
	asset *a = get(s, id);
	if (a && a->state == ECS_ASSET_UNLOADING) a->state = ECS_ASSET_UNLOADED;
}

int ecs_stream_poll(ecs_streamer *s, ecs_stream_event *out, int max)
{
	int n = 0;
	while (s && n < max && s->ev_head < s->n_events) out[n++] = s->events[s->ev_head++];
	if (s && s->ev_head == s->n_events) s->ev_head = s->n_events = 0;
	return n;
}

ecs_asset_state ecs_stream_asset_state(const ecs_streamer *s, ecs_asset_id id)
{
	return (s && id > 0 && (int)id < s->n_assets) ? (ecs_asset_state)s->assets[id].state : ECS_ASSET_UNLOADED;
}

ecs_stream_stats ecs_stream_get_stats(const ecs_streamer *s)
{
	ecs_stream_stats st;
	memset(&st, 0, sizeof st);
	st.tick = s->tick;
	for (int i = 1; i < s->n_assets; i++) {
		switch (s->assets[i].state) {
		case ECS_ASSET_LOADING: st.loading++; break;
		case ECS_ASSET_READY: st.ready++; break;
		case ECS_ASSET_UNLOADING: st.unloading++; break;
		case ECS_ASSET_FAILED: st.failed++; break;
		default: break;
		}
		if (resident_state(s->assets[i].state)) st.resident_bytes += s->assets[i].bytes;
	}
	st.wanted = s->n_wanted;
	st.cells_ready = s->n_ready;
	return st;
}
