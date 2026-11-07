@tool
extends EditorPlugin

## Registers the autoload `Ecs` (the shared EcsWorld) - same self-contained pattern as
## ezsave's `Saves`, so dropping this tool folder in is enough, no manual project.godot edit.
const AUTOLOAD_NAME := "Ecs"
const SCRIPT := "res://addons/toolbox/tools/ecs/Ecs.gd"

func _enter_tree() -> void:
	if not ProjectSettings.has_setting("autoload/" + AUTOLOAD_NAME):
		add_autoload_singleton(AUTOLOAD_NAME, SCRIPT)

func _exit_tree() -> void:
	if ProjectSettings.has_setting("autoload/" + AUTOLOAD_NAME):
		remove_autoload_singleton(AUTOLOAD_NAME)
