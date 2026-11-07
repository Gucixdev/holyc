#include "ecs_gas_wrap.h"

#include "ecs_events.h"

using namespace godot;

static ecs_effect_def effect_from_dict(const Dictionary &d) {
	ecs_effect_def def{};
	static thread_local CharString name_storage;
	name_storage = d.has("name") ? String(d["name"]).utf8() : CharString();
	def.name = name_storage.get_data();
	def.kind = d.has("kind") ? (ecs_effect_kind)(int)d["kind"] : ECS_EFFECT_INSTANT;
	def.duration = d.has("duration") ? (float)d["duration"] : 0.0f;
	def.period = d.has("period") ? (float)d["period"] : 0.0f;
	def.tick_on_apply = d.has("tick_on_apply") ? (bool)d["tick_on_apply"] : false;
	def.stat = d.has("stat") ? (ecs_stat_col)(int)d["stat"] : ECS_NO_STAT;
	def.op = d.has("op") ? (ecs_mod_op)(int)d["op"] : ECS_MOD_ADD;
	def.magnitude = d.has("magnitude") ? (float)d["magnitude"] : 0.0f;
	def.stacking = d.has("stacking") ? (ecs_stack_policy)(int)d["stacking"] : ECS_STACK_NONE;
	def.max_stacks = d.has("max_stacks") ? (int)d["max_stacks"] : 1;
	def.grants_tags = d.has("grants_tags") ? (uint64_t)(int64_t)d["grants_tags"] : 0;
	def.requires_tags = d.has("requires_tags") ? (uint64_t)(int64_t)d["requires_tags"] : 0;
	def.blocked_by_tags = d.has("blocked_by_tags") ? (uint64_t)(int64_t)d["blocked_by_tags"] : 0;
	def.cancels_tags = d.has("cancels_tags") ? (uint64_t)(int64_t)d["cancels_tags"] : 0;
	return def;
}

static ecs_ability_def ability_from_dict(const Dictionary &d) {
	ecs_ability_def def{};
	static thread_local CharString name_storage;
	name_storage = d.has("name") ? String(d["name"]).utf8() : CharString();
	def.name = name_storage.get_data();
	def.cost_pool = d.has("cost_pool") ? (ecs_pool)(int)d["cost_pool"] : ECS_POOL_SP;
	def.cost = d.has("cost") ? (float)d["cost"] : 0.0f;
	def.cooldown = d.has("cooldown") ? (float)d["cooldown"] : 0.0f;
	def.required_tags = d.has("required_tags") ? (uint64_t)(int64_t)d["required_tags"] : 0;
	def.blocked_tags = d.has("blocked_tags") ? (uint64_t)(int64_t)d["blocked_tags"] : 0;
	def.effect_self = d.has("effect_self") ? (int)d["effect_self"] : -1;
	def.effect_target = d.has("effect_target") ? (int)d["effect_target"] : -1;
	return def;
}

EcsGas::~EcsGas() {
	if (_g) ecs_gas_free(_g);
}

Ref<EcsGas> EcsGas::create(const Ref<EcsStats> &stats, const Ref<RefCounted> &events) {
	Ref<EcsGas> r;
	r.instantiate();
	if (stats.is_null()) return r;
	ecs_events *ev = nullptr;
	if (events.is_valid()) {
		EcsEventBus *b = Object::cast_to<EcsEventBus>(events.ptr());
		if (b) ev = b->raw();
	}
	r->_g = ecs_gas_new(stats->raw(), ev);
	return r;
}

