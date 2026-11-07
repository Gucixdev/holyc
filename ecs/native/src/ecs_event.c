/* ecs_event.c - see ecs_event.h. */
#include "ecs_event.h"

#include <string.h>

#include "ecs_arena.h"
#include "ecs_mem.h"

typedef struct type_info { char *name; uint32_t size; } type_info;
typedef struct queued { uint32_t type, size; void *data; } queued;
typedef struct sub { uint32_t type; ecs_event_fn fn; void *user; int id; } sub;

struct ecs_events {
	type_info *types;
	int n_types, cap_types;

	queued *q;                 /* queued events; [head, n_q) are undelivered */
	int n_q, cap_q, head;
	ecs_arena *payloads;

	sub *subs;
	int n_subs, cap_subs, next_sub;
	int dispatching;           /* > 0 while handlers run: unsubscribe must not compact */
	bool in_dispatch;          /* guards against a handler calling ecs_events_dispatch recursively */
	int max_waves;
};

static const char *BUILTIN_NAMES[ECS_EV_BUILTIN_COUNT] = {
	"damage", "death", "stagger", "exhausted", "status_applied", "status_ended",
	"effect_applied", "effect_expired", "ability_activated", "ability_failed"
};
static const uint32_t BUILTIN_SIZES[ECS_EV_BUILTIN_COUNT] = {
	sizeof(ecs_ev_damage), sizeof(ecs_ev_entity), sizeof(ecs_ev_entity), sizeof(ecs_ev_entity), sizeof(ecs_ev_status),
	sizeof(ecs_ev_status), sizeof(ecs_ev_effect), sizeof(ecs_ev_effect), sizeof(ecs_ev_ability), sizeof(ecs_ev_ability)
};

static char *dup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *p = ecs_malloc(n);
	if (p) memcpy(p, s, n);
	return p;
}

static bool add_type(ecs_events *e, const char *name, uint32_t size)
{
	if (e->n_types == e->cap_types) {
		int nc = e->cap_types ? e->cap_types * 2 : 32;
		type_info *t = ecs_realloc(e->types, (size_t)nc * sizeof *t);
		if (!t) return false;
		e->types = t;
		e->cap_types = nc;
	}
	char *n = dup(name);
	if (!n) return false;
	e->types[e->n_types].name = n;
	e->types[e->n_types].size = size;
	e->n_types++;
	return true;
}

ecs_events *ecs_events_new(void)
{
	ecs_events *e = ecs_calloc(1, sizeof *e);
	if (!e) return NULL;
	e->payloads = ecs_arena_new(16 * 1024);
	e->max_waves = 8;
	e->next_sub = 1;
	if (!e->payloads) { ecs_free(e); return NULL; }
	for (int i = 0; i < ECS_EV_BUILTIN_COUNT; i++) {
		if (!add_type(e, BUILTIN_NAMES[i], BUILTIN_SIZES[i])) { ecs_events_free(e); return NULL; }
	}
	return e;
}

void ecs_events_free(ecs_events *e)
{
	if (!e) return;
	for (int i = 0; i < e->n_types; i++) ecs_free(e->types[i].name);
	ecs_free(e->types);
	ecs_free(e->q);
	ecs_free(e->subs);
	ecs_arena_free(e->payloads);
	ecs_free(e);
}

int ecs_event_type(const ecs_events *e, const char *name)
{
	for (int i = 0; i < e->n_types; i++)
		if (strcmp(e->types[i].name, name) == 0) return i;
	return -1;
}

int ecs_event_register(ecs_events *e, const char *name, size_t payload_size)
{
	if (!e || !name || ecs_event_type(e, name) >= 0) return -1;
	if (!add_type(e, name, (uint32_t)payload_size)) return -1;
	return e->n_types - 1;
}

bool ecs_event_emit(ecs_events *e, uint32_t type, const void *payload)
{
	if (!e || type >= (uint32_t)e->n_types) return false;
	uint32_t size = e->types[type].size;
	if (e->n_q == e->cap_q) {
		int nc = e->cap_q ? e->cap_q * 2 : 64;
		queued *q = ecs_realloc(e->q, (size_t)nc * sizeof *q);
		if (!q) return false;
		e->q = q;
		e->cap_q = nc;
	}
	void *copy = NULL;
	if (size) {
		copy = ecs_arena_alloc(e->payloads, size);
		if (!copy) return false;
		if (payload) memcpy(copy, payload, size);
		else memset(copy, 0, size);
	}
	e->q[e->n_q++] = (queued){ type, size, copy };
	return true;
}

