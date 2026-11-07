# ecs

A small archetype-based Entity-Component-System in plain C99 + libc, no other dependency. This is the same storage strategy
[flecs](https://github.com/SanderMertens/flecs) and [bevy_ecs](https://github.com/bevyengine/bevy) use: every entity lives in exactly one
"archetype" (a table for its exact component signature), with each component type stored as one tightly packed, contiguous array per
archetype. See `include/ecs.h` for the full design rationale and API doc comments - that header is the actual spec, this file is the map.

## Status

A real, tested archetype core: entities (with generation-checked handles so a stale handle safely reads as dead), component
registration (including zero-sized tags), add/remove/get/has that correctly moves an entity's data between archetypes, and views that
iterate the matching archetypes either one entity at a time or one whole table at a time (the bulk path, which hands back a raw
contiguous column pointer per component - the same shape a compiler auto-vectorizes, or that you can hand to your own SIMD code).

This is **not yet** a flecs/bevy-competitive feature set. See `ROADMAP.md` for exactly what's missing and roughly how big each piece is -
that gap is real, ongoing, multi-session work, not something a rewrite closes in one sitting.

## Modules beyond the core

The archetype core is generic. Three modules sit next to it, each usable on its own and all built to be bound to an engine (Godot
first) without changing the C API - plain handles, flat columns, no globals, one allocator hook (`ecs_mem.h`):

| header | what it is |
|---|---|
| `ecs_arena.h` | A bump **arena** (alloc = pointer add, mark/rewind, reset keeps a chunk) and a fixed-size **slab** (O(1) alloc/free, stable pointers, double-free refused). Used internally for event payloads and per-tick scratch. |
| `ecs_event.h` | Typed **event bus**: queued emit (payload copied to a frame arena), subscribe/unsubscribe, ordered delivery in bounded waves, polling, `emit_now`. Built-in game events (damage/death/stagger/exhausted/status/effect/ability). |
| `ecs_sched.h` | **Tick/system scheduler**: PRE / FIXED / UPDATE / POST phases, fixed-timestep accumulator with spiral cap and render alpha, systems ordered from declared reads/writes into conflict-free **batches** (hand a batch to any parallel runner), explicit order edges, time scale / pause / hit-stop, event dispatch at sync points. |
| `ecs_gas.h` | **Gameplay-ability layer** (Unreal-GAS-shaped, data-oriented): 64 gameplay tags, effects (instant / timed / periodic DoT-HoT, stacking policies, exactly-undone modifiers, tag gating, cleanse), abilities (cost, cooldown-as-a-tag, gates, failure reasons). |
| `ecs_math.h` | Bulk float kernels (add/scale/clamp/lerp/approach, reductions, SoA vec3, masks/compaction). Plain loops, compiled per ISA (avx2+fma / sse4.2 / baseline) with load-time dispatch. **Bit-identical on every ISA** (`-ffp-contract=off`, fixed 8-lane reductions) - safe for replays and netcode. |
| `ecs_stats.h` | Souls-like stats as a dense SoA table: HP/FP/SP/Poise pools with regen + regen delay (and a longer delay when SP is bottomed out), equip load -> 3 roll classes (+ OVERLOADED flag), per-type defense and resist, **weapons with attribute scaling -> attack rating -> hit** (defense, poise, crit, buildup), poison/bleed/frost/curse buildup with timed status + DoT, attribute soft-cap curves, derived flags, events. One vector tick for every entity; columns are exposed raw for zero-copy binding. |
| `ecs_stream.h` | Streaming *policy* for big worlds: nearest-first loads, per-tick throttle, memory budget + LRU eviction, hysteresis, shared-asset dedupe, failure backoff, per-asset range, cell READY/RELEASED events. No I/O - the host (Godot's threaded loader) does the loading and reports back. |

See each header's top comment: those are the specs. Measured on this machine: a stats tick over 100,000 entities (~35 columns each)
is ~1.3 ms, memory-bandwidth bound rather than ALU bound.

### Engine binding contract (what "designed for Godot from the start" means here)
- Everything is a C ABI with opaque handles (`ecs_world*`, `ecs_stats*`, `ecs_streamer*`); entities are 32-bit values. A GDExtension
  wrapper is thin glue, no redesign.
- Columns are contiguous, 32-byte aligned and padded, so a binding can expose them as `PackedFloat32Array`-style views or upload
  them to a GPU buffer without copying. Pointers are stable until the next add; re-fetch after adding.
- All allocation goes through `ecs_set_allocator()` so Godot's `memalloc` can own it (and its leak tracking sees it).
- The streamer never blocks and never does I/O: it emits LOAD/UNLOAD requests and consumes completions, matching
  `ResourceLoader.load_threaded_request` + polling on the main thread.
- Single-threaded per object by design today; the scheduler/parallel iteration items in `ROADMAP.md` come before any threading.

## Layout

```
include/ecs.h        the core API - read this first
include/ecs_mem.h    allocator hooks          include/ecs_math.h   SIMD kernels
include/ecs_stats.h  souls-like stat table    include/ecs_stream.h streaming policy
src/                 the implementations (one .c per header)
tests/test_ecs.c      directed tests: entities, registration, add/remove/get/has across archetype moves, views
tests/test_arena.c    arena alignment/growth/mark-rewind/reset reuse; slab reuse, stability, double-free
tests/test_event.c    ordering, payload copy, ANY, unsubscribe in handlers, waves + cap, polling, bulk reuse
tests/test_sched.c    fixed-step maths, phases, dependency batching, cycles, time control, parallel runner, event sync points
tests/test_combat.c   stamina exhaustion delay, 3 roll classes, weapon scaling, hit/crit/poise/death/status events exactly once
tests/test_gas.c      tags, instant/timed/periodic effects, exact undo, stacking, gating/cleanse, DoT kill credit, abilities
tests/test_math.c     every kernel bit-compared to a scalar reference over awkward lengths and unaligned starts
tests/test_stats.c    membership, regen/delay/clamp, defense, poise, buildup+DoT, load, attributes, 20000-row differential vs a model
tests/test_stream.c   ordering, throttle, budget+LRU, hysteresis, dedupe, backoff, events, 600-tick randomized walk with invariants
tests/test_stress.c   20000 random creates/destroys/adds/removes across 5 component types, checking every entity's data against an
                      independent tracker after every single operation - the test most likely to catch a wrong-column-copied-on-move bug
```

## Building

```
make            # libecs.a
make test       # build + run both test suites
make asan       # the same, under ASan+UBSan with leak detection
```

No configure step, no external dependencies - just a C99 compiler and `ar`.

## A minimal example

```c
#include "ecs.h"

typedef struct { float x, y; } Pos;
typedef struct { float x, y; } Vel;

ecs_world *w = ecs_world_new();
int POS = ecs_register(w, "Pos", sizeof(Pos));
int VEL = ecs_register(w, "Vel", sizeof(Vel));
int ENEMY = ecs_register_tag(w, "Enemy");     /* a tag: presence-only, no data */

ecs_entity e = ecs_create(w);
Pos *p = ecs_add(w, e, POS); p->x = 1; p->y = 2;
Vel *v = ecs_add(w, e, VEL); v->x = 0; v->y = -1;
ecs_add(w, e, ENEMY);

/* a system, walked table-at-a-time for speed: */
int q[2] = {POS, VEL};
ecs_view *view = ecs_view_new(w, q, 2);
while (ecs_view_next_table(view)) {
    int n = ecs_view_count(view);
    Pos *pos = ecs_view_column(view, POS);
    Vel *vel = ecs_view_column(view, VEL);
    for (int i = 0; i < n; i++) { pos[i].x += vel[i].x; pos[i].y += vel[i].y; }
}
ecs_view_free(view);

ecs_world_free(w);
```

## Why archetypes, not a sparse set

The simpler, and arguably easier to implement correctly, alternative is a sparse-set-per-component-type design (EnTT's original
approach): O(1) add/remove, but a query has to walk the smallest component's array and check membership in every other one, one entity
at a time. Archetypes push the cost the other way: add/remove is O(components on the entity) because it copies a whole row to a new
table, but a query just walks the matching tables' columns directly - no membership check, no branching per entity, and the data is
laid out exactly right for the compiler (or you) to vectorize. For a typical simulation or game, systems iterate far more often than
components are added or removed, so that trade almost always wins - which is why both flecs and bevy_ecs made the same choice.
