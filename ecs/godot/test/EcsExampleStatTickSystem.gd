@tool
class_name EcsExampleStatTickSystem
extends EcsSystem

## Generic regen/decay tick for one EcsExampleStatComponent subclass. Set `stat_script`
## in the inspector (EcsExampleHp/Mp/Sp, or your own) - drop one instance of this system
## per stat into the scene. Each tick: current += (regen_per_sec - decay_per_sec) * delta,
## clamped to [0, max] - set either rate to 0 on the component if a stat only regens or
## only decays.

@export var stat_script: Script

func _ready() -> void:
	required_components = [stat_script] if stat_script else []
	super._ready()

func _process_batch(view: EcsView, delta: float) -> void:
	var comp_id := Ecs.register(stat_script)
	var col := view.column_bytes(comp_id) # a copy - mutate it, then write it back
	for i in view.count():
		var off := i * 16
		var cur := col.decode_float(off)
		var max_v := col.decode_float(off + 4)
		var regen := col.decode_float(off + 8)
		var decay := col.decode_float(off + 12)
		cur = clampf(cur + (regen - decay) * delta, 0.0, max_v)
		col.encode_float(off, cur)
	view.set_column_bytes(comp_id, col)
