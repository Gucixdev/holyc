#ifndef HOLYC_TAG_H_
#define HOLYC_TAG_H_

/* Generic string-key -> typed-value metadata bag ("tags"), for attaching
 * small amounts of ad-hoc metadata to an arbitrary thing (an asset, a
 * log entry, a config value) without that thing needing its own
 * bespoke struct field per possible tag. Backed by src/math/hash/
 * hashtable.h (Ht) rather than reimplementing hashing/probing here -
 * a Tag_Set IS a Ht plus a typed value discipline on top of Ht's
 * opaque void* values.
 *
 * Deliberately small: four value kinds (i64/f64/string/bool) cover the
 * overwhelming majority of real metadata use (counts, measurements,
 * names/labels, flags) without turning into a general dynamic-typing
 * system - a caller that needs more than this is better served by
 * src/codec/json.h's Json_Value tree, not by growing this file to
 * match it. String values are copied into arena-owned storage on set
 * (same convention hashtable.h's own keys already use), so the
 * caller's string buffer doesn't need to outlive the call. */

#include "core/base.h"
#include "core/arena_alloc.h"
#include "data/hash/hashtable.h"

typedef enum {
    TAG_I64 = 0,
    TAG_F64,
    TAG_STRING,
    TAG_BOOL
} Tag_Kind;

typedef struct {
    Tag_Kind kind;
    union {
        i64 as_i64;
        f64 as_f64;
        const char *as_string; /* arena-owned copy */
        i32 as_bool;
    } v;
} Tag_Value;

typedef struct {
    Ht ht;
} Tag_Set;

void tag_set_init(Arena *a, Tag_Set *ts, size_t capacity_hint);

void tag_set_i64(Arena *a, Tag_Set *ts, const char *key, i64 value);
void tag_set_f64(Arena *a, Tag_Set *ts, const char *key, f64 value);
void tag_set_string(Arena *a, Tag_Set *ts, const char *key, const char *value);
void tag_set_bool(Arena *a, Tag_Set *ts, const char *key, i32 value);

/* NULL if key isn't present. */
const Tag_Value *tag_get(const Tag_Set *ts, const char *key);
i32 tag_has(const Tag_Set *ts, const char *key);

#endif /* HOLYC_TAG_H_ */

#ifdef TAG_IMPLEMENTATION
#ifndef TAG_IMPLEMENTATION_
#define TAG_IMPLEMENTATION_

void tag_set_init(Arena *a, Tag_Set *ts, size_t capacity_hint)
{
    ht_init(a, &ts->ht, capacity_hint);
}

static Tag_Value *tag_set_alloc_value_(Arena *a, Tag_Set *ts, const char *key, Tag_Kind kind)
{
    Tag_Value *v = (Tag_Value *)arena_alloc(a, sizeof(Tag_Value));
    v->kind = kind;
    ht_put(a, &ts->ht, key, arena_strlen(key), v);
    return v;
}

void tag_set_i64(Arena *a, Tag_Set *ts, const char *key, i64 value)
{
    Tag_Value *v = tag_set_alloc_value_(a, ts, key, TAG_I64);
    v->v.as_i64 = value;
}

void tag_set_f64(Arena *a, Tag_Set *ts, const char *key, f64 value)
{
    Tag_Value *v = tag_set_alloc_value_(a, ts, key, TAG_F64);
    v->v.as_f64 = value;
}

void tag_set_string(Arena *a, Tag_Set *ts, const char *key, const char *value)
{
    Tag_Value *v = tag_set_alloc_value_(a, ts, key, TAG_STRING);
    v->v.as_string = arena_strdup(a, value);
}

void tag_set_bool(Arena *a, Tag_Set *ts, const char *key, i32 value)
{
    Tag_Value *v = tag_set_alloc_value_(a, ts, key, TAG_BOOL);
    v->v.as_bool = value;
}

const Tag_Value *tag_get(const Tag_Set *ts, const char *key)
{
    return (const Tag_Value *)ht_get(&ts->ht, key, arena_strlen(key));
}

i32 tag_has(const Tag_Set *ts, const char *key)
{
    return tag_get(ts, key) != 0;
}

#endif /* TAG_IMPLEMENTATION_ */
#endif /* TAG_IMPLEMENTATION */
