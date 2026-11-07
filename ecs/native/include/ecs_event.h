/* ecs_event.h - a typed event bus: systems announce things that happened (damage, death, a status landing, an ability firing) and
 * other systems react, without either knowing about the other.
 *
 * MODEL
 *  - Event types are small integers. The built-in game events (below) occupy ids 0..ECS_EV_BUILTIN_COUNT-1 so every module can emit
 *    them without a registry lookup; ecs_event_register adds your own with a fixed payload size.
 *  - ecs_event_emit COPIES the payload into a per-bus frame arena and queues it: emitting is cheap, never re-enters a handler, and
 *    the producer's stack data can die immediately. Delivery happens at ecs_events_dispatch(), which the scheduler calls at its
 *    sync points (after each fixed step and after the frame).
 *  - Handlers may emit more events. Those are delivered in the same dispatch (in waves, up to max_waves, so a feedback loop can't
 *    hang a frame; what's left is carried to the next dispatch).
 *  - ecs_event_emit_now skips the queue and calls subscribers immediately - for the rare case that must be synchronous.
 *  - You can also poll instead of subscribe: ecs_events_count/get read the queued events of a type before they are dispatched.
 *
 * Event payloads are plain structs, valid only during the callback (or until the dispatch that consumes them). Not thread-safe. */
#ifndef ECS_EVENT_H
#define ECS_EVENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ecs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ecs_builtin_event {
	ECS_EV_DAMAGE,            /* ecs_ev_damage:  a hit was applied                              */
	ECS_EV_DEATH,             /* ecs_ev_entity:  HP reached zero                                */
	ECS_EV_STAGGER,           /* ecs_ev_entity:  poise broke                                    */
	ECS_EV_EXHAUSTED,         /* ecs_ev_entity:  SP reached zero                                */
	ECS_EV_STATUS_APPLIED,    /* ecs_ev_status:  a buildup status triggered                     */
	ECS_EV_STATUS_ENDED,      /* ecs_ev_status:  it wore off                                    */
	ECS_EV_EFFECT_APPLIED,    /* ecs_ev_effect:  a gameplay effect landed (or gained a stack)   */
	ECS_EV_EFFECT_EXPIRED,    /* ecs_ev_effect:  it ran out or was removed                      */
	ECS_EV_ABILITY_ACTIVATED, /* ecs_ev_ability: an ability fired                               */
	ECS_EV_ABILITY_FAILED,    /* ecs_ev_ability: activation refused, `reason` says why          */
	ECS_EV_BUILTIN_COUNT
} ecs_builtin_event;

#define ECS_EVENT_ANY 0xFFFFFFFFu     /* subscribe to every type */

enum { ECS_HIT_KILLED = 1u << 0, ECS_HIT_STAGGERED = 1u << 1, ECS_HIT_CRIT = 1u << 2 };

typedef struct ecs_ev_damage { ecs_entity target, source; float dealt, overkill, poise; uint32_t flags; } ecs_ev_damage;
typedef struct ecs_ev_entity { ecs_entity entity, source; } ecs_ev_entity;
typedef struct ecs_ev_status { ecs_entity entity; uint32_t status; } ecs_ev_status;     /* status = ecs_buildup index */
typedef struct ecs_ev_effect { ecs_entity target, source; int32_t def, stacks; } ecs_ev_effect;
typedef struct ecs_ev_ability { ecs_entity user, target; int32_t ability, reason; } ecs_ev_ability;

typedef struct ecs_event {
	uint32_t type;
	uint32_t size;
	const void *data;
} ecs_event;

typedef void (*ecs_event_fn)(const ecs_event *ev, void *user);

typedef struct ecs_events ecs_events;

ecs_events *ecs_events_new(void);
void ecs_events_free(ecs_events *e);

/* Adds a custom type; returns its id (>= ECS_EV_BUILTIN_COUNT), or -1 if the name is taken / OOM. payload_size may be 0. */
int ecs_event_register(ecs_events *e, const char *name, size_t payload_size);
int ecs_event_type(const ecs_events *e, const char *name);          /* -1 if unknown (built-ins resolve too) */

bool ecs_event_emit(ecs_events *e, uint32_t type, const void *payload);   /* queued; false on a bad type or OOM */
void ecs_event_emit_now(ecs_events *e, uint32_t type, const void *payload);

int ecs_event_subscribe(ecs_events *e, uint32_t type, ecs_event_fn fn, void *user);   /* subscription id > 0, 0 on failure */
void ecs_event_unsubscribe(ecs_events *e, int sub);                                    /* safe from inside a handler */

/* Delivers everything queued (and what handlers emit in turn, up to max_waves). Returns how many events were delivered. */
int ecs_events_dispatch(ecs_events *e);
void ecs_events_set_max_waves(ecs_events *e, int waves);                               /* default 8 */
int ecs_events_pending(const ecs_events *e);

/* Polling over the not-yet-dispatched queue. */
int ecs_events_count(const ecs_events *e, uint32_t type);
bool ecs_events_get(const ecs_events *e, uint32_t type, int nth, ecs_event *out);

#ifdef __cplusplus
}
#endif
#endif
