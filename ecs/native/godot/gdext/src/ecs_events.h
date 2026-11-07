#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <unordered_map>

extern "C" {
#include "ecs_event.h"
}

namespace godot {

// Thin wrapper over ecs_events (ecs_event.h): a typed event bus. Builtin event types are the
// EV_* integer constants (payloads are decoded/encoded as PackedByteArray, the same convention
// EcsWorld uses for component data - see each builtin struct's layout in ecs_event.h). Custom
// types registered via register_type() carry whatever payload the caller chooses.
class EcsEventBus : public RefCounted {
	GDCLASS(EcsEventBus, RefCounted)

	struct Sub {
		EcsEventBus *self;
		Callable cb;
	};

	ecs_events *_e = nullptr;
	std::unordered_map<int, Sub *> _subs;

	static void _trampoline(const ecs_event *ev, void *user);

protected:
	static void _bind_methods();

public:
	EcsEventBus();
	~EcsEventBus();

	int register_type(const String &name, int payload_size); // >= EV_BUILTIN_COUNT, -1 on failure
	int event_type(const String &name) const;                // -1 if unknown (builtins resolve too)

	bool emit(int type, PackedByteArray payload);
	void emit_now(int type, PackedByteArray payload);

	int subscribe(int type, const Callable &cb); // cb.call(type: int, payload: PackedByteArray)
	void unsubscribe(int sub);

	int dispatch();
	void set_max_waves(int waves);
	int pending() const;

	int count(int type) const;
	PackedByteArray get_event(int type, int nth) const; // empty if out of range

	ecs_events *raw() const { return _e; } // for EcsStats::attach_events / EcsGas::new / EcsScheduler::set_events
};

} // namespace godot
