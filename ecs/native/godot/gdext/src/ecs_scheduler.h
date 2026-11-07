#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/string.hpp>
#include <unordered_map>

extern "C" {
#include "ecs_sched.h"
}

namespace godot {

// Thin wrapper over ecs_sched (ecs_sched.h): phases, fixed timestep, conflict-batched system
// ordering. Systems are GDScript Callables (fn(dt: float)) bridged through a per-id
// trampoline, the same pattern EcsEventBus uses for its subscriptions. No parallel runner is
// bound here - ecs_sched_set_runner (bridging to Godot's WorkerThreadPool) is a deliberate,
// separate gap; everything registered here runs serially.
class EcsScheduler : public RefCounted {
	GDCLASS(EcsScheduler, RefCounted)

	struct Sys {
		Callable cb;
	};

	ecs_sched *_s = nullptr;
	std::unordered_map<int, Sys *> _systems;

	static void _trampoline(void *user, float dt);

protected:
	static void _bind_methods();

public:
	EcsScheduler(); // default config (fixed_dt 1/60, max_steps 5)
	~EcsScheduler();

	static Ref<EcsScheduler> create(float fixed_dt, int max_steps);

	int add_system(const String &name, int phase, const Callable &cb, int64_t reads, int64_t writes);
	bool order(int before, int after);
	void set_enabled(int id, bool enabled);
	bool build();

	void set_events(const Ref<RefCounted> &bus); // EcsEventBus; null detaches

	int update(float frame_dt);

	void set_time_scale(float scale);
	void set_paused(bool paused);
	void hit_stop(float seconds);

	int64_t fixed_ticks() const;
	int64_t frames() const;
	float alpha() const;
	float fixed_dt() const;

	int batch_count(int phase) const;
	int batch_of(int id) const;
};

} // namespace godot
