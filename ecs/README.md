# ecs

> Moved here from gdtoolbox (2026-11-07): gdtoolbox is GDScript only now. `native/` is the C core; `godot/` is the
> GDExtension binding plus the GDScript layer, kept together so it can be copied back into a project. The prebuilt
> .so/.dll were not moved; build them with the Makefile in native/.

Archetype-table Entity-Component-System. The actual storage (entities, component registration,
archetype tables, queries) is a native C core (`EcsWorld`/`EcsView`, a GDExtension binding over
a standalone C99 library) - this folder's GDScript is a thin, optional layer on top of it, not
the storage itself. Replaces the old `EzEntity`/`EzComponent`/`EzSystem` (Node + SceneTree-group
facade, O(n) scans) with real contiguous-column storage and bulk per-table iteration.

```
ecs.gdextension                 points at bin/ for each platform/config
bin/
  libecs_gdext.linux.template_debug.x86_64.so     editor + debug export
  libecs_gdext.linux.template_release.x86_64.so   release export
Ecs.gd                          autoload singleton: shared EcsWorld + component-id cache, plus
                                 lazy shared EcsStats/EcsGas/EcsScheduler/EcsStreamer
EcsComponent.gd                 base for a component definition (byte_size/to_bytes/from_bytes)
EcsEntity.gd                    Node wrapper around one native entity id, incl. stats/gas helpers
EcsSystem.gd                    Node wrapper that runs a query every _process()
test/
  EcsExampleStatComponent.gd    generic depletable/regenerating float stat (hp/mp/sp/...)
  EcsExampleHp.gd / Mp.gd / Sp.gd    one-line subclasses (class_name only, all logic inherited)
  EcsExampleStatTickSystem.gd   generic regen/decay tick for one stat component
```

The native core and this binding's source are in [`native/`](native/) (C99, `native/godot/gdext`
for the GDExtension); `native/godot/README.md` has the exact build commands, and the binaries in
`bin/` are built from it. The folder carries a `.gdignore`, so the editor does not scan the C sources.

Linux x86_64 and Windows x86_64 ship (debug + release binaries, cross-built via mingw-w64 for
Windows). macOS isn't set up - it needs Apple's SDK (osxcross) to cross-build from Linux, which
isn't available here; add a build under `bin/` and a matching entry in `ecs.gdextension` if you
get access to one. Building for Windows surfaced one real portability bug in the native core:
`ecs_mem.c`'s aligned allocator used POSIX's `posix_memalign`, which MinGW/MSVC don't have -
fixed with a `_WIN32` branch using `_aligned_malloc`/`_aligned_free` instead.

## Using it

Define a component by subclassing `EcsComponent`:

```gdscript
@tool
class_name Position
extends EcsComponent

static func byte_size() -> int:
	return 8 # 2x float32

static func to_bytes(data: Dictionary) -> PackedByteArray:
	var b := PackedByteArray()
	b.resize(8)
	b.encode_float(0, data.get("x", 0.0))
	b.encode_float(4, data.get("y", 0.0))
	return b

static func from_bytes(bytes: PackedByteArray):
	return {"x": bytes.decode_float(0), "y": bytes.decode_float(4)}
```

A tag (pure presence/absence, e.g. `Dead`) only needs `byte_size()` left at the default 0 -
skip `to_bytes`/`from_bytes`. The script needs a `class_name`; that name is also the native
component's registered name (Godot's own global-class-name uniqueness is what keeps two
different component types from colliding).

Attach data to entities via `EcsEntity` (a plain `Node`, drop it anywhere in the tree - under a
`CharacterBody3D`, a UI root, whatever owns the entity conceptually):

```gdscript
var e := EcsEntity.new()
add_child(e)
e.add_component(Position, {"x": 1.0, "y": 2.0})
var p: Dictionary = e.get_component(Position)
e.remove_component(Position)
```

Write a system by subclassing `EcsSystem` and overriding `_process_batch()` - it's called once
per matching archetype table, not once per entity:

```gdscript
class_name MoveSystem
extends EcsSystem

func _ready() -> void:
	required_components = [Position, Velocity]
	super._ready()

func _process_batch(view: EcsView, delta: float) -> void:
	var pos_id := Ecs.register(Position)
	var vel_id := Ecs.register(Velocity)
	var pos := view.column_bytes(pos_id)   # a COPY - mutate it, then write it back
	var vel := view.column_bytes(vel_id)
	for i in view.count():
		pos.encode_float(i * 8, pos.decode_float(i * 8) + vel.decode_float(i * 8) * delta)
		pos.encode_float(i * 8 + 4, pos.decode_float(i * 8 + 4) + vel.decode_float(i * 8 + 4) * delta)
	view.set_column_bytes(pos_id, pos)
```

Drop one instance of a system node anywhere in the tree (an autoload, or under the level root);
it runs its query every `_process()` while `active` is true. `EcsExampleStatTickSystem` is a
ready-made example of this pattern (generic regen/decay for any `EcsExampleStatComponent`
subclass - set `stat_script` in the Inspector).

