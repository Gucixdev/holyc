# Roadmap toward a flecs/bevy-competitive feature set

The goal stated for this project is to genuinely compete with flecs and bevy_ecs, not just borrow their storage strategy. This is an
honest list of what that actually takes beyond today's core, roughly ordered by how much a typical user would miss it first, with a
rough sense of how big each piece is. None of this is started yet unless marked done.

- [x] **Archetype core**: entities, generation-checked handles, component registration incl. tags, add/remove/get/has with correct
      cross-archetype data migration, per-entity and bulk/table views. Tested directed + 20k-op randomized stress, clean under ASan+UBSan.

- [x] **Allocator hooks** (`ecs_mem.h`), **SIMD math** (`ecs_math.h`), **souls-like stats** (`ecs_stats.h`) and **streaming policy**
      (`ecs_stream.h`): done and tested (directed + differential + randomized, ASan/UBSan clean, scalar build bit-identical).

- [ ] **Godot GDExtension binding**: thin C glue over the C ABI (generate `gdextension_interface.h` with
      `godot --dump-gdextension-interface`), exposing `EcsWorld`/`EcsStats`/`EcsStreamer` classes, column views as packed arrays, and
      the streamer wired to `ResourceLoader.load_threaded_*`. Medium; the C API was shaped for this, so no core changes expected.

- [ ] **Stats: active-set ticking**: today every row is touched every tick (memory-bandwidth bound at 100k rows). Track a dirty/active
      set so idle entities cost nothing. Small-medium.

- [ ] **Streaming: spatial index + resident list**: the tick scans all registered assets for release/eviction (fine to ~100k assets);
      keep an intrusive resident list and a cell grid index for 1M+ placed assets. Medium. Also: vertical (Y) bands and per-observer
      view-direction bias for load priority.

- [ ] **Query caching**: today, `ecs_view_new` does a linear scan of every archetype to find matches, every time. flecs caches a query's
      matching archetype list and updates it incrementally as new archetypes are created, so a query built once and reused every frame
      (the normal case for a system) doesn't re-scan. Medium: needs a query object distinct from a one-shot view, and archetype-creation
      needs to notify live cached queries.

- [ ] **Relationships / pairs**: flecs's signature feature - a component id that is itself an (relation, target) pair, so you can say
      "this entity is a ChildOf that entity" or "this entity Likes that entity" and query over relationships, not just plain component
      presence. Large: touches component id representation (pairs need their own id space), archetype signature comparison, and query
      matching (wildcard queries over a relation).

- [x] **System scheduler (v1)**: `ecs_sched.h` - phases, fixed timestep, declared read/write conflict batching, parallel-runner hook,
      time control. NOT yet: auto-derived reads/writes from the query's component list, and a built-in thread pool (the runner hook is
      where Godot's WorkerThreadPool plugs in).
- [x] **Events (v1)**: `ecs_event.h` - typed bus with deferred delivery. NOT yet: component add/remove/set *observers* inside the
      archetype core (the core still has no hooks; `ecs_stats_collect` papers over entity destruction).
- [x] **Gameplay effects/abilities (v1)**: `ecs_gas.h`.
- [ ] **Original: A real system scheduler**: right now a "system" is just a function you call yourself with a view, in whatever order you write the
      calls. A scheduler would let you register systems with declared component reads/writes, resolve a safe execution order (or safe
      parallel batches) from that automatically, and run a "frame" as one call. Medium-large.

- [ ] **Parallel/multithreaded iteration**: splitting a bulk view's tables (or a single large table) across worker threads. Needs the
      scheduler's read/write conflict info first to be safe - two systems that only *read* the same component can run concurrently, two
      that write it can't. Large, and easy to get subtly wrong; do this after the scheduler, not before.

- [ ] **Change detection**: let a system ask "which entities' Position changed since I last ran" instead of re-processing everyone every
      frame. flecs and bevy both do this with a per-component "last written" tick compared against a system's "last ran" tick. Medium.

- [ ] **Hierarchies / prefabs**: parent-child entity trees (usually built on relationships, above) and prefab entities that others can be
      instantiated from with their component values pre-filled. Medium once relationships exist, awkward without them.

- [ ] **Events / observers**: callbacks that fire when a component is added/removed/set on an entity, or on a custom event type. Medium.

- [ ] **Serialization / reflection**: describing a component's fields at runtime (for save/load, a debugger UI, network replication)
      rather than it being an opaque blob of bytes the way it is today. Large - this is most of what makes flecs's own reflection system
      and its explorer/REST tooling possible, and it's a lot of surface area (field types, offsets, nested structs, arrays...).

- [ ] **A query DSL / string queries**: flecs lets you write a query as a string ("Position, Velocity, !Dead") parsed at runtime; today
      this library only has the C array-of-ids form. Small-medium, mostly a parser, once relationships/wildcards exist to parse toward.

- [ ] **Bigger entity handles or wider generations**: today's 24-bit index / 8-bit generation packing caps out around 16M live slots
      and a stale handle could in principle alias after exactly 256 destroys of the same slot. Fine for now; revisit if either limit is
      ever actually hit in practice rather than pre-emptively.

Anything not listed here (a specific flecs/bevy feature you want that isn't on this list) - say so and it gets triaged in.
