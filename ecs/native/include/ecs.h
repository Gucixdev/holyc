/* ecs.h - an archetype-based Entity-Component-System, plain C99 + libc, no other dependency. This is the same storage strategy flecs and
 * bevy_ecs use (as opposed to a sparse-set/EnTT-style design): every entity lives in exactly one "archetype" - a table holding every
 * entity that currently has the exact same set of component types, its components stored as tightly packed, contiguous columns (one
 * array per component type, indexed in lockstep with the entity list). Adding or removing a component moves the entity to a different
 * archetype, copying its other components' data along - archetype ECS trades a heavier add/remove (O(components) to copy) for a much
 * faster, cache-friendly, per-entity-check-free iteration (a query just walks the matching archetypes' columns directly), which is the
 * right trade for most simulations/games where systems run over many entities every frame far more often than components are added or
 * removed. Archetypes are linked into a graph (an "add this component" / "remove this component" edge per neighbour, cached after the
 * first transition), so repeated add/remove of the same component type from the same starting shape is O(1) after the first time.
 *
 * A tag is just a zero-sized component - no column, no data, purely presence/absence (ecs_register_tag is a thin convenience over
 * ecs_register with size 0). Tags and data components share the exact same storage/query machinery.
 *
 * Status: a real, tested archetype core - entities, components, tags, and queries over an archetype graph. It is NOT yet a flecs/bevy-
 * competitive feature set: no relationships/pairs, no system scheduler (a "system" here is just a plain function you call with a view),
 * no parallel iteration, no change detection, no hierarchies/prefabs, no reflection. See ROADMAP.md for what a fuller build-out needs -
 * that is real, ongoing, multi-session work, not a one-sitting rewrite.
 *
 * Nothing here is thread-safe; one ecs_world is meant to be driven from one thread. */
#ifndef ECS_H
#define ECS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t ecs_entity;
#define ECS_NULL ((ecs_entity)0xFFFFFFFFu)                 /* never a valid handle; a safe zero-value / "no entity" sentinel */

typedef struct ecs_world ecs_world;

ecs_world *ecs_world_new(void);
void ecs_world_free(ecs_world *w);

/* ---- entities: create/destroy, and whether a handle still refers to what it did when you got it. A fresh entity starts in the empty
 * archetype (no components at all) and moves as components are added. ---- */
ecs_entity ecs_create(ecs_world *w);
void ecs_destroy(ecs_world *w, ecs_entity e);              /* a stale/already-dead handle is a no-op */
bool ecs_alive(const ecs_world *w, ecs_entity e);
int ecs_entity_count(const ecs_world *w);                  /* how many entities are currently alive */

/* ---- component types: register each plain-data struct (or a zero-sized tag) once, up front ---- */
int ecs_register(ecs_world *w, const char *name, size_t component_size);   /* size 0 = a tag; -1 if `name` is already taken */
int ecs_register_tag(ecs_world *w, const char *name);                      /* ecs_register(w, name, 0) */
int ecs_component_id(const ecs_world *w, const char *name);                /* -1 if no such name was registered */
const char *ecs_component_name(const ecs_world *w, int comp);
size_t ecs_component_size(const ecs_world *w, int comp);                   /* 0 for a tag */
int ecs_type_count(const ecs_world *w);                                    /* how many component types (incl. tags) are registered */

/* ---- components: add/remove/query one entity's data. ecs_add's returned pointer stays valid until the NEXT add/remove of ANY
 * component on ANY entity that shares this one's archetype (that can move rows around, including this one's) - copy out of it, or
 * re-fetch with ecs_get, rather than holding it across such a call. A tag has no data; ecs_get on one returns a non-NULL, non-
 * dereferenceable presence marker (use ecs_has to test a tag, not ecs_get). ---- */
void *ecs_add(ecs_world *w, ecs_entity e, int comp);       /* already has it -> returns the existing data untouched */
void ecs_remove(ecs_world *w, ecs_entity e, int comp);     /* doesn't have it, or a dead handle -> a no-op */
void *ecs_get(ecs_world *w, ecs_entity e, int comp);       /* NULL if it doesn't have that component (or the handle is stale/dead) */
bool ecs_has(const ecs_world *w, ecs_entity e, int comp);
int ecs_type_count_entities(const ecs_world *w, int comp); /* how many entities currently have this component, across every archetype */

/* ---- views: the entities that have every one of `comps` (an AND, not an OR) - walks whichever archetypes are a superset of the query
 * directly, with no per-entity membership check needed (that's the point of the archetype layout). Don't add/remove a component of a
 * type the view is querying, on any entity, while the view is still open - same rule as any array you're mid-loop over. ---- */
typedef struct ecs_view ecs_view;
ecs_view *ecs_view_new(ecs_world *w, const int *comps, int n);
bool ecs_view_next(ecs_view *v, ecs_entity *out);          /* advances and returns true, or returns false once exhausted (v is spent) */
void ecs_view_free(ecs_view *v);
int ecs_view_archetype_count(const ecs_view *v);           /* how many archetypes matched - a rough cost/shape indicator, mainly for tests */

/* ---- bulk/table iteration: the fast path - walk the matching archetypes one whole table at a time instead of one entity at a time.
 * ecs_view_column's returned array is exactly ecs_view_count() elements of that component's size, contiguous, so a system can loop over
 * it directly - or hand it to your own SIMD code, since this contiguous-per-type layout is exactly what makes that possible. Pick one
 * iteration style (ecs_view_next, or this) per view; they share the same cursor. */
bool ecs_view_next_table(ecs_view *v);                     /* advances to the next matching archetype; false once exhausted */
int ecs_view_count(const ecs_view *v);                     /* how many entities are in the CURRENT table */
const ecs_entity *ecs_view_entities(const ecs_view *v);    /* ecs_view_count() of them, parallel to every column below */
void *ecs_view_column(const ecs_view *v, int comp);        /* NULL if the current table doesn't have that component, or it's a tag */

#ifdef __cplusplus
}
#endif
#endif
