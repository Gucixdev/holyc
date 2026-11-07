@tool
class_name EcsSystem
extends Node

## Base system over the native ECS. Replaces the old EzSystem: queries are no longer
## get_tree().get_nodes_in_group() intersections (O(n) per group, per frame) - they walk
## the matching archetype tables directly via EcsView, table by table.
##
## Subclass, set `required_components` (an Array of EcsComponent scripts) in _init(),
## and override _process_batch() for the per-table bulk logic.

@export var active: bool = true
var required_components: Array = []
var _comp_ids := PackedInt32Array()

func _ready() -> void:
	_comp_ids = PackedInt32Array()
	for c in required_components:
		_comp_ids.append(Ecs.register(c))

func _process(delta: float) -> void:
	if not active or Engine.is_editor_hint():
		return
	var view: EcsView = Ecs.world.create_view(_comp_ids)
	while view.next_table():
		_process_batch(view, delta)

## Override: use view.entities() / view.column_bytes(comp_id) to iterate the current
## table's rows in bulk - column_bytes() is a copy, write mutations back with
## view.set_column_bytes(comp_id, data) - or call view.next() yourself for a
## one-entity-at-a-time style instead.
func _process_batch(_view: EcsView, _delta: float) -> void:
	pass
