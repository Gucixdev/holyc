#include "test_common.h"
#define BASE_TYPE_IMPLEMENTATION
#define ARENA_ALLOC_IMPLEMENTATION
#define HASHTABLE_IMPLEMENTATION
#define HASH_MAP_IMPLEMENTATION
#define STR_IMPLEMENTATION
#define CONFIG_IMPLEMENTATION
#include "alloc/arena_alloc.h"
#include "util/config.h"

int main(void)
{
    Arena a = {0};
    Config c;

    print_cstr("test_config\n");

    config_init(&a, &c);
    TEST_CHECK(!config_has(&c, "width"), "a fresh config has no keys");
    TEST_CHECK(config_get_i32(&c, "width", 800) == 800, "a missing key returns the given default");

    config_set_i32(&a, &c, "width", 1920);
    config_set_f64(&a, &c, "volume", 0.75);
    config_set_string(&a, &c, "title", "holyc");
    config_set_bool(&a, &c, "fullscreen", 1);

    TEST_CHECK(config_get_i32(&c, "width", 0) == 1920, "config_get_i32 returns a previously set value");
    TEST_CHECK(config_get_f64(&c, "volume", 0.0) == 0.75, "config_get_f64 returns a previously set value");
    TEST_CHECK(str_cmp(config_get_string(&c, "title", ""), "holyc") == 0, "config_get_string returns a previously set value");
    TEST_CHECK(config_get_bool(&c, "fullscreen", 0) == 1, "config_get_bool returns a previously set value");

    TEST_CHECK(config_get_f64(&c, "width", -1.0) == -1.0, "reading an i32 key as f64 returns the default, not a bit-punned garbage value");

    config_set_i32(&a, &c, "width", 640);
    TEST_CHECK(config_get_i32(&c, "width", 0) == 640, "setting an existing key overwrites its value");

    print_flush();
    return test_failures_ != 0;
}
