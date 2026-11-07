#include "ecs_scheduler.h"

#include "ecs_events.h"

using namespace godot;

EcsScheduler::EcsScheduler() {
	_s = ecs_sched_new(nullptr);
}

EcsScheduler::~EcsScheduler() {
	for (auto &kv : _systems) delete kv.second;
	_systems.clear();
	if (_s) ecs_sched_free(_s);
}

Ref<EcsScheduler> EcsScheduler::create(float fixed_dt, int max_steps) {
	Ref<EcsScheduler> r;
	r.instantiate();
	ecs_sched_config c{fixed_dt, max_steps};
	if (r->_s) ecs_sched_free(r->_s);
	r->_s = ecs_sched_new(&c);
	return r;
}

void EcsScheduler::_trampoline(void *user, float dt) {
	Sys *s = (Sys *)user;
	s->cb.call(dt);
}

void EcsScheduler::_bind_methods() {
	ClassDB::bind_static_method("EcsScheduler", D_METHOD("create", "fixed_dt", "max_steps"), &EcsScheduler::create);

	ClassDB::bind_method(D_METHOD("add_system", "name", "phase", "callable", "reads", "writes"), &EcsScheduler::add_system);
	ClassDB::bind_method(D_METHOD("order", "before", "after"), &EcsScheduler::order);
	ClassDB::bind_method(D_METHOD("set_enabled", "id", "enabled"), &EcsScheduler::set_enabled);
	ClassDB::bind_method(D_METHOD("build"), &EcsScheduler::build);

	ClassDB::bind_method(D_METHOD("set_events", "bus"), &EcsScheduler::set_events);

	ClassDB::bind_method(D_METHOD("update", "frame_dt"), &EcsScheduler::update);

	ClassDB::bind_method(D_METHOD("set_time_scale", "scale"), &EcsScheduler::set_time_scale);
	ClassDB::bind_method(D_METHOD("set_paused", "paused"), &EcsScheduler::set_paused);
	ClassDB::bind_method(D_METHOD("hit_stop", "seconds"), &EcsScheduler::hit_stop);

	ClassDB::bind_method(D_METHOD("fixed_ticks"), &EcsScheduler::fixed_ticks);
	ClassDB::bind_method(D_METHOD("frames"), &EcsScheduler::frames);
	ClassDB::bind_method(D_METHOD("alpha"), &EcsScheduler::alpha);
	ClassDB::bind_method(D_METHOD("fixed_dt"), &EcsScheduler::fixed_dt);

	ClassDB::bind_method(D_METHOD("batch_count", "phase"), &EcsScheduler::batch_count);
	ClassDB::bind_method(D_METHOD("batch_of", "id"), &EcsScheduler::batch_of);

	static const StringName cls = get_class_static();
	ClassDB::bind_integer_constant(cls, "", "PHASE_PRE_UPDATE", ECS_PHASE_PRE_UPDATE);
	ClassDB::bind_integer_constant(cls, "", "PHASE_FIXED_UPDATE", ECS_PHASE_FIXED_UPDATE);
	ClassDB::bind_integer_constant(cls, "", "PHASE_UPDATE", ECS_PHASE_UPDATE);
	ClassDB::bind_integer_constant(cls, "", "PHASE_POST_UPDATE", ECS_PHASE_POST_UPDATE);
	ClassDB::bind_integer_constant(cls, "", "PHASE_COUNT", ECS_PHASE_COUNT);
}

int EcsScheduler::add_system(const String &name, int phase, const Callable &cb, int64_t reads, int64_t writes) {
	Sys *s = new Sys{cb};
	int id = ecs_sched_add(_s, name.utf8().get_data(), (ecs_phase)phase, _trampoline, s, (uint64_t)reads, (uint64_t)writes);
	if (id < 0) {
		delete s;
		return -1;
	}
	_systems[id] = s;
	return id;
}
bool EcsScheduler::order(int before, int after) { return ecs_sched_order(_s, before, after); }
void EcsScheduler::set_enabled(int id, bool enabled) { ecs_sched_set_enabled(_s, id, enabled); }
bool EcsScheduler::build() { return ecs_sched_build(_s); }

void EcsScheduler::set_events(const Ref<RefCounted> &bus) {
	EcsEventBus *b = bus.is_valid() ? Object::cast_to<EcsEventBus>(bus.ptr()) : nullptr;
	ecs_sched_set_events(_s, b ? b->raw() : nullptr);
}

int EcsScheduler::update(float frame_dt) { return ecs_sched_update(_s, frame_dt); }

void EcsScheduler::set_time_scale(float scale) { ecs_sched_set_time_scale(_s, scale); }
void EcsScheduler::set_paused(bool paused) { ecs_sched_set_paused(_s, paused); }
void EcsScheduler::hit_stop(float seconds) { ecs_sched_hit_stop(_s, seconds); }

int64_t EcsScheduler::fixed_ticks() const { return (int64_t)ecs_sched_fixed_ticks(_s); }
int64_t EcsScheduler::frames() const { return (int64_t)ecs_sched_frames(_s); }
float EcsScheduler::alpha() const { return ecs_sched_alpha(_s); }
float EcsScheduler::fixed_dt() const { return ecs_sched_fixed_dt(_s); }

int EcsScheduler::batch_count(int phase) const { return ecs_sched_batch_count(_s, (ecs_phase)phase); }
int EcsScheduler::batch_of(int id) const { return ecs_sched_batch_of(_s, id); }