static void deliver(ecs_events *e, uint32_t type, uint32_t size, const void *data)
{
	ecs_event ev = { type, size, data };
	e->dispatching++;
	int n = e->n_subs;                                    /* subscribers added by a handler start with the next event */
	for (int i = 0; i < n; i++) {
		sub *s = &e->subs[i];
		if (s->fn && (s->type == type || s->type == ECS_EVENT_ANY)) s->fn(&ev, s->user);
	}
	e->dispatching--;
}

void ecs_event_emit_now(ecs_events *e, uint32_t type, const void *payload)
{
	if (!e || type >= (uint32_t)e->n_types) return;
	deliver(e, type, e->types[type].size, payload);
}

int ecs_event_subscribe(ecs_events *e, uint32_t type, ecs_event_fn fn, void *user)
{
	if (!e || !fn || (type != ECS_EVENT_ANY && type >= (uint32_t)e->n_types)) return 0;
	if (e->n_subs == e->cap_subs) {
		if (e->dispatching) return 0;                      /* growing would move the array under a running handler loop */
		int nc = e->cap_subs ? e->cap_subs * 2 : 16;
		sub *s = ecs_realloc(e->subs, (size_t)nc * sizeof *s);
		if (!s) return 0;
		e->subs = s;
		e->cap_subs = nc;
	}
	int id = e->next_sub++;
	e->subs[e->n_subs++] = (sub){ type, fn, user, id };
	return id;
}

void ecs_event_unsubscribe(ecs_events *e, int id)
{
	if (!e || id <= 0) return;
	for (int i = 0; i < e->n_subs; i++) {
		if (e->subs[i].id != id) continue;
		e->subs[i].fn = NULL;                              /* tombstone: compaction waits until nothing is iterating */
		break;
	}
	if (!e->dispatching) {
		int w = 0;
		for (int i = 0; i < e->n_subs; i++)
			if (e->subs[i].fn) e->subs[w++] = e->subs[i];
		e->n_subs = w;
	}
}

int ecs_events_dispatch(ecs_events *e)
{
	if (!e || e->in_dispatch) return 0;
	e->in_dispatch = true;
	int delivered = 0;
	for (int wave = 0; wave < e->max_waves && e->head < e->n_q; wave++) {
		int end = e->n_q;                                  /* snapshot: events emitted by handlers belong to the next wave */
		for (int i = e->head; i < end; i++) {
			queued q = e->q[i];
			deliver(e, q.type, q.size, q.data);
			delivered++;
		}
		e->head = end;
	}
	if (e->head >= e->n_q) {                               /* fully drained: the payload memory can be reused */
		e->head = e->n_q = 0;
		ecs_arena_reset(e->payloads);
	}
	if (!e->dispatching) {                                 /* drop tombstones left by handlers that unsubscribed */
		int w = 0;
		for (int i = 0; i < e->n_subs; i++)
			if (e->subs[i].fn) e->subs[w++] = e->subs[i];
		e->n_subs = w;
	}
	e->in_dispatch = false;
	return delivered;
}

void ecs_events_set_max_waves(ecs_events *e, int waves) { if (e && waves > 0) e->max_waves = waves; }
int ecs_events_pending(const ecs_events *e) { return e->n_q - e->head; }

int ecs_events_count(const ecs_events *e, uint32_t type)
{
	int n = 0;
	for (int i = e->head; i < e->n_q; i++)
		if (e->q[i].type == type) n++;
	return n;
}

bool ecs_events_get(const ecs_events *e, uint32_t type, int nth, ecs_event *out)
{
	for (int i = e->head; i < e->n_q; i++) {
		if (e->q[i].type != type) continue;
		if (nth-- == 0) {
			*out = (ecs_event){ e->q[i].type, e->q[i].size, e->q[i].data };
			return true;
		}
	}
	return false;
}