void EcsGas::_bind_methods() {
	ClassDB::bind_static_method("EcsGas", D_METHOD("create", "stats", "events"), &EcsGas::create);

	ClassDB::bind_method(D_METHOD("tag", "name"), &EcsGas::tag);
	ClassDB::bind_method(D_METHOD("has_tags", "entity", "all"), &EcsGas::has_tags);
	ClassDB::bind_method(D_METHOD("set_tags", "entity", "tags"), &EcsGas::set_tags);

	ClassDB::bind_method(D_METHOD("add_effect", "def"), &EcsGas::add_effect);
	ClassDB::bind_method(D_METHOD("apply", "target", "effect", "source"), &EcsGas::apply);
	ClassDB::bind_method(D_METHOD("remove", "target", "effect"), &EcsGas::remove);
	ClassDB::bind_method(D_METHOD("cancel_tags", "target", "tags"), &EcsGas::cancel_tags);
	ClassDB::bind_method(D_METHOD("stacks", "target", "effect"), &EcsGas::stacks);
	ClassDB::bind_method(D_METHOD("active_count"), &EcsGas::active_count);

	ClassDB::bind_method(D_METHOD("add_ability", "def"), &EcsGas::add_ability);
	ClassDB::bind_method(D_METHOD("can_activate", "user", "ability"), &EcsGas::can_activate);
	ClassDB::bind_method(D_METHOD("activate", "user", "ability", "target"), &EcsGas::activate);

	ClassDB::bind_method(D_METHOD("tick", "dt"), &EcsGas::tick);

	static const StringName cls = get_class_static();
	ClassDB::bind_integer_constant(cls, "", "EFFECT_INSTANT", ECS_EFFECT_INSTANT);
	ClassDB::bind_integer_constant(cls, "", "EFFECT_DURATION", ECS_EFFECT_DURATION);
	ClassDB::bind_integer_constant(cls, "", "EFFECT_INFINITE", ECS_EFFECT_INFINITE);

	ClassDB::bind_integer_constant(cls, "", "MOD_ADD", ECS_MOD_ADD);
	ClassDB::bind_integer_constant(cls, "", "MOD_MUL", ECS_MOD_MUL);
	ClassDB::bind_integer_constant(cls, "", "MOD_SET", ECS_MOD_SET);

	ClassDB::bind_integer_constant(cls, "", "STACK_NONE", ECS_STACK_NONE);
	ClassDB::bind_integer_constant(cls, "", "STACK_REFRESH", ECS_STACK_REFRESH);
	ClassDB::bind_integer_constant(cls, "", "STACK_STACK", ECS_STACK_STACK);

	ClassDB::bind_integer_constant(cls, "", "NO_STAT", (int)ECS_NO_STAT);

	ClassDB::bind_integer_constant(cls, "", "ABILITY_OK", ECS_ABILITY_OK);
	ClassDB::bind_integer_constant(cls, "", "ABILITY_BAD_ENTITY", ECS_ABILITY_BAD_ENTITY);
	ClassDB::bind_integer_constant(cls, "", "ABILITY_DEAD", ECS_ABILITY_DEAD);
	ClassDB::bind_integer_constant(cls, "", "ABILITY_MISSING_TAGS", ECS_ABILITY_MISSING_TAGS);
	ClassDB::bind_integer_constant(cls, "", "ABILITY_BLOCKED", ECS_ABILITY_BLOCKED);
	ClassDB::bind_integer_constant(cls, "", "ABILITY_COOLDOWN", ECS_ABILITY_COOLDOWN);
	ClassDB::bind_integer_constant(cls, "", "ABILITY_NO_RESOURCE", ECS_ABILITY_NO_RESOURCE);
}

int64_t EcsGas::tag(const String &name) {
	return _g ? (int64_t)ecs_gas_tag(_g, name.utf8().get_data()) : 0;
}
bool EcsGas::has_tags(int64_t e, int64_t all) const {
	return _g && ecs_gas_has_tags(_g, (ecs_entity)e, (uint64_t)all);
}
void EcsGas::set_tags(int64_t e, int64_t tags) {
	if (_g) ecs_gas_set_tags(_g, (ecs_entity)e, (uint64_t)tags);
}

int EcsGas::add_effect(Dictionary def) {
	if (!_g) return -1;
	ecs_effect_def d = effect_from_dict(def);
	return ecs_gas_add_effect(_g, &d);
}
bool EcsGas::apply(int64_t target, int effect, int64_t source) {
	return _g && ecs_gas_apply(_g, (ecs_entity)target, effect, (ecs_entity)source);
}
int EcsGas::remove(int64_t target, int effect) {
	return _g ? ecs_gas_remove(_g, (ecs_entity)target, effect) : 0;
}
int EcsGas::cancel_tags(int64_t target, int64_t tags) {
	return _g ? ecs_gas_cancel_tags(_g, (ecs_entity)target, (uint64_t)tags) : 0;
}
int EcsGas::stacks(int64_t target, int effect) const {
	return _g ? ecs_gas_stacks(_g, (ecs_entity)target, effect) : 0;
}
int EcsGas::active_count() const { return _g ? ecs_gas_active_count(_g) : 0; }

int EcsGas::add_ability(Dictionary def) {
	if (!_g) return -1;
	ecs_ability_def d = ability_from_dict(def);
	return ecs_gas_add_ability(_g, &d);
}
int EcsGas::can_activate(int64_t user, int ability) const {
	return _g ? (int)ecs_gas_can_activate(_g, (ecs_entity)user, ability) : (int)ECS_ABILITY_BAD_ENTITY;
}
int EcsGas::activate(int64_t user, int ability, int64_t target) {
	return _g ? (int)ecs_gas_activate(_g, (ecs_entity)user, ability, (ecs_entity)target) : (int)ECS_ABILITY_BAD_ENTITY;
}

void EcsGas::tick(float dt) {
	if (_g) ecs_gas_tick(_g, dt);
}
