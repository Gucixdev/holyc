#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

#include "ecs_stats_wrap.h"

extern "C" {
#include "ecs_gas.h"
}

namespace godot {

// Thin wrapper over ecs_gas (ecs_gas.h): tags + effects + abilities, driven off an EcsStats
// table. Effect/ability definitions cross the boundary as Dictionary - see ecs_gas_wrap.cpp
// for the field names, matching ecs_effect_def/ecs_ability_def 1:1. Must be built with
// EcsGas.create(stats, events) - there is no standalone default state, since every ecs_gas
// needs a backing EcsStats table.
class EcsGas : public RefCounted {
	GDCLASS(EcsGas, RefCounted)

	ecs_gas *_g = nullptr;

protected:
	static void _bind_methods();

public:
	EcsGas() {}
	~EcsGas();

	static Ref<EcsGas> create(const Ref<EcsStats> &stats, const Ref<RefCounted> &events);

	int64_t tag(const String &name);
	bool has_tags(int64_t e, int64_t all) const;
	void set_tags(int64_t e, int64_t tags);

	int add_effect(Dictionary def);
	bool apply(int64_t target, int effect, int64_t source);
	int remove(int64_t target, int effect);
	int cancel_tags(int64_t target, int64_t tags);
	int stacks(int64_t target, int effect) const;
	int active_count() const;

	int add_ability(Dictionary def);
	int can_activate(int64_t user, int ability) const;
	int activate(int64_t user, int ability, int64_t target);

	void tick(float dt);
};

} // namespace godot
