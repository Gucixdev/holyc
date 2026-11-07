#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>

#include "ecs_world.h"

extern "C" {
#include "ecs_stats.h"
}

namespace godot {

// Thin wrapper over ecs_stats (ecs_stats.h): the souls-like combat stats side table (SoA
// pools/buildup/defense/attack columns, ticked in bulk). Structs (weapon/attributes/attack/
// hit_result/config) cross the boundary as Dictionary - see ecs_stats.cpp for the field names,
// matching the C struct fields 1:1. Rows are addressed by plain int, as the native API does.
class EcsStats : public RefCounted {
	GDCLASS(EcsStats, RefCounted)

	ecs_stats *_s = nullptr;

protected:
	static void _bind_methods();

public:
	EcsStats(); // defaults config, capacity hint 0
	~EcsStats();

	static Ref<EcsStats> create(Dictionary cfg, int capacity_hint);

	int add(int64_t e);
	void remove(int64_t e);
	int row(int64_t e) const;
	int count() const;
	PackedInt64Array entities() const;
	int collect(const Ref<EcsWorld> &world);

	PackedFloat32Array col(int col) const; // copy of ecs_stats_count() floats
	PackedInt32Array flags() const;        // copy, one uint32 bitmask per row (as int32 bits)

	void tick(float dt);

	float damage(int row, PackedFloat32Array amounts, float poise_damage);
	float damage_preview(int row, PackedFloat32Array amounts) const;
	bool spend(int row, int pool, float amount);
	void restore(int row, int pool, float amount);
	void reset_poise(int row);
	bool add_buildup(int row, int type, float amount);

	float scaling(int level) const;
	void set_weapon(int row, Dictionary weapon, Dictionary attributes);
	Dictionary make_attack(int row, float motion) const;
	Dictionary apply_attack(int target_row, Dictionary attack);
	Dictionary hit(int attacker_row, int target_row, float motion);
	float apply_delta(int row, int pool, float delta, int64_t source);
	void set_crit_mult(float mult);

	void attach_events(const Ref<RefCounted> &bus); // EcsEventBus; NULL/invalid detaches

	int64_t tags(int row) const;      // effective tags (base | granted), as a 64-bit mask
	int64_t tags_base(int row) const; // permanent tags
	void set_tags_base(int row, int64_t mask);
	void set_exhaust_mult(int pool, float mult);
	Dictionary get_config() const;

	void set_load(int row, float carried, float capacity);
	float load_ratio(int row) const;
	int roll_class(int row) const;
	void derive(int row, Dictionary attributes);

	ecs_stats *raw() const { return _s; } // for EcsGas::new
};

} // namespace godot
