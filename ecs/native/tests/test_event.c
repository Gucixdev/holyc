/* test_event.c - registration, deferred emit/dispatch order, payload copy semantics, ANY subscription, unsubscribe (also from inside a
 * handler), re-emitting handlers (waves + cap), emit_now, polling, and payload memory reuse. */
#include "ecs_event.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); fails++; } } while (0)

typedef struct { int v; } ping;
static int seen[64], n_seen;
static void on_ping(const ecs_event *e, void *u) { (void)u; if (n_seen < 64) seen[n_seen] = ((const ping *)e->data)->v; n_seen++; }
static int any_count;
static void on_any(const ecs_event *e, void *u) { (void)e; (void)u; any_count++; }

static ecs_events *g_bus;
static int g_ping_type;
static int resub_id;
static void reemit(const ecs_event *e, void *u)
{
	(void)u;
	int v = ((const ping *)e->data)->v;
	if (v < 100) { ping p = { v + 1 }; ecs_event_emit(g_bus, (uint32_t)g_ping_type, &p); }
}
static void self_unsub(const ecs_event *e, void *u) { (void)e; (void)u; any_count += 100; ecs_event_unsubscribe(g_bus, resub_id); }

int main(void)
{
	ecs_events *bus = ecs_events_new();
	g_bus = bus;
	CHECK(ecs_event_type(bus, "damage") == ECS_EV_DAMAGE, "built-ins resolve by name");
	int t = ecs_event_register(bus, "ping", sizeof(ping));
	CHECK(t >= ECS_EV_BUILTIN_COUNT, "custom ids follow the built-ins");
	CHECK(ecs_event_register(bus, "ping", 4) == -1, "duplicate name refused");
	g_ping_type = t;

	int sub = ecs_event_subscribe(bus, (uint32_t)t, on_ping, NULL);
	CHECK(sub > 0, "subscribe");
	for (int i = 1; i <= 3; i++) { ping p = { i }; ecs_event_emit(bus, (uint32_t)t, &p); }
	CHECK(n_seen == 0 && ecs_events_pending(bus) == 3, "emit is deferred");
	ping poll;
	ecs_event ev;
	CHECK(ecs_events_count(bus, (uint32_t)t) == 3 && ecs_events_get(bus, (uint32_t)t, 1, &ev), "polling sees the queue");
	memcpy(&poll, ev.data, sizeof poll);
	CHECK(poll.v == 2, "polling by nth");

	{ ping p = { 99 }; ecs_event_emit(bus, (uint32_t)t, &p); p.v = -1; }       /* payload is copied: mutating the source is harmless */
	CHECK(ecs_events_dispatch(bus) == 4, "dispatch returns how many were delivered");
	CHECK(n_seen == 4 && seen[0] == 1 && seen[1] == 2 && seen[2] == 3 && seen[3] == 99, "FIFO order, payload captured at emit time");
	CHECK(ecs_events_pending(bus) == 0, "queue drained");

	/* ANY + unsubscribe */
	int any = ecs_event_subscribe(bus, ECS_EVENT_ANY, on_any, NULL);
	ecs_ev_entity de = { 1, 2 };
	ecs_event_emit(bus, ECS_EV_DEATH, &de);
	{ ping p = { 5 }; ecs_event_emit(bus, (uint32_t)t, &p); }
	ecs_events_dispatch(bus);
	CHECK(any_count == 2, "ANY receives every type");
	ecs_event_unsubscribe(bus, any);
	ecs_event_emit(bus, ECS_EV_DEATH, &de);
	ecs_events_dispatch(bus);
	CHECK(any_count == 2, "unsubscribed handler is not called");

	/* a handler that unsubscribes itself mid-dispatch */
	any_count = 0;
	resub_id = ecs_event_subscribe(bus, ECS_EVENT_ANY, self_unsub, NULL);
	ecs_event_emit(bus, ECS_EV_DEATH, &de);
	ecs_event_emit(bus, ECS_EV_DEATH, &de);
	ecs_events_dispatch(bus);
	CHECK(any_count == 100, "self-unsubscribe takes effect for the next event");

	/* re-emitting handler: runs in waves, bounded by max_waves */
	n_seen = 0;
	ecs_event_unsubscribe(bus, sub);
	int rs = ecs_event_subscribe(bus, (uint32_t)t, reemit, NULL);
	ecs_events_set_max_waves(bus, 4);
	{ ping p = { 0 }; ecs_event_emit(bus, (uint32_t)t, &p); }
	int delivered = ecs_events_dispatch(bus);
	CHECK(delivered == 4, "4 waves of a chain that re-emits once per delivery");
	CHECK(ecs_events_pending(bus) == 1, "the unfinished chain carries to the next dispatch");
	delivered = ecs_events_dispatch(bus);
	CHECK(delivered == 4 && ecs_events_pending(bus) == 1, "and continues, a bounded amount each time");
	ecs_event_unsubscribe(bus, rs);
	ecs_events_dispatch(bus);
	CHECK(ecs_events_pending(bus) == 0, "drains once nothing re-emits");

	/* emit_now */
	n_seen = 0;
	ecs_event_subscribe(bus, (uint32_t)t, on_ping, NULL);
	{ ping p = { 7 }; ecs_event_emit_now(bus, (uint32_t)t, &p); }
	CHECK(n_seen == 1 && seen[0] == 7 && ecs_events_pending(bus) == 0, "emit_now delivers immediately and bypasses the queue");

	/* lots of events: payload arena is reused after a drain */
	for (int round = 0; round < 50; round++) {
		for (int i = 0; i < 500; i++) { ping p = { i }; ecs_event_emit(bus, (uint32_t)t, &p); }
		n_seen = 0;
		int d = ecs_events_dispatch(bus);
		(void)d;
	}
	CHECK(ecs_events_pending(bus) == 0, "bulk traffic drains");

	ecs_events_free(bus);
	if (fails) { printf("%d FAILED\n", fails); return 1; }
	printf("test_event: ok\n");
	return 0;
}
