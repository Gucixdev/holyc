#ifndef HOLYC_BLACKBOARD_H_
#define HOLYC_BLACKBOARD_H_

/* Minimal shared key-value store for AI systems (behavior trees, FSMs,
 * planners, ...) to communicate without direct coupling - one system
 * writes "target_hp", another reads it, neither knows the other exists.
 *
 * This is exactly what src/utils/tag.h's Tag_Set already is (string
 * key -> typed value: i64/f64/string/bool, arena-backed, hashtable-
 * backed) - checked before writing anything new here, per this
 * project's reuse-before-reimplement discipline. Blackboard is
 * therefore a thin wrapper (typedef + real blackboard_-named
 * functions, not just macro aliases) rather than a fresh
 * reimplementation: the wrapper exists so gamedev/ml call sites read
 * as "blackboard" (the domain vocabulary) instead of the more generic
 * "tag", not because the underlying behavior differs at all. */

#include "base.h"
#include "arena_alloc.h"
#include "tag.h"

typedef Tag_Set Blackboard;

void blackboard_init(Arena *a, Blackboard *bb, size_t capacity_hint);

void blackboard_set_i64(Arena *a, Blackboard *bb, const char *key, i64 value);
void blackboard_set_f64(Arena *a, Blackboard *bb, const char *key, f64 value);
void blackboard_set_string(Arena *a, Blackboard *bb, const char *key, const char *value);
void blackboard_set_bool(Arena *a, Blackboard *bb, const char *key, i32 value);

/* NULL if key isn't present. */
const Tag_Value *blackboard_get(const Blackboard *bb, const char *key);
i32 blackboard_has(const Blackboard *bb, const char *key);

#endif /* HOLYC_BLACKBOARD_H_ */

#ifdef BLACKBOARD_IMPLEMENTATION
#ifndef BLACKBOARD_IMPLEMENTATION_
#define BLACKBOARD_IMPLEMENTATION_

void blackboard_init(Arena *a, Blackboard *bb, size_t capacity_hint)
{
    tag_set_init(a, bb, capacity_hint);
}

void blackboard_set_i64(Arena *a, Blackboard *bb, const char *key, i64 value)
{
    tag_set_i64(a, bb, key, value);
}

void blackboard_set_f64(Arena *a, Blackboard *bb, const char *key, f64 value)
{
    tag_set_f64(a, bb, key, value);
}

void blackboard_set_string(Arena *a, Blackboard *bb, const char *key, const char *value)
{
    tag_set_string(a, bb, key, value);
}

void blackboard_set_bool(Arena *a, Blackboard *bb, const char *key, i32 value)
{
    tag_set_bool(a, bb, key, value);
}

const Tag_Value *blackboard_get(const Blackboard *bb, const char *key)
{
    return tag_get(bb, key);
}

i32 blackboard_has(const Blackboard *bb, const char *key)
{
    return tag_has(bb, key);
}

#endif /* BLACKBOARD_IMPLEMENTATION_ */
#endif /* BLACKBOARD_IMPLEMENTATION */