For a one-entity-at-a-time style instead of bulk columns, call `view.next()` yourself (returns
an entity id, or -1 once exhausted) rather than `next_table()`/`column_bytes()`.

## Stats, gas, scheduler, streamer

Beyond the base archetype API, the native core also provides souls-like combat stats
(`EcsStats`), tags/effects/abilities (`EcsGas`), a phase/fixed-timestep scheduler
(`EcsScheduler`) and distance-driven asset streaming (`EcsStreamer`) - all raw GDExtension
classes, bindable directly. `Ecs.gd` adds one `ensure_*()` per module that lazily creates a
single shared instance (not every game needs combat or streaming, so none of this is paid for
until used), and `EcsEntity` adds thin entity-id <-> row convenience on top of `Ecs.stats`/
`Ecs.gas` - the row lookup is the one gotcha these wrap away (`EcsStats` addresses rows, not
entity ids; `EcsGas` takes entity ids directly):

```gdscript
var hero := EcsEntity.new()
var slime := EcsEntity.new()
add_child(hero); add_child(slime)

hero.add_stats({"vigor": 20, "mind": 10, "endurance": 15, "strength": 40})
slime.add_stats({"vigor": 5, "mind": 5, "endurance": 5, "strength": 5})
Ecs.stats.set_weapon(hero.stats_row(), {"base": PackedFloat32Array([25.0, 0, 0, 0, 0])}, {})

var result := hero.hit(slime, 1.0)   # Dictionary: dealt/overkill/killed/staggered/status_triggered
print(slime.pool_cur(EcsStats.POOL_HP), " / ", slime.pool_max(EcsStats.POOL_HP))

var burning := Ecs.ensure_gas().tag("burning")
slime.set_tags(burning)
var dot := Ecs.gas.add_effect({
	"name": "burn", "kind": EcsGas.EFFECT_DURATION, "duration": 3.0, "period": 0.5,
	"stat": EcsStats.COL_CUR + EcsStats.POOL_HP, "op": EcsGas.MOD_ADD, "magnitude": -2.0,
})
slime.apply_effect(dot, hero)
```

`EcsEntity._exit_tree()` also removes the entity's stats row if it has one - `EcsStats` is a
side table the native core doesn't know about, so `destroy_entity()` alone would otherwise
leave a stale row behind. `Ecs.events` (an `EcsEventBus`) is created eagerly rather than
lazily - cheap, and useful as plain pub/sub even in a game that uses neither stats nor gas;
`Ecs.ensure_stats()`/`ensure_gas()` both wire themselves to it automatically.
`EcsScheduler`/`EcsStreamer` don't have entity-level sugar (there's nothing per-entity to wrap) -
reach `Ecs.ensure_scheduler()`/`Ecs.ensure_streamer()` directly.

## Rules the native core expects you to follow

- **One world per game is normal.** `Ecs.world` is the shared instance everything above uses;
  spin up your own `EcsWorld.new()` only if a project genuinely needs an isolated simulation.
- **`column_bytes()` returns a copy.** Mutating it does nothing until you call
  `set_column_bytes()` with the whole column back - this is the one real gotcha found and fixed
  in this binding (the native `ecs_view_column()` pointer is meant for in-place writes; the
  Godot binding has to copy out to hand you a `PackedByteArray`, so the write-back is explicit).
- **Don't add/remove a component on any entity while a view that queries it is still open** -
  same rule as mutating an array mid-loop. Build a fresh view per `_process()` call (which
  `EcsSystem` already does) rather than holding one across frames.
- **Not thread-safe.** One `EcsWorld` is meant to be driven from one thread - Godot's main
  thread, for normal gameplay code.
- A pointer/row from `add_component`/`get_component` on `EcsWorld` itself is only valid until
  the next add/remove on *any* entity sharing that archetype (it can move rows around);
  `get_component_bytes`/`set_component_bytes` copy, so they're the safe default outside a
  system's bulk column loop.

## What's bound, and what isn't

Besides `EcsWorld`/`EcsView` (base archetype API: entities, components, tags, views), this
binding also exposes `EcsEventBus` (`ecs_event.h`), `EcsStats` (`ecs_stats.h`, souls-like combat
stats), `EcsGas` (`ecs_gas.h`, tags/effects/abilities) and `EcsScheduler` (`ecs_sched.h`,
phase/fixed-timestep scheduling), plus `EcsStreamer` (`ecs_stream.h`, distance-driven asset/cell
streaming). `ecs_dynamics.h` is deliberately not bound here - it stays native-only.

Relationships, hierarchies and change-detection are **not gaps in this binding** - they don't
exist in the native core yet either (see its own `ROADMAP.md` at `/data/dev/setup/base/holyC/ecs`,
under "not started"). Once they land natively, they'll need their own binding pass. Query caching
does exist natively (`ecs_query`) but isn't exposed through this binding yet - `EcsView` only
wraps the one-shot `ecs_view`.
