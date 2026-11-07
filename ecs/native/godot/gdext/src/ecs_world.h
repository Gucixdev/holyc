#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include "ecs_view.h"

extern "C" {
#include "ecs.h"
}

namespace godot {

// Thin GDExtension wrapper over the plain-C99 archetype ECS (ecs.h / libecs). One EcsWorld
// per ecs_world - this is the real storage (archetype tables, contiguous component columns),
// not the old GDScript ezECS facade (Node + SceneTree groups, O(n) per query).
class EcsWorld : public RefCounted {
	GDCLASS(EcsWorld, RefCounted)

	ecs_world *_w = nullptr;

protected:
	static void _bind_methods();

public:
	EcsWorld();
	~EcsWorld();

	int64_t create_entity();
	void destroy_entity(int64_t e);
	bool is_alive(int64_t e) const;
	int entity_count() const;

	int register_component(const String &name, int size); // size 0 = tag; -1 if name taken
	int register_tag(const String &name);
	int component_id(const String &name) const;            // -1 if unknown
	String component_name(int comp) const;
	int component_size(int comp) const;
	int type_count() const;
	int type_count_entities(int comp) const;

	bool add_component(int64_t e, int comp);
	void remove_component(int64_t e, int comp);
	bool has_component(int64_t e, int comp) const;
	PackedByteArray get_component_bytes(int64_t e, int comp) const; // empty if absent or a tag
	bool set_component_bytes(int64_t e, int comp, PackedByteArray data);

	Ref<EcsView> create_view(PackedInt32Array comps); // AND of every comp id in `comps`

	ecs_world *raw() const { return _w; } // for EcsStats::collect
};

} // namespace godot
