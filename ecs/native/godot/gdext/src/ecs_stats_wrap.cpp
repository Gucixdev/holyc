#include "ecs_stats_wrap.h"

#include "ecs_events.h"

using namespace godot;

// ---- Dictionary <-> C struct helpers (file-local; GDScript sees Dictionary, not these structs) ----

static void floats_from_dict(const Dictionary &d, const char *key, float *out, int n) {
	for (int i = 0; i < n; i++) out[i] = 0.0f;
	if (!d.has(key)) return;
	PackedFloat32Array a = d[key];
	for (int i = 0; i < n && i < a.size(); i++) out[i] = a[i];
}

static PackedFloat32Array floats_to_array(const float *in, int n) {
	PackedFloat32Array a;
	a.resize(n);
	for (int i = 0; i < n; i++) a[i] = in[i];
	return a;
}

static ecs_stats_config config_from_dict(const Dictionary &d) {
	ecs_stats_config c = ecs_stats_config_default();
	floats_from_dict(d, "pool_regen", c.pool_regen, ECS_POOL_COUNT);
	floats_from_dict(d, "pool_delay", c.pool_delay, ECS_POOL_COUNT);
	floats_from_dict(d, "pool_exhaust_delay_mult", c.pool_exhaust_delay_mult, ECS_POOL_COUNT);
	floats_from_dict(d, "bu_threshold", c.bu_threshold, ECS_BUILDUP_COUNT);
	floats_from_dict(d, "bu_decay", c.bu_decay, ECS_BUILDUP_COUNT);
	floats_from_dict(d, "bu_duration", c.bu_duration, ECS_BUILDUP_COUNT);
	floats_from_dict(d, "bu_dot_pct_max_hp", c.bu_dot_pct_max_hp, ECS_BUILDUP_COUNT);
	if (d.has("roll_light_max")) c.roll_light_max = d["roll_light_max"];
	if (d.has("roll_medium_max")) c.roll_medium_max = d["roll_medium_max"];
	return c;
}

static Dictionary config_to_dict(const ecs_stats_config &c) {
	Dictionary d;
	d["pool_regen"] = floats_to_array(c.pool_regen, ECS_POOL_COUNT);
	d["pool_delay"] = floats_to_array(c.pool_delay, ECS_POOL_COUNT);
	d["pool_exhaust_delay_mult"] = floats_to_array(c.pool_exhaust_delay_mult, ECS_POOL_COUNT);
	d["bu_threshold"] = floats_to_array(c.bu_threshold, ECS_BUILDUP_COUNT);
	d["bu_decay"] = floats_to_array(c.bu_decay, ECS_BUILDUP_COUNT);
	d["bu_duration"] = floats_to_array(c.bu_duration, ECS_BUILDUP_COUNT);
	d["bu_dot_pct_max_hp"] = floats_to_array(c.bu_dot_pct_max_hp, ECS_BUILDUP_COUNT);
	d["roll_light_max"] = c.roll_light_max;
	d["roll_medium_max"] = c.roll_medium_max;
	return d;
}

static ecs_weapon weapon_from_dict(const Dictionary &d) {
	ecs_weapon w{};
	floats_from_dict(d, "base", w.base, ECS_DMG_COUNT);
	floats_from_dict(d, "buildup", w.buildup, ECS_BUILDUP_COUNT);
	if (d.has("scale_str")) w.scale_str = d["scale_str"];
	if (d.has("scale_dex")) w.scale_dex = d["scale_dex"];
	if (d.has("scale_int")) w.scale_int = d["scale_int"];
	if (d.has("scale_fai")) w.scale_fai = d["scale_fai"];
	if (d.has("scale_arc")) w.scale_arc = d["scale_arc"];
	if (d.has("poise_damage")) w.poise_damage = d["poise_damage"];
	return w;
}

static ecs_attributes attrs_from_dict(const Dictionary &d) {
	ecs_attributes a{};
	if (d.has("vigor")) a.vigor = (int)d["vigor"];
	if (d.has("mind")) a.mind = (int)d["mind"];
	if (d.has("endurance")) a.endurance = (int)d["endurance"];
	if (d.has("strength")) a.strength = (int)d["strength"];
	if (d.has("dexterity")) a.dexterity = (int)d["dexterity"];
	if (d.has("intelligence")) a.intelligence = (int)d["intelligence"];
	if (d.has("faith")) a.faith = (int)d["faith"];
	if (d.has("arcane")) a.arcane = (int)d["arcane"];
	return a;
}

