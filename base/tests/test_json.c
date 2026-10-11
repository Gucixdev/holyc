#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#define STRING_VIEW_IMPLEMENTATION
#define JSON_IMPLEMENTATION
#include "arena_alloc.h"
#include "str.h"
#include "json.h"
#include <string.h>

static i32 sv_eq_cstr_(String_View sv, const char *expected)
{
    size_t len = strlen(expected);
    return sv.count == len && memcmp(sv.data, expected, len) == 0;
}

int main(void)
{
    Arena a = {0};

    print_cstr("test_json\n");

    {
        const char *text = "null";
        Json_Value *v;
        i32 err = json_parse(&a, text, strlen(text), &v);
        TEST_CHECK(err == JSON_ESUCCESS && v->type == JSON_NULL, "bare null parses");
    }

    {
        const char *text = "true";
        Json_Value *v;
        i32 err = json_parse(&a, text, strlen(text), &v);
        TEST_CHECK(err == JSON_ESUCCESS && v->type == JSON_BOOL && v->boolean == 1, "bare true parses");
    }

    {
        const char *text = "-12.5e2";
        Json_Value *v;
        i32 err = json_parse(&a, text, strlen(text), &v);
        TEST_CHECK(err == JSON_ESUCCESS && v->type == JSON_NUMBER && v->number == -1250.0,
                   "signed float with exponent parses to the correct value");
    }

    {
        const char *text = "\"hello\\nworld \\u0041\\u00e9\"";
        Json_Value *v;
        i32 err = json_parse(&a, text, strlen(text), &v);
        TEST_CHECK(err == JSON_ESUCCESS && v->type == JSON_STRING, "escaped string parses");
        TEST_CHECK(sv_eq_cstr_(v->string, "hello\nworld A\xc3\xa9"),
                   "\\n, \\uXXXX (ASCII and non-ASCII BMP) all decode correctly (\\u00e9 -> UTF-8 0xC3 0xA9)");
    }

    {
        const char *text = "[1, 2, [3, 4], \"five\"]";
        Json_Value *v;
        i32 err = json_parse(&a, text, strlen(text), &v);
        TEST_CHECK(err == JSON_ESUCCESS && v->type == JSON_ARRAY && v->array.count == 4, "nested array parses, 4 top-level elements");
        TEST_CHECK(json_array_get(v, 0)->number == 1.0 && json_array_get(v, 1)->number == 2.0,
                   "flat number elements correct");
        TEST_CHECK(json_array_get(v, 2)->type == JSON_ARRAY && json_array_get(v, 2)->array.count == 2,
                   "nested array element correct");
        TEST_CHECK(sv_eq_cstr_(json_array_get(v, 3)->string, "five"), "trailing string element correct");
        TEST_CHECK(json_array_get(v, 99) == 0, "out-of-range array index returns NULL, not garbage");
    }

    {
        const char *text = "{\"name\": \"vecindex\", \"n\": 3, \"nested\": {\"ok\": true}}";
        Json_Value *v;
        Json_Value *nested;
        i32 err = json_parse(&a, text, strlen(text), &v);
        TEST_CHECK(err == JSON_ESUCCESS && v->type == JSON_OBJECT, "object parses");
        TEST_CHECK(sv_eq_cstr_(json_object_get(v, "name")->string, "vecindex"), "string member correct");
        TEST_CHECK(json_object_get(v, "n")->number == 3.0, "number member correct");
        nested = json_object_get(v, "nested");
        TEST_CHECK(nested && nested->type == JSON_OBJECT, "nested object member correct");
        TEST_CHECK(json_object_get(nested, "ok")->boolean == 1, "value inside the nested object correct");
        TEST_CHECK(json_object_get(v, "missing") == 0, "missing key returns NULL, not garbage");
    }

    {
        const char *text = "{\"a\": }";
        Json_Value *v;
        i32 err = json_parse(&a, text, strlen(text), &v);
        TEST_CHECK(err == JSON_EUNEXPECTED, "malformed value after ':' is rejected cleanly, not crashed on");
    }

    {
        const char *text = "[1, 2";
        Json_Value *v;
        i32 err = json_parse(&a, text, strlen(text), &v);
        TEST_CHECK(err == JSON_EUNCLOSED, "unclosed array is rejected cleanly");
    }

    {
        const char *text = "1 2";
        Json_Value *v;
        i32 err = json_parse(&a, text, strlen(text), &v);
        TEST_CHECK(err == JSON_ETRAILING, "trailing content after the top-level value is rejected");
    }

    {
        const char *text = "\"\\uD800\"";
        Json_Value *v;
        i32 err = json_parse(&a, text, strlen(text), &v);
        TEST_CHECK(err == JSON_EBADESCAPE, "an unpaired surrogate escape is a documented, clean rejection (see file header)");
    }

    print_flush();
    return test_failures_ != 0;
}
