#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>

extern "C" {
#include "ecs.h"
}

namespace godot {

// Wraps one ecs_view*. Created by EcsWorld::create_view, never directly - _bind() is called
// right after construction since GDREGISTER_CLASS requires a no-arg constructor.
class EcsView : public RefCounted {
	GDCLASS(EcsView, RefCounted)

	ecs_view *_v = nullptr;
	ecs_world *_w = nullptr; // needed for ecs_component_size in column_bytes

protected:
	static void _bind_methods();

public:
	EcsView() = default;
	~EcsView();

	void _bind(ecs_view *v, ecs_world *w);

	int64_t next();                 // next entity, or -1 once exhausted
	bool next_table();              // bulk iteration: advance to the next matching archetype
	int count() const;              // entities in the CURRENT table (bulk mode)
	PackedInt64Array entities() const;
	PackedByteArray column_bytes(int comp) const;
	void set_column_bytes(int comp, PackedByteArray data); // writes straight into the live table column (same memory column_bytes() copied from)
	int archetype_count() const;
};

} // namespace godot
