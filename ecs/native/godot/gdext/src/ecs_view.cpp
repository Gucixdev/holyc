#include "ecs_view.h"

#include <cstring>

using namespace godot;

EcsView::~EcsView() {
	if (_v) ecs_view_free(_v);
}

void EcsView::_bind(ecs_view *v, ecs_world *w) {
	_v = v;
	_w = w;
}

void EcsView::_bind_methods() {
	ClassDB::bind_method(D_METHOD("next"), &EcsView::next);
	ClassDB::bind_method(D_METHOD("next_table"), &EcsView::next_table);
	ClassDB::bind_method(D_METHOD("count"), &EcsView::count);
	ClassDB::bind_method(D_METHOD("entities"), &EcsView::entities);
	ClassDB::bind_method(D_METHOD("column_bytes", "comp"), &EcsView::column_bytes);
	ClassDB::bind_method(D_METHOD("set_column_bytes", "comp", "data"), &EcsView::set_column_bytes);
	ClassDB::bind_method(D_METHOD("archetype_count"), &EcsView::archetype_count);
}

int64_t EcsView::next() {
	ecs_entity e;
	if (_v && ecs_view_next(_v, &e)) return (int64_t)e;
	return -1;
}

bool EcsView::next_table() {
	return _v && ecs_view_next_table(_v);
}

int EcsView::count() const {
	return _v ? ecs_view_count(_v) : 0;
}

PackedInt64Array EcsView::entities() const {
	PackedInt64Array out;
	if (!_v) return out;
	int n = ecs_view_count(_v);
	const ecs_entity *es = ecs_view_entities(_v);
	if (!es || n <= 0) return out;
	out.resize(n);
	for (int i = 0; i < n; i++) out[i] = (int64_t)es[i];
	return out;
}

PackedByteArray EcsView::column_bytes(int comp) const {
	PackedByteArray out;
	if (!_v || !_w) return out;
	size_t size = ecs_component_size(_w, comp);
	int n = ecs_view_count(_v);
	void *col = ecs_view_column(_v, comp);
	if (!col || size == 0 || n <= 0) return out;
	out.resize((int)(size * (size_t)n));
	memcpy(out.ptrw(), col, size * (size_t)n);
	return out;
}

void EcsView::set_column_bytes(int comp, PackedByteArray data) {
	if (!_v || !_w) return;
	size_t size = ecs_component_size(_w, comp);
	int n = ecs_view_count(_v);
	void *col = ecs_view_column(_v, comp);
	if (!col || size == 0 || n <= 0) return;
	if ((size_t)data.size() != size * (size_t)n) return; // must match column_bytes()'s own shape exactly
	memcpy(col, data.ptr(), size * (size_t)n);
}

int EcsView::archetype_count() const {
	return _v ? ecs_view_archetype_count(_v) : 0;
}
