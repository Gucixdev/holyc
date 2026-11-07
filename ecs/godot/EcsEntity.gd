@tool
class_name EcsEntity
extends Node

## Scene-tree handle for one native-ECS entity (see Ecs.gd / EcsComponent.gd). Replaces
## the old EzEntity: component DATA no longer lives on this node (it's a contiguous
## column inside the shared Ecs.world), this is just the id plus node-lifecycle glue so
## you can still drop it under a CharacterBody3D/Node3D etc. like before.

var id: int = -1

func _ready() -> void:
	if Engine.is_editor_hint():
		return
	id = Ecs.world.create_entity()

func _exit_tree() -> void:
	if Engine.is_editor_hint() or id < 0:
		return
	# Stats is a side table the native core doesn't know about - destroy_entity() won't
	# touch it, so a row would otherwise outlive its entity.
	if Ecs.stats != null and Ecs.stats.row(id) >= 0:
		Ecs.stats.remove(id)
	Ecs.world.destroy_entity(id)
	id = -1

func add_component(component_script: Script, data = null) -> void:
	var comp_id := Ecs.register(component_script)
	Ecs.world.add_component(id, comp_id)
	if data != null and component_script.byte_size() > 0:
		Ecs.world.set_component_bytes(id, comp_id, component_script.to_bytes(data))

func remove_component(component_script: Script) -> void:
	Ecs.world.remove_component(id, Ecs.register(component_script))

func has_component(component_script: Script) -> bool:
	return Ecs.world.has_component(id, Ecs.register(component_script))

## Tag (byte_size() == 0) -> true/null. Data component -> whatever from_bytes() returns, or null if absent.
func get_component(component_script: Script):
	var comp_id := Ecs.register(component_script)
	if not Ecs.world.has_component(id, comp_id):
		return null
	if component_script.byte_size() == 0:
		return true
	return component_script.from_bytes(Ecs.world.get_component_bytes(id, comp_id))

func set_component(component_script: Script, data) -> void:
	Ecs.world.set_component_bytes(id, Ecs.register(component_script), component_script.to_bytes(data))

## -- Stats (ecs_stats.h via Ecs.stats): souls-like combat pools/buildup/defense/attack ---
## Rows, not entity ids, are what EcsStats actually takes - these wrap that lookup so
## callers only ever deal in entities.

func add_stats(attributes: Dictionary = {}) -> int:
	var row := Ecs.ensure_stats().add(id)
	if not attributes.is_empty():
		Ecs.stats.derive(row, attributes)
	return row

func has_stats() -> bool:
	return Ecs.stats != null and Ecs.stats.row(id) >= 0

func stats_row() -> int:
	return Ecs.stats.row(id) if Ecs.stats != null else -1

## cur/max pool value, e.g. `pool_cur(EcsStats.POOL_HP)`.
func pool_cur(pool_type: int) -> float:
	return Ecs.stats.col(EcsStats.COL_CUR + pool_type)[stats_row()]

func pool_max(pool_type: int) -> float:
	return Ecs.stats.col(EcsStats.COL_MAX + pool_type)[stats_row()]

func hit(target: EcsEntity, motion: float = 1.0) -> Dictionary:
	return Ecs.stats.hit(stats_row(), target.stats_row(), motion)

## -- Gas (ecs_gas.h via Ecs.gas): tags/effects/abilities, driven off this entity's stats row -

func has_tags(mask: int) -> bool:
	return Ecs.gas != null and Ecs.gas.has_tags(id, mask)

func set_tags(mask: int) -> void:
	Ecs.ensure_gas().set_tags(id, mask)

func apply_effect(effect_id: int, source: EcsEntity = null) -> bool:
	return Ecs.ensure_gas().apply(id, effect_id, (source if source else self).id)

func remove_effect(effect_id: int) -> int:
	return Ecs.gas.remove(id, effect_id) if Ecs.gas != null else 0

func can_activate(ability_id: int) -> int:
	return Ecs.gas.can_activate(id, ability_id) if Ecs.gas != null else EcsGas.ABILITY_BAD_ENTITY

func activate(ability_id: int, target: EcsEntity = null) -> int:
	return Ecs.ensure_gas().activate(id, ability_id, (target if target else self).id)