static ecs_attack attack_from_dict(const Dictionary &d) {
	ecs_attack a{};
	floats_from_dict(d, "damage", a.damage, ECS_DMG_COUNT);
	floats_from_dict(d, "buildup", a.buildup, ECS_BUILDUP_COUNT);
	if (d.has("poise_damage")) a.poise_damage = d["poise_damage"];
	if (d.has("source")) a.source = (ecs_entity)(int64_t)d["source"];
	if (d.has("crit")) a.crit = d["crit"];
	return a;
}

static Dictionary attack_to_dict(const ecs_attack &a) {
	Dictionary d;
	d["damage"] = floats_to_array(a.damage, ECS_DMG_COUNT);
	d["buildup"] = floats_to_array(a.buildup, ECS_BUILDUP_COUNT);
	d["poise_damage"] = a.poise_damage;
	d["source"] = (int64_t)a.source;
	d["crit"] = a.crit;
	return d;
}

static Dictionary hit_result_to_dict(const ecs_hit_result &r) {
	Dictionary d;
	d["dealt"] = r.dealt;
	d["overkill"] = r.overkill;
	d["killed"] = r.killed;
	d["staggered"] = r.staggered;
	d["status_triggered"] = (int)r.status_triggered;
	return d;
}

// ---- EcsStats ----

EcsStats::EcsStats() {
	_s = ecs_stats_new(nullptr, 0);
}

EcsStats::~EcsStats() {
	if (_s) ecs_stats_free(_s);
}

Ref<EcsStats> EcsStats::create(Dictionary cfg, int capacity_hint) {
	Ref<EcsStats> r;
	r.instantiate();
	ecs_stats_config c = config_from_dict(cfg);
	if (r->_s) ecs_stats_free(r->_s);
	r->_s = ecs_stats_new(&c, capacity_hint);
	return r;
}

