#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>

extern "C" {
#include "ecs_stream.h"
}

namespace godot {

// Thin wrapper over ecs_streamer (ecs_stream.h): distance-driven asset/cell streaming policy
// (what to load/unload, in what order, within a budget) - no I/O of its own, the host (here,
// GDScript via ResourceLoader.load_threaded_request) does the actual loading.
class EcsStreamer : public RefCounted {
	GDCLASS(EcsStreamer, RefCounted)

	ecs_streamer *_s = nullptr;

protected:
	static void _bind_methods();

public:
	EcsStreamer(); // default config
	~EcsStreamer();

	static Ref<EcsStreamer> create(Dictionary cfg);

	int64_t asset_add(int64_t bytes, int user);
	bool cell_add(int cx, int cz, int64_t asset, float max_dist);
	int cell_of(float world_coord) const;

	// Each element of the returned Array is a Dictionary: {kind:int, asset:int64, user:int, distance:float}.
	Array tick(PackedFloat32Array obs_x, PackedFloat32Array obs_z, int cap);

	void asset_loaded(int64_t a);
	void asset_failed(int64_t a);
	void asset_unloaded(int64_t a);

	// Each element is a Dictionary: {kind:int, cx:int, cz:int}.
	Array poll(int max);

	int asset_state(int64_t a) const;
	Dictionary get_stats() const;
};

} // namespace godot
