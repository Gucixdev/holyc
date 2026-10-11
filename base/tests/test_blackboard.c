#include "test_common.h"
#define BASE_TYPE_IMPLEMENTATION
#define ARENA_ALLOC_IMPLEMENTATION
#define HASHTABLE_IMPLEMENTATION
#define TAG_IMPLEMENTATION
#define BLACKBOARD_IMPLEMENTATION
#include "base.h"
#include "arena_alloc.h"
#include "hashtable.h"
#include "tag.h"
#include "blackboard.h"

int main(void)
{
    Arena a = {0};
    Blackboard bb;

    print_cstr("test_blackboard\n");

    blackboard_init(&a, &bb, 8);

    blackboard_set_i64(&a, &bb, "target_hp", 100);
    blackboard_set_f64(&a, &bb, "alert_radius", 12.5);
    blackboard_set_string(&a, &bb, "target_name", "goblin");
    blackboard_set_bool(&a, &bb, "has_target", 1);

    TEST_CHECK(blackboard_has(&bb, "target_hp"), "blackboard_has finds an i64 key that was set");
    TEST_CHECK(!blackboard_has(&bb, "nonexistent_key"), "blackboard_has returns false for a key never set");

    {
        const Tag_Value *v = blackboard_get(&bb, "target_hp");
        TEST_CHECK(v != 0 && v->kind == TAG_I64 && v->v.as_i64 == 100, "blackboard_get retrieves the correct i64 value");
    }
    {
        const Tag_Value *v = blackboard_get(&bb, "alert_radius");
        TEST_CHECK(v != 0 && v->kind == TAG_F64 && v->v.as_f64 > 12.4 && v->v.as_f64 < 12.6, "blackboard_get retrieves the correct f64 value");
    }
    {
        const Tag_Value *v = blackboard_get(&bb, "target_name");
        TEST_CHECK(v != 0 && v->kind == TAG_STRING && v->v.as_string[0] == 'g', "blackboard_get retrieves the correct string value");
    }
    {
        const Tag_Value *v = blackboard_get(&bb, "has_target");
        TEST_CHECK(v != 0 && v->kind == TAG_BOOL && v->v.as_bool == 1, "blackboard_get retrieves the correct bool value");
    }

    TEST_CHECK(blackboard_get(&bb, "missing") == 0, "blackboard_get on a missing key returns NULL, not garbage");

    /* overwrite an existing key */
    blackboard_set_i64(&a, &bb, "target_hp", 42);
    {
        const Tag_Value *v = blackboard_get(&bb, "target_hp");
        TEST_CHECK(v != 0 && v->v.as_i64 == 42, "setting an already-present key overwrites its value");
    }

    print_flush();
    return test_failures_ != 0;
}