void EcsStats::_bind_methods() {
	ClassDB::bind_static_method("EcsStats", D_METHOD("create", "cfg", "capacity_hint"), &EcsStats::create);

	ClassDB::bind_method(D_METHOD("add", "entity"), &EcsStats::add);
	ClassDB::bind_method(D_METHOD("remove", "entity"), &EcsStats::remove);
	ClassDB::bind_method(D_METHOD("row", "entity"), &EcsStats::row);
	ClassDB::bind_method(D_METHOD("count"), &EcsStats::count);
	ClassDB::bind_method(D_METHOD("entities"), &EcsStats::entities);
	ClassDB::bind_method(D_METHOD("collect", "world"), &EcsStats::collect);

	ClassDB::bind_method(D_METHOD("col", "column"), &EcsStats::col);
	ClassDB::bind_method(D_METHOD("flags"), &EcsStats::flags);

	ClassDB::bind_method(D_METHOD("tick", "dt"), &EcsStats::tick);

	ClassDB::bind_method(D_METHOD("damage", "row", "amounts", "poise_damage"), &EcsStats::damage);
	ClassDB::bind_method(D_METHOD("damage_preview", "row", "amounts"), &EcsStats::damage_preview);
	ClassDB::bind_method(D_METHOD("spend", "row", "pool", "amount"), &EcsStats::spend);
	ClassDB::bind_method(D_METHOD("restore", "row", "pool", "amount"), &EcsStats::restore);
	ClassDB::bind_method(D_METHOD("reset_poise", "row"), &EcsStats::reset_poise);
	ClassDB::bind_method(D_METHOD("add_buildup", "row", "type", "amount"), &EcsStats::add_buildup);

	ClassDB::bind_method(D_METHOD("scaling", "level"), &EcsStats::scaling);
	ClassDB::bind_method(D_METHOD("set_weapon", "row", "weapon", "attributes"), &EcsStats::set_weapon);
	ClassDB::bind_method(D_METHOD("make_attack", "row", "motion"), &EcsStats::make_attack);
	ClassDB::bind_method(D_METHOD("apply_attack", "target_row", "attack"), &EcsStats::apply_attack);
	ClassDB::bind_method(D_METHOD("hit", "attacker_row", "target_row", "motion"), &EcsStats::hit);
	ClassDB::bind_method(D_METHOD("apply_delta", "row", "pool", "delta", "source"), &EcsStats::apply_delta);
	ClassDB::bind_method(D_METHOD("set_crit_mult", "mult"), &EcsStats::set_crit_mult);

	ClassDB::bind_method(D_METHOD("attach_events", "bus"), &EcsStats::attach_events);

	ClassDB::bind_method(D_METHOD("tags", "row"), &EcsStats::tags);
	ClassDB::bind_method(D_METHOD("tags_base", "row"), &EcsStats::tags_base);
	ClassDB::bind_method(D_METHOD("set_tags_base", "row", "mask"), &EcsStats::set_tags_base);
	ClassDB::bind_method(D_METHOD("set_exhaust_mult", "pool", "mult"), &EcsStats::set_exhaust_mult);
	ClassDB::bind_method(D_METHOD("get_config"), &EcsStats::get_config);

	ClassDB::bind_method(D_METHOD("set_load", "row", "carried", "capacity"), &EcsStats::set_load);
	ClassDB::bind_method(D_METHOD("load_ratio", "row"), &EcsStats::load_ratio);
	ClassDB::bind_method(D_METHOD("roll_class", "row"), &EcsStats::roll_class);
	ClassDB::bind_method(D_METHOD("derive", "row", "attributes"), &EcsStats::derive);

	static const StringName cls = get_class_static();
	ClassDB::bind_integer_constant(cls, "", "POOL_HP", ECS_POOL_HP);
	ClassDB::bind_integer_constant(cls, "", "POOL_FP", ECS_POOL_FP);
	ClassDB::bind_integer_constant(cls, "", "POOL_SP", ECS_POOL_SP);
	ClassDB::bind_integer_constant(cls, "", "POOL_POISE", ECS_POOL_POISE);
	ClassDB::bind_integer_constant(cls, "", "POOL_COUNT", ECS_POOL_COUNT);

	ClassDB::bind_integer_constant(cls, "", "DMG_PHYSICAL", ECS_DMG_PHYSICAL);
	ClassDB::bind_integer_constant(cls, "", "DMG_MAGIC", ECS_DMG_MAGIC);
	ClassDB::bind_integer_constant(cls, "", "DMG_FIRE", ECS_DMG_FIRE);
	ClassDB::bind_integer_constant(cls, "", "DMG_LIGHTNING", ECS_DMG_LIGHTNING);
	ClassDB::bind_integer_constant(cls, "", "DMG_HOLY", ECS_DMG_HOLY);
	ClassDB::bind_integer_constant(cls, "", "DMG_COUNT", ECS_DMG_COUNT);

	ClassDB::bind_integer_constant(cls, "", "BUILDUP_POISON", ECS_BUILDUP_POISON);
	ClassDB::bind_integer_constant(cls, "", "BUILDUP_BLEED", ECS_BUILDUP_BLEED);
	ClassDB::bind_integer_constant(cls, "", "BUILDUP_FROST", ECS_BUILDUP_FROST);
	ClassDB::bind_integer_constant(cls, "", "BUILDUP_CURSE", ECS_BUILDUP_CURSE);
	ClassDB::bind_integer_constant(cls, "", "BUILDUP_COUNT", ECS_BUILDUP_COUNT);

	ClassDB::bind_integer_constant(cls, "", "FLAG_DEAD", ECS_FLAG_DEAD);
	ClassDB::bind_integer_constant(cls, "", "FLAG_STAGGERED", ECS_FLAG_STAGGERED);
	ClassDB::bind_integer_constant(cls, "", "FLAG_EXHAUSTED", ECS_FLAG_EXHAUSTED);
	ClassDB::bind_integer_constant(cls, "", "FLAG_OVERLOADED", ECS_FLAG_OVERLOADED);
	ClassDB::bind_integer_constant(cls, "", "FLAG_POISONED", ECS_FLAG_POISONED);
	ClassDB::bind_integer_constant(cls, "", "FLAG_BLEEDING", ECS_FLAG_BLEEDING);
	ClassDB::bind_integer_constant(cls, "", "FLAG_FROZEN", ECS_FLAG_FROZEN);
	ClassDB::bind_integer_constant(cls, "", "FLAG_CURSED", ECS_FLAG_CURSED);

	ClassDB::bind_integer_constant(cls, "", "ROLL_LIGHT", ECS_ROLL_LIGHT);
	ClassDB::bind_integer_constant(cls, "", "ROLL_MEDIUM", ECS_ROLL_MEDIUM);
	ClassDB::bind_integer_constant(cls, "", "ROLL_HEAVY", ECS_ROLL_HEAVY);
	ClassDB::bind_integer_constant(cls, "", "ROLL_COUNT", ECS_ROLL_COUNT);

	ClassDB::bind_integer_constant(cls, "", "COL_CUR", ECS_COL_CUR);
	ClassDB::bind_integer_constant(cls, "", "COL_MAX", ECS_COL_MAX);
	ClassDB::bind_integer_constant(cls, "", "COL_REGEN", ECS_COL_REGEN);
	ClassDB::bind_integer_constant(cls, "", "COL_DELAY", ECS_COL_DELAY);
	ClassDB::bind_integer_constant(cls, "", "COL_TIMER", ECS_COL_TIMER);
	ClassDB::bind_integer_constant(cls, "", "COL_BU_VALUE", ECS_COL_BU_VALUE);
	ClassDB::bind_integer_constant(cls, "", "COL_BU_THRESH", ECS_COL_BU_THRESH);
	ClassDB::bind_integer_constant(cls, "", "COL_BU_DECAY", ECS_COL_BU_DECAY);
	ClassDB::bind_integer_constant(cls, "", "COL_BU_ACTIVE", ECS_COL_BU_ACTIVE);
	ClassDB::bind_integer_constant(cls, "", "COL_DEFENSE", ECS_COL_DEFENSE);
	ClassDB::bind_integer_constant(cls, "", "COL_RESIST", ECS_COL_RESIST);
	ClassDB::bind_integer_constant(cls, "", "COL_ATK", ECS_COL_ATK);
	ClassDB::bind_integer_constant(cls, "", "COL_ATK_POISE", ECS_COL_ATK_POISE);
	ClassDB::bind_integer_constant(cls, "", "COL_ATK_BU", ECS_COL_ATK_BU);
	ClassDB::bind_integer_constant(cls, "", "COL_LOAD", ECS_COL_LOAD);
	ClassDB::bind_integer_constant(cls, "", "COL_LOAD_MAX", ECS_COL_LOAD_MAX);
	ClassDB::bind_integer_constant(cls, "", "COL_COUNT", ECS_COL_COUNT);
}

