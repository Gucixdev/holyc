#include "register_types.h"

#include <gdextension_interface.h>

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

#include "ecs_events.h"
#include "ecs_gas_wrap.h"
#include "ecs_scheduler.h"
#include "ecs_stats_wrap.h"
#include "ecs_streamer.h"
#include "ecs_view.h"
#include "ecs_world.h"

using namespace godot;

void initialize_ecs_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	GDREGISTER_CLASS(EcsView);
	GDREGISTER_CLASS(EcsWorld);
	GDREGISTER_CLASS(EcsEventBus);
	GDREGISTER_CLASS(EcsStats);
	GDREGISTER_CLASS(EcsGas);
	GDREGISTER_CLASS(EcsScheduler);
	GDREGISTER_CLASS(EcsStreamer);
}

void uninitialize_ecs_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
}

extern "C" {
GDExtensionBool GDE_EXPORT ecs_library_init(GDExtensionInterfaceGetProcAddress p_get_proc_address, GDExtensionClassLibraryPtr p_library, GDExtensionInitialization *r_initialization) {
	::godot::GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);

	init_obj.register_initializer(initialize_ecs_module);
	init_obj.register_terminator(uninitialize_ecs_module);
	init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);

	return init_obj.init();
}
}
