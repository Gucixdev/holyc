#include "ecs_world.h"

#include <cstring>

using namespace godot;

EcsWorld::EcsWorld() {
	_w = ecs_world_new();
}

EcsWorld::~EcsWorld() {
	if (_w) ecs_world_free(_w);
}

void EcsWorld::_bind_methods() {
	ClassDB::bind_method(D_METHOD("create_entity"), &EcsWorld::create_entity);
	ClassDB::bind_method(D_METHOD("destroy_entity", "entity"), &EcsWorld::destroy_entity);
	ClassDB::bind_method(D_METHOD("is_alive", "entity"), &EcsWorld::is_alive);
	ClassDB::bind_method(D_METHOD("entity_count"), &EcsWorld::entity_count);

	ClassDB::bind_method(D_METHOD("register_component", "name", "size"), &EcsWorld::register_component);
	ClassDB::bind_method(D_METHOD("register_tag", "name"), &EcsWorld::register_tag);
	ClassDB::bind_method(D_METHOD("component_id", "name"), &EcsWorld::component_id);
	ClassDB::bind_method(D_METHOD("component_name", "comp"), &EcsWorld::component_name);
	ClassDB::bind_method(D_METHOD("component_size", "comp"), &EcsWorld::component_size);
	ClassDB::bind_method(D_METHOD("type_count"), &EcsWorld::type_count);
	ClassDB::bind_method(D_METHOD("type_count_entities", "comp"), &EcsWorld::type_count_entities);

	ClassDB::bind_method(D_METHOD("add_component", "entity", "comp"), &EcsWorld::add_component);
	ClassDB::bind_method(D_METHOD("remove_component", "entity", "comp"), &EcsWorld::remove_component);
	ClassDB::bind_method(D_METHOD("has_component", "entity", "comp"), &EcsWorld::has_component);
	ClassDB::bind_method(D_METHOD("get_component_bytes", "entity", "comp"), &EcsWorld::get_component_bytes);
	ClassDB::bind_method(D_METHOD("set_component_bytes", "entity", "comp", "data"), &EcsWorld::set_component_bytes);

	ClassDB::bind_method(D_METHOD("create_view", "comps"), &EcsWorld::create_view);
}

int64_t EcsWorld::create_entity() { return (int64_t)ecs_create(_w); }
void EcsWorld::destroy_entity(int64_t e) { ecs_destroy(_w, (ecs_entity)e); }
bool EcsWorld::is_alive(int64_t e) const { return ecs_alive(_w, (ecs_entity)e); }
int EcsWorld::entity_count() const { return ecs_entity_count(_w); }

int EcsWorld::register_component(const String &name, int size) {
	if (size < 0) return -1;
	return ecs_register(_w, name.utf8().get_data(), (size_t)size);
}
int EcsWorld::register_tag(const String &name) {
	return ecs_register_tag(_w, name.utf8().get_data());
}
int EcsWorld::component_id(const String &name) const {
	return ecs_component_id(_w, name.utf8().get_data());
}
String EcsWorld::component_name(int comp) const {
	const char *n = ecs_component_name(_w, comp);
	return n ? String(n) : String();
}
int EcsWorld::component_size(int comp) const {
	return (int)ecs_component_size(_w, comp);
}
int EcsWorld::type_count() const { return ecs_type_count(_w); }
int EcsWorld::type_count_entities(int comp) const { return ecs_type_count_entities(_w, comp); }

bool EcsWorld::add_component(int64_t e, int comp) {
	return ecs_add(_w, (ecs_entity)e, comp) != nullptr;
}
void EcsWorld::remove_component(int64_t e, int comp) {
	ecs_remove(_w, (ecs_entity)e, comp);
}
bool EcsWorld::has_component(int64_t e, int comp) const {
	return ecs_has(_w, (ecs_entity)e, comp);
}

PackedByteArray EcsWorld::get_component_bytes(int64_t e, int comp) const {
	PackedByteArray out;
	size_t size = ecs_component_size(_w, comp);
	if (size == 0) return out; // tag - no data
	void *p = ecs_get(_w, (ecs_entity)e, comp);
	if (!p) return out;
	out.resize((int)size);
	memcpy(out.ptrw(), p, size);
	return out;
}

bool EcsWorld::set_component_bytes(int64_t e, int comp, PackedByteArray data) {
	size_t size = ecs_component_size(_w, comp);
	if (size == 0 || (size_t)data.size() != size) return false;
	void *p = ecs_add(_w, (ecs_entity)e, comp); // ensures it exists, then we fill it
	if (!p) return false;
	memcpy(p, data.ptr(), size);
	return true;
}

Ref<EcsView> EcsWorld::create_view(PackedInt32Array comps) {
	Ref<EcsView> v;
	v.instantiate();
	ecs_view *raw_v = ecs_view_new(_w, comps.ptr(), comps.size());
	v->_bind(raw_v, _w);
	return v;
}