int EcsStats::add(int64_t e) { return ecs_stats_add(_s, (ecs_entity)e); }
void EcsStats::remove(int64_t e) { ecs_stats_remove(_s, (ecs_entity)e); }
int EcsStats::row(int64_t e) const { return ecs_stats_row(_s, (ecs_entity)e); }
int EcsStats::count() const { return ecs_stats_count(_s); }

PackedInt64Array EcsStats::entities() const {
	PackedInt64Array out;
	int n = ecs_stats_count(_s);
	out.resize(n);
	const ecs_entity *ents = ecs_stats_entities(_s);
	for (int i = 0; i < n; i++) out[i] = (int64_t)ents[i];
	return out;
}

int EcsStats::collect(const Ref<EcsWorld> &world) {
	if (world.is_null()) return 0;
	return ecs_stats_collect(_s, world->raw());
}

PackedFloat32Array EcsStats::col(int column) const {
	int n = ecs_stats_count(_s);
	float *c = ecs_stats_col((ecs_stats *)_s, (ecs_stat_col)column);
	PackedFloat32Array out;
	if (!c) return out;
	out.resize(n);
	for (int i = 0; i < n; i++) out[i] = c[i];
	return out;
}

PackedInt32Array EcsStats::flags() const {
	int n = ecs_stats_count(_s);
	uint32_t *f = ecs_stats_flags((ecs_stats *)_s);
	PackedInt32Array out;
	if (!f) return out;
	out.resize(n);
	for (int i = 0; i < n; i++) out[i] = (int32_t)f[i];
	return out;
}

