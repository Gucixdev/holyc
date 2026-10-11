#ifndef HOLYC_CONFIG_H_
#define HOLYC_CONFIG_H_

/* Typed in-memory config registry (string key -> typed value, with a
 * default returned when a key is missing) - built on hashmap.h for
 * storage. Deliberately NOT a file-format parser (that's codec/
 * ini.h's job, already real elsewhere in this repo) - this is
 * what an app's settings actually live in AFTER being loaded from
 * wherever (an ini file, argv via app/args.h, hardcoded defaults),
 * queried with a default so a missing key is never a crash. */

#include "data/hash/hashmap.h"
#include "core/base.h"

typedef enum { CONFIG_I32, CONFIG_F64, CONFIG_STRING, CONFIG_BOOL } Config_Type;

typedef struct {
    Config_Type type;
    union {
        i32 i;
        f64 f;
        const char *s;
        i32 b;
    } as;
} Config_Value;

typedef struct {
    Hash_Map map;
} Config;

void config_init(Arena *a, Config *c);

void config_set_i32(Arena *a, Config *c, const char *key, i32 v);
void config_set_f64(Arena *a, Config *c, const char *key, f64 v);
void config_set_string(Arena *a, Config *c, const char *key, const char *v);
void config_set_bool(Arena *a, Config *c, const char *key, i32 v);

/* each getter returns default_value if the key is absent OR present
 * under a DIFFERENT type - a type mismatch is never silently
 * reinterpreted (no punning an i32 as an f64's bits). */
i32 config_get_i32(const Config *c, const char *key, i32 default_value);
f64 config_get_f64(const Config *c, const char *key, f64 default_value);
const char *config_get_string(const Config *c, const char *key, const char *default_value);
i32 config_get_bool(const Config *c, const char *key, i32 default_value);

i32 config_has(const Config *c, const char *key);

#endif /* HOLYC_CONFIG_H_ */

#ifdef CONFIG_IMPLEMENTATION
#ifndef CONFIG_IMPLEMENTATION_
#define CONFIG_IMPLEMENTATION_

void config_init(Arena *a, Config *c)
{
    hash_map_init(a, &c->map, 8);
}

static Config_Value *config_put_(Arena *a, Config *c, const char *key)
{
    Config_Value *v = (Config_Value *)arena_alloc(a, sizeof(Config_Value));
    hash_map_put(a, &c->map, key, str_len(key), v);
    return v;
}

void config_set_i32(Arena *a, Config *c, const char *key, i32 v)
{
    Config_Value *cv = config_put_(a, c, key);
    cv->type = CONFIG_I32;
    cv->as.i = v;
}

void config_set_f64(Arena *a, Config *c, const char *key, f64 v)
{
    Config_Value *cv = config_put_(a, c, key);
    cv->type = CONFIG_F64;
    cv->as.f = v;
}

void config_set_string(Arena *a, Config *c, const char *key, const char *v)
{
    Config_Value *cv = config_put_(a, c, key);
    cv->type = CONFIG_STRING;
    cv->as.s = v;
}

void config_set_bool(Arena *a, Config *c, const char *key, i32 v)
{
    Config_Value *cv = config_put_(a, c, key);
    cv->type = CONFIG_BOOL;
    cv->as.b = v;
}

static const Config_Value *config_find_(const Config *c, const char *key)
{
    return (const Config_Value *)hash_map_get(&c->map, key, str_len(key));
}

i32 config_get_i32(const Config *c, const char *key, i32 default_value)
{
    const Config_Value *v = config_find_(c, key);
    if (!v || v->type != CONFIG_I32) return default_value;
    return v->as.i;
}

f64 config_get_f64(const Config *c, const char *key, f64 default_value)
{
    const Config_Value *v = config_find_(c, key);
    if (!v || v->type != CONFIG_F64) return default_value;
    return v->as.f;
}

const char *config_get_string(const Config *c, const char *key, const char *default_value)
{
    const Config_Value *v = config_find_(c, key);
    if (!v || v->type != CONFIG_STRING) return default_value;
    return v->as.s;
}

i32 config_get_bool(const Config *c, const char *key, i32 default_value)
{
    const Config_Value *v = config_find_(c, key);
    if (!v || v->type != CONFIG_BOOL) return default_value;
    return v->as.b;
}

i32 config_has(const Config *c, const char *key)
{
    return config_find_(c, key) != NULL;
}

#endif /* CONFIG_IMPLEMENTATION_ */
#endif /* CONFIG_IMPLEMENTATION */
