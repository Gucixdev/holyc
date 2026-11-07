#include "ecs_streamer.h"

#include <vector>

using namespace godot;

static ecs_stream_config config_from_dict(const Dictionary &d) {
	ecs_stream_config c = ecs_stream_config_default();
	if (d.has("cell_size")) c.cell_size = d["cell_size"];
	if (d.has("load_radius")) c.load_radius = d["load_radius"];
	if (d.has("unload_radius")) c.unload_radius = d["unload_radius"];
	if (d.has("budget_bytes")) c.budget_bytes = (uint64_t)(int64_t)d["budget_bytes"];
	if (d.has("max_requests_per_tick")) c.max_requests_per_tick = (int)d["max_requests_per_tick"];
	if (d.has("max_in_flight")) c.max_in_flight = (int)d["max_in_flight"];
	if (d.has("unload_grace_ticks")) c.unload_grace_ticks = (int)d["unload_grace_ticks"];
	if (d.has("retry_ticks")) c.retry_ticks = (int)d["retry_ticks"];
	return c;
}

EcsStreamer::EcsStreamer() {
	_s = ecs_stream_new(nullptr);
}

EcsStreamer::~EcsStreamer() {
	if (_s) ecs_stream_free(_s);
}

Ref<EcsStreamer> EcsStreamer::create(Dictionary cfg) {
	Ref<EcsStreamer> r;
	r.instantiate();
	ecs_stream_config c = config_from_dict(cfg);
	if (r->_s) ecs_stream_free(r->_s);
	r->_s = ecs_stream_new(&c);
	return r;
}

void EcsStreamer::_bind_methods() {
	ClassDB::bind_static_method("EcsStreamer", D_METHOD("create", "cfg"), &EcsStreamer::create);

	ClassDB::bind_method(D_METHOD("asset_add", "bytes", "user"), &EcsStreamer::asset_add);
	ClassDB::bind_method(D_METHOD("cell_add", "cx", "cz", "asset", "max_dist"), &EcsStreamer::cell_add);
	ClassDB::bind_method(D_METHOD("cell_of", "world_coord"), &EcsStreamer::cell_of);

	ClassDB::bind_method(D_METHOD("tick", "obs_x", "obs_z", "cap"), &EcsStreamer::tick);

	ClassDB::bind_method(D_METHOD("asset_loaded", "asset"), &EcsStreamer::asset_loaded);
	ClassDB::bind_method(D_METHOD("asset_failed", "asset"), &EcsStreamer::asset_failed);
	ClassDB::bind_method(D_METHOD("asset_unloaded", "asset"), &EcsStreamer::asset_unloaded);

	ClassDB::bind_method(D_METHOD("poll", "max"), &EcsStreamer::poll);

	ClassDB::bind_method(D_METHOD("asset_state", "asset"), &EcsStreamer::asset_state);
	ClassDB::bind_method(D_METHOD("get_stats"), &EcsStreamer::get_stats);

	static const StringName cls = get_class_static();
	ClassDB::bind_integer_constant(cls, "", "ASSET_UNLOADED", ECS_ASSET_UNLOADED);
	ClassDB::bind_integer_constant(cls, "", "ASSET_LOADING", ECS_ASSET_LOADING);
	ClassDB::bind_integer_constant(cls, "", "ASSET_READY", ECS_ASSET_READY);
	ClassDB::bind_integer_constant(cls, "", "ASSET_UNLOADING", ECS_ASSET_UNLOADING);
	ClassDB::bind_integer_constant(cls, "", "ASSET_FAILED", ECS_ASSET_FAILED);

	ClassDB::bind_integer_constant(cls, "", "STREAM_LOAD", ECS_STREAM_LOAD);
	ClassDB::bind_integer_constant(cls, "", "STREAM_UNLOAD", ECS_STREAM_UNLOAD);

	ClassDB::bind_integer_constant(cls, "", "STREAM_CELL_READY", ECS_STREAM_CELL_READY);
	ClassDB::bind_integer_constant(cls, "", "STREAM_CELL_RELEASED", ECS_STREAM_CELL_RELEASED);
}

int64_t EcsStreamer::asset_add(int64_t bytes, int user) {
	return (int64_t)ecs_stream_asset_add(_s, (uint64_t)bytes, (uint32_t)user);
}
bool EcsStreamer::cell_add(int cx, int cz, int64_t asset, float max_dist) {
	return ecs_stream_cell_add(_s, cx, cz, (ecs_asset_id)asset, max_dist);
}
int EcsStreamer::cell_of(float world_coord) const { return ecs_stream_cell_of(_s, world_coord); }

Array EcsStreamer::tick(PackedFloat32Array obs_x, PackedFloat32Array obs_z, int cap) {
	Array out;
	int n_obs = obs_x.size() < obs_z.size() ? obs_x.size() : obs_z.size();
	if (cap <= 0) return out;
	std::vector<ecs_stream_request> reqs(cap);
	int n = ecs_stream_tick(_s, obs_x.ptr(), obs_z.ptr(), n_obs, reqs.data(), cap);
	for (int i = 0; i < n; i++) {
		Dictionary d;
		d["kind"] = (int)reqs[i].kind;
		d["asset"] = (int64_t)reqs[i].asset;
		d["user"] = (int)reqs[i].user;
		d["distance"] = reqs[i].distance;
		out.push_back(d);
	}
	return out;
}

void EcsStreamer::asset_loaded(int64_t a) { ecs_stream_asset_loaded(_s, (ecs_asset_id)a); }
void EcsStreamer::asset_failed(int64_t a) { ecs_stream_asset_failed(_s, (ecs_asset_id)a); }
void EcsStreamer::asset_unloaded(int64_t a) { ecs_stream_asset_unloaded(_s, (ecs_asset_id)a); }

Array EcsStreamer::poll(int max) {
	Array out;
	if (max <= 0) return out;
	std::vector<ecs_stream_event> evs(max);
	int n = ecs_stream_poll(_s, evs.data(), max);
	for (int i = 0; i < n; i++) {
		Dictionary d;
		d["kind"] = (int)evs[i].kind;
		d["cx"] = evs[i].cx;
		d["cz"] = evs[i].cz;
		out.push_back(d);
	}
	return out;
}

int EcsStreamer::asset_state(int64_t a) const { return (int)ecs_stream_asset_state(_s, (ecs_asset_id)a); }

Dictionary EcsStreamer::get_stats() const {
	ecs_stream_stats st = ecs_stream_get_stats(_s);
	Dictionary d;
	d["tick"] = (int)st.tick;
	d["resident_bytes"] = (int64_t)st.resident_bytes;
	d["loading"] = st.loading;
	d["ready"] = st.ready;
	d["unloading"] = st.unloading;
	d["failed"] = st.failed;
	d["wanted"] = st.wanted;
	d["cells_ready"] = st.cells_ready;
	return d;
}
