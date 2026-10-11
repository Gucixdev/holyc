#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#define BASE_TYPE_IMPLEMENTATION
#define HASHTABLE_IMPLEMENTATION
#define TAG_IMPLEMENTATION
#include "arena_alloc.h"
#include "base.h"
#include "hashtable.h"
#include "tag.h"
#include <string.h>

int main(void)
{
    Arena a = {0};
    Tag_Set ts;
    const Tag_Value *v;

    print_cstr("test_tag\n");

    tag_set_init(&a, &ts, 0);

    TEST_CHECK(!tag_has(&ts, "count"), "tag_has: false before anything is set");
    TEST_CHECK(tag_get(&ts, "count") == 0, "tag_get: NULL before anything is set");

    tag_set_i64(&a, &ts, "count", 42);
    v = tag_get(&ts, "count");
    TEST_CHECK(v != 0, "tag_get: i64 tag found after set");
    TEST_CHECK(v->kind == TAG_I64, "tag_get: kind is TAG_I64");
    TEST_CHECK(v->v.as_i64 == 42, "tag_get: i64 value round-trips");

    tag_set_f64(&a, &ts, "ratio", 3.5);
    v = tag_get(&ts, "ratio");
    TEST_CHECK(v->kind == TAG_F64 && v->v.as_f64 == 3.5, "tag_get: f64 tag round-trips");

    {
        char buf[16];
        strcpy(buf, "hello");
        tag_set_string(&a, &ts, "name", buf);
        strcpy(buf, "clobbered"); /* caller's buffer reused - the copy must not alias it */
    }
    v = tag_get(&ts, "name");
    TEST_CHECK(v->kind == TAG_STRING, "tag_get: kind is TAG_STRING");
    TEST_CHECK(strcmp(v->v.as_string, "hello") == 0, "tag_get: string value was copied, not aliased (survives caller buffer reuse)");

    tag_set_bool(&a, &ts, "enabled", 1);
    v = tag_get(&ts, "enabled");
    TEST_CHECK(v->kind == TAG_BOOL && v->v.as_bool == 1, "tag_get: bool tag round-trips");

    TEST_CHECK(tag_has(&ts, "count"), "tag_has: true after set");
    TEST_CHECK(!tag_has(&ts, "nonexistent"), "tag_has: false for a key that was never set");

    /* overwrite an existing key with a different kind entirely */
    tag_set_bool(&a, &ts, "count", 0);
    v = tag_get(&ts, "count");
    TEST_CHECK(v->kind == TAG_BOOL && v->v.as_bool == 0, "tag_set: overwriting an existing key changes both kind and value");

    arena_free(&a);
    print_flush();
    return test_failures_ != 0;
}