void EcsStats::tick(float dt) { ecs_stats_tick(_s, dt); }

float EcsStats::damage(int r, PackedFloat32Array amounts, float poise_damage) {
	float a[ECS_DMG_COUNT] = {};
	for (int i = 0; i < ECS_DMG_COUNT && i < amounts.size(); i++) a[i] = amounts[i];
	return ecs_stats_damage(_s, r, a, poise_damage);
}
float EcsStats::damage_preview(int r, PackedFloat32Array amounts) const {
	float a[ECS_DMG_COUNT] = {};
	for (int i = 0; i < ECS_DMG_COUNT && i < amounts.size(); i++) a[i] = amounts[i];
	return ecs_stats_damage_preview(_s, r, a);
}
bool EcsStats::spend(int r, int pool, float amount) { return ecs_stats_spend(_s, r, (ecs_pool)pool, amount); }
void EcsStats::restore(int r, int pool, float amount) { ecs_stats_restore(_s, r, (ecs_pool)pool, amount); }
void EcsStats::reset_poise(int r) { ecs_stats_reset_poise(_s, r); }
bool EcsStats::add_buildup(int r, int type, float amount) { return ecs_stats_add_buildup(_s, r, (ecs_buildup)type, amount); }

float EcsStats::scaling(int level) const { return ecs_stats_scaling(level); }

void EcsStats::set_weapon(int r, Dictionary weapon, Dictionary attributes) {
	ecs_weapon w = weapon_from_dict(weapon);
	ecs_attributes a = attrs_from_dict(attributes);
	ecs_stats_set_weapon(_s, r, &w, &a);
}
Dictionary EcsStats::make_attack(int r, float motion) const {
	ecs_attack a = ecs_stats_make_attack(_s, r, motion);
	return attack_to_dict(a);
}
Dictionary EcsStats::apply_attack(int target_row, Dictionary attack) {
	ecs_attack a = attack_from_dict(attack);
	ecs_hit_result res = ecs_stats_apply_attack(_s, target_row, &a);
	return hit_result_to_dict(res);
}
Dictionary EcsStats::hit(int attacker_row, int target_row, float motion) {
	ecs_hit_result res = ecs_stats_hit(_s, attacker_row, target_row, motion);
	return hit_result_to_dict(res);
}
float EcsStats::apply_delta(int r, int pool, float delta, int64_t source) {
	return ecs_stats_apply_delta(_s, r, (ecs_pool)pool, delta, (ecs_entity)source);
}
void EcsStats::set_crit_mult(float mult) { ecs_stats_set_crit_mult(_s, mult); }

void EcsStats::attach_events(const Ref<RefCounted> &bus) {
	EcsEventBus *b = bus.is_valid() ? Object::cast_to<EcsEventBus>(bus.ptr()) : nullptr;
	ecs_stats_attach_events(_s, b ? b->raw() : nullptr);
}

int64_t EcsStats::tags(int r) const {
	uint64_t *t = ecs_stats_tags((ecs_stats *)_s);
	return t ? (int64_t)t[r] : 0;
}
int64_t EcsStats::tags_base(int r) const {
	uint64_t *t = ecs_stats_tags_base((ecs_stats *)_s);
	return t ? (int64_t)t[r] : 0;
}
void EcsStats::set_tags_base(int r, int64_t mask) {
	uint64_t *t = ecs_stats_tags_base(_s);
	if (t) t[r] = (uint64_t)mask;
}
void EcsStats::set_exhaust_mult(int pool, float mult) { ecs_stats_set_exhaust_mult(_s, (ecs_pool)pool, mult); }
Dictionary EcsStats::get_config() const { return config_to_dict(*ecs_stats_get_config(_s)); }

void EcsStats::set_load(int r, float carried, float capacity) { ecs_stats_set_load(_s, r, carried, capacity); }
float EcsStats::load_ratio(int r) const { return ecs_stats_load_ratio(_s, r); }
int EcsStats::roll_class(int r) const { return (int)ecs_stats_roll_class(_s, r); }
void EcsStats::derive(int r, Dictionary attributes) {
	ecs_attributes a = attrs_from_dict(attributes);
	ecs_stats_derive(_s, r, &a);
}
