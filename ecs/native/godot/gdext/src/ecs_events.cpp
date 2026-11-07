#include "ecs_events.h"

#include <cstring>

using namespace godot;

EcsEventBus::EcsEventBus() {
	_e = ecs_events_new();
}

EcsEventBus::~EcsEventBus() {
	for (auto &kv : _subs) delete kv.second;
	_subs.clear();
	if (_e) ecs_events_free(_e);
}

void EcsEventBus::_trampoline(const ecs_event *ev, void *user) {
	Sub *s = (Sub *)user;
	PackedByteArray data;
	if (ev->size && ev->data) {
		data.resize((int)ev->size);
		memcpy(data.ptrw(), ev->data, ev->size);
	}
	s->cb.call((int)ev->type, data);
}

void EcsEventBus::_bind_methods() {
	ClassDB::bind_method(D_METHOD("register_type", "name", "payload_size"), &EcsEventBus::register_type);
	ClassDB::bind_method(D_METHOD("event_type", "name"), &EcsEventBus::event_type);

	ClassDB::bind_method(D_METHOD("emit", "type", "payload"), &EcsEventBus::emit);
	ClassDB::bind_method(D_METHOD("emit_now", "type", "payload"), &EcsEventBus::emit_now);

	ClassDB::bind_method(D_METHOD("subscribe", "type", "callable"), &EcsEventBus::subscribe);
	ClassDB::bind_method(D_METHOD("unsubscribe", "sub"), &EcsEventBus::unsubscribe);

	ClassDB::bind_method(D_METHOD("dispatch"), &EcsEventBus::dispatch);
	ClassDB::bind_method(D_METHOD("set_max_waves", "waves"), &EcsEventBus::set_max_waves);
	ClassDB::bind_method(D_METHOD("pending"), &EcsEventBus::pending);

	ClassDB::bind_method(D_METHOD("count", "type"), &EcsEventBus::count);
	ClassDB::bind_method(D_METHOD("get_event", "type", "nth"), &EcsEventBus::get_event);

	ClassDB::bind_integer_constant(get_class_static(), "", "EV_DAMAGE", ECS_EV_DAMAGE);
	ClassDB::bind_integer_constant(get_class_static(), "", "EV_DEATH", ECS_EV_DEATH);
	ClassDB::bind_integer_constant(get_class_static(), "", "EV_STAGGER", ECS_EV_STAGGER);
	ClassDB::bind_integer_constant(get_class_static(), "", "EV_EXHAUSTED", ECS_EV_EXHAUSTED);
	ClassDB::bind_integer_constant(get_class_static(), "", "EV_STATUS_APPLIED", ECS_EV_STATUS_APPLIED);
	ClassDB::bind_integer_constant(get_class_static(), "", "EV_STATUS_ENDED", ECS_EV_STATUS_ENDED);
	ClassDB::bind_integer_constant(get_class_static(), "", "EV_EFFECT_APPLIED", ECS_EV_EFFECT_APPLIED);
	ClassDB::bind_integer_constant(get_class_static(), "", "EV_EFFECT_EXPIRED", ECS_EV_EFFECT_EXPIRED);
	ClassDB::bind_integer_constant(get_class_static(), "", "EV_ABILITY_ACTIVATED", ECS_EV_ABILITY_ACTIVATED);
	ClassDB::bind_integer_constant(get_class_static(), "", "EV_ABILITY_FAILED", ECS_EV_ABILITY_FAILED);
	ClassDB::bind_integer_constant(get_class_static(), "", "EV_BUILTIN_COUNT", ECS_EV_BUILTIN_COUNT);
	ClassDB::bind_integer_constant(get_class_static(), "", "EV_ANY", (int)ECS_EVENT_ANY);
}

int EcsEventBus::register_type(const String &name, int payload_size) {
	if (payload_size < 0) return -1;
	return ecs_event_register(_e, name.utf8().get_data(), (size_t)payload_size);
}
int EcsEventBus::event_type(const String &name) const {
	return ecs_event_type(_e, name.utf8().get_data());
}

bool EcsEventBus::emit(int type, PackedByteArray payload) {
	return ecs_event_emit(_e, (uint32_t)type, payload.size() ? payload.ptr() : nullptr);
}
void EcsEventBus::emit_now(int type, PackedByteArray payload) {
	ecs_event_emit_now(_e, (uint32_t)type, payload.size() ? payload.ptr() : nullptr);
}

int EcsEventBus::subscribe(int type, const Callable &cb) {
	Sub *s = new Sub{ this, cb };
	int id = ecs_event_subscribe(_e, (uint32_t)type, _trampoline, s);
	if (id == 0) {
		delete s;
		return 0;
	}
	_subs[id] = s;
	return id;
}
void EcsEventBus::unsubscribe(int sub) {
	auto it = _subs.find(sub);
	if (it == _subs.end()) return;
	ecs_event_unsubscribe(_e, sub);
	delete it->second;
	_subs.erase(it);
}

int EcsEventBus::dispatch() { return ecs_events_dispatch(_e); }
void EcsEventBus::set_max_waves(int waves) { ecs_events_set_max_waves(_e, waves); }
int EcsEventBus::pending() const { return ecs_events_pending(_e); }

int EcsEventBus::count(int type) const { return ecs_events_count(_e, (uint32_t)type); }

PackedByteArray EcsEventBus::get_event(int type, int nth) const {
	PackedByteArray out;
	ecs_event ev;
	if (!ecs_events_get(_e, (uint32_t)type, nth, &ev)) return out;
	if (ev.size && ev.data) {
		out.resize((int)ev.size);
		memcpy(out.ptrw(), ev.data, ev.size);
	}
	return out;
}
