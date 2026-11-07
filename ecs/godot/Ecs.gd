extends Node

## Autoload singleton: the one shared native EcsWorld for the whole game, plus
## component-id caching by script so EcsEntity/EcsSystem don't re-resolve a
## name -> id lookup every call. One world per game is the normal ECS setup;
## add more EcsWorld.new() instances yourself if a project genuinely needs
## several isolated simulations.

var world: EcsWorld = EcsWorld.new()
var events: EcsEventBus = EcsEventBus.new()
var stats: EcsStats = null
var gas: EcsGas = null
var scheduler: EcsScheduler = null
var streamer: EcsStreamer = null
var _ids_by_script: Dictionary = {}

func register(component_script: Script) -> int:
	var cached = _ids_by_script.get(component_script)
	if cached != null:
		return cached
	var comp_name := component_script.get_global_name()
	if comp_name.is_empty():
		push_error("Ecs: component script must have a class_name")
		return -1
	var id := world.component_id(comp_name)
	if id < 0:
		id = world.register_component(comp_name, component_script.byte_size())
	_ids_by_script[component_script] = id
	return id

## -- Optional native modules, created on first use ---------------------------------------
## Not every game needs combat stats, abilities, a scheduler, or streaming - call the
## matching ensure_*() once (EcsEntity's stats/gas helpers do this for you) and the shared
## instance sticks around after. `events` itself is created eagerly since it's cheap and
## generically useful as plain pub/sub glue even without the other modules.

func ensure_stats(cfg: Dictionary = {}) -> EcsStats:
	if stats == null:
		stats = EcsStats.create(cfg, 0)
		stats.attach_events(events)
	return stats

func ensure_gas() -> EcsGas:
	if gas == null:
		gas = EcsGas.create(ensure_stats(), events)
	return gas

func ensure_scheduler(fixed_dt: float = 1.0 / 60.0, max_steps: int = 5) -> EcsScheduler:
	if scheduler == null:
		scheduler = EcsScheduler.create(fixed_dt, max_steps)
		scheduler.set_events(events)
	return scheduler

func ensure_streamer(cfg: Dictionary = {}) -> EcsStreamer:
	if streamer == null:
		streamer = EcsStreamer.create(cfg)
	return streamer
