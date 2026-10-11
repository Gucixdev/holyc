#ifndef HOLYC_JSON_H_
#define HOLYC_JSON_H_

/* RFC 8259 JSON parser (a real, working subset - objects, arrays,
 * strings with all 8 standard escapes including \uXXXX, numbers as f64,
 * true/false/null). Parser only, no writer - a caller that has a
 * Json_Value tree and wants to serialize it back out is a separate,
 * independent module if/when something needs it (same "encode-only/
 * decode-only is a legitimate scope" stance as base64_encode.h/
 * base63_decode.h in this same directory).
 *
 * \uXXXX handling is BMP-only (each \uXXXX becomes one decoded UTF-8
 * sequence for that single 16-bit code point) - surrogate PAIRS
 * (\uD800-\uDFFF, used for astral characters outside the BMP) are a
 * documented gap: JSON_EBADESCAPE on an unpaired or paired surrogate
 * escape, rather than silently mis-decoding it. Real-world JSON from
 * astral-plane-heavy sources (emoji, some CJK extensions) can hit this;
 * everything else is unaffected. */

#include "types/base.h"
#include "alloc/arena_alloc.h"
#include "util/str.h"

typedef enum {
    JSON_ESUCCESS     = 0,
    JSON_EUNEXPECTED  = -1, /* an unexpected character where a value/token was expected */
    JSON_EUNCLOSED    = -2, /* an object/array/string never found its closing token */
    JSON_EBADNUMBER   = -3,
    JSON_EBADESCAPE   = -4, /* bad \escape, including unsupported surrogate pairs - see file header */
    JSON_ETRAILING    = -5  /* non-whitespace content after the top-level value */
} Json_Error;

typedef enum {
    JSON_NULL = 0,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} Json_Type;

typedef struct Json_Value Json_Value;

typedef struct {
    String_View key;
    Json_Value *value;
} Json_Member;

typedef struct {
    Json_Value **items;
    size_t count;
    size_t capacity;
} Json_Array_List_;

typedef struct {
    Json_Member *items;
    size_t count;
    size_t capacity;
} Json_Object_List_;

struct Json_Value {
    Json_Type type;
    i32 boolean;             /* JSON_BOOL */
    f64 number;               /* JSON_NUMBER */
    String_View string;       /* JSON_STRING */
    Json_Array_List_ array;   /* JSON_ARRAY */
    Json_Object_List_ object; /* JSON_OBJECT */
};

i32 json_parse(Arena *a, const char *data, size_t size, Json_Value **out);
const char *json_strerror(i32 err);

/* NULL if key isn't present or value isn't a JSON_OBJECT. */
Json_Value *json_object_get(const Json_Value *value, const char *key);

/* NULL if index is out of range or value isn't a JSON_ARRAY. */
Json_Value *json_array_get(const Json_Value *value, size_t index);

#endif /* HOLYC_JSON_H_ */

#ifdef JSON_IMPLEMENTATION
#ifndef JSON_IMPLEMENTATION_
#define JSON_IMPLEMENTATION_

#include <stdlib.h> /* strtod - parsing number tokens, this file's sole real need */

const char *json_strerror(i32 err)
{
    switch (err) {
    case JSON_ESUCCESS:    return "success";
    case JSON_EUNEXPECTED: return "unexpected character";
    case JSON_EUNCLOSED:   return "unclosed object/array/string";
    case JSON_EBADNUMBER:  return "malformed number";
    case JSON_EBADESCAPE:  return "bad string escape";
    case JSON_ETRAILING:   return "trailing content after the top-level value";
    default: break;
    }
    return "unknown error";
}

typedef struct {
    Arena *a;
    const char *p;
    const char *end;
    i32 err;
} Json_Parser_;

static void json_skip_ws_(Json_Parser_ *jp)
{
    while (jp->p < jp->end && (*jp->p == ' ' || *jp->p == '\t' || *jp->p == '\n' || *jp->p == '\r')) {
        jp->p++;
    }
}

static i32 json_hex_digit_(char c, u32 *out)
{
    if (c >= '0' && c <= '9') { *out = (u32)(c - '0'); return 1; }
    if (c >= 'a' && c <= 'f') { *out = (u32)(c - 'a' + 10); return 1; }
    if (c >= 'A' && c <= 'F') { *out = (u32)(c - 'A' + 10); return 1; }
    return 0;
}

static i32 json_parse_u16_escape_(Json_Parser_ *jp, u32 *out)
{
    u32 v = 0;
    i32 i;
    if (jp->end - jp->p < 4) return 0;
    for (i = 0; i < 4; i++) {
        u32 d;
        if (!json_hex_digit_(jp->p[i], &d)) return 0;
        v = (v << 4) | d;
    }
    jp->p += 4;
    *out = v;
    return 1;
}

/* appends the UTF-8 encoding of a BMP code point (< 0x10000, no
 * surrogate pairs - see file header) to buf at *n, advancing *n. */
static void json_append_utf8_(char *buf, size_t *n, u32 cp)
{
    if (cp < 0x80) {
        buf[(*n)++] = (char)cp;
    } else if (cp < 0x800) {
        buf[(*n)++] = (char)(0xC0 | (cp >> 6));
        buf[(*n)++] = (char)(0x80 | (cp & 0x3F));
    } else {
        buf[(*n)++] = (char)(0xE0 | (cp >> 12));
        buf[(*n)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[(*n)++] = (char)(0x80 | (cp & 0x3F));
    }
}

/* *jp->p must be at the opening quote. On success, advances past the
 * closing quote and fills *out. */
static i32 json_parse_string_(Json_Parser_ *jp, String_View *out)
{
    const char *content_start;
    const char *q;
    i32 has_escape = 0;
    char *buf;
    size_t n;

    jp->p++; /* opening quote */
    content_start = jp->p;
    q = content_start;
    while (q < jp->end && *q != '"') {
        if (*q == '\\') {
            has_escape = 1;
            q++;
            if (q >= jp->end) { jp->err = JSON_EUNCLOSED; return 0; }
        }
        q++;
    }
    if (q >= jp->end) { jp->err = JSON_EUNCLOSED; return 0; }

    if (!has_escape) {
        *out = sv_from_parts(content_start, (size_t)(q - content_start));
        jp->p = q + 1;
        return 1;
    }

    /* each \uXXXX can expand to up to 3 UTF-8 bytes from a 6-byte
     * escape, and every other escape shrinks 2 source bytes to 1 - the
     * raw source length is always a safe upper bound for the buffer. */
    buf = (char *)arena_alloc(jp->a, (size_t)(q - content_start));
    n = 0;
    jp->p = content_start;
    while (jp->p < q) {
        char c = *jp->p;
        if (c != '\\') {
            buf[n++] = c;
            jp->p++;
            continue;
        }
        jp->p++;
        switch (*jp->p) {
        case '"':  buf[n++] = '"';  jp->p++; break;
        case '\\': buf[n++] = '\\'; jp->p++; break;
        case '/':  buf[n++] = '/';  jp->p++; break;
        case 'b':  buf[n++] = '\b'; jp->p++; break;
        case 'f':  buf[n++] = '\f'; jp->p++; break;
        case 'n':  buf[n++] = '\n'; jp->p++; break;
        case 'r':  buf[n++] = '\r'; jp->p++; break;
        case 't':  buf[n++] = '\t'; jp->p++; break;
        case 'u': {
            u32 cp;
            jp->p++;
            if (!json_parse_u16_escape_(jp, &cp)) { jp->err = JSON_EBADESCAPE; return 0; }
            if (cp >= 0xD800 && cp <= 0xDFFF) { jp->err = JSON_EBADESCAPE; return 0; } /* surrogate pair - see file header */
            json_append_utf8_(buf, &n, cp);
            break;
        }
        default:
            jp->err = JSON_EBADESCAPE;
            return 0;
        }
    }

    *out = sv_from_parts(buf, n);
    jp->p = q + 1;
    return 1;
}

static i32 json_parse_number_(Json_Parser_ *jp, f64 *out)
{
    const char *start = jp->p;
    char buf[64];
    size_t n = 0;
    char *stop;

    if (jp->p < jp->end && *jp->p == '-') jp->p++;
    if (jp->p >= jp->end || *jp->p < '0' || *jp->p > '9') { jp->err = JSON_EBADNUMBER; return 0; }
    while (jp->p < jp->end && *jp->p >= '0' && *jp->p <= '9') jp->p++;
    if (jp->p < jp->end && *jp->p == '.') {
        jp->p++;
        if (jp->p >= jp->end || *jp->p < '0' || *jp->p > '9') { jp->err = JSON_EBADNUMBER; return 0; }
        while (jp->p < jp->end && *jp->p >= '0' && *jp->p <= '9') jp->p++;
    }
    if (jp->p < jp->end && (*jp->p == 'e' || *jp->p == 'E')) {
        jp->p++;
        if (jp->p < jp->end && (*jp->p == '+' || *jp->p == '-')) jp->p++;
        if (jp->p >= jp->end || *jp->p < '0' || *jp->p > '9') { jp->err = JSON_EBADNUMBER; return 0; }
        while (jp->p < jp->end && *jp->p >= '0' && *jp->p <= '9') jp->p++;
    }

    n = (size_t)(jp->p - start);
    if (n >= sizeof(buf)) { jp->err = JSON_EBADNUMBER; return 0; }
    arena_memcpy(buf, start, n);
    buf[n] = '\0';
    *out = strtod(buf, &stop);
    return 1;
}

static i32 json_parse_value_(Json_Parser_ *jp, Json_Value **out);

static i32 json_parse_array_(Json_Parser_ *jp, Json_Value *v)
{
    v->type = JSON_ARRAY;
    v->array.items = 0;
    v->array.count = 0;
    v->array.capacity = 0;

    jp->p++; /* '[' */
    json_skip_ws_(jp);
    if (jp->p < jp->end && *jp->p == ']') { jp->p++; return 1; }

    for (;;) {
        Json_Value *elem;
        json_skip_ws_(jp);
        if (!json_parse_value_(jp, &elem)) return 0;
        arena_da_append(jp->a, &v->array, elem);
        json_skip_ws_(jp);
        if (jp->p >= jp->end) { jp->err = JSON_EUNCLOSED; return 0; }
        if (*jp->p == ',') { jp->p++; continue; }
        if (*jp->p == ']') { jp->p++; return 1; }
        jp->err = JSON_EUNEXPECTED;
        return 0;
    }
}

static i32 json_parse_object_(Json_Parser_ *jp, Json_Value *v)
{
    v->type = JSON_OBJECT;
    v->object.items = 0;
    v->object.count = 0;
    v->object.capacity = 0;

    jp->p++; /* '{' */
    json_skip_ws_(jp);
    if (jp->p < jp->end && *jp->p == '}') { jp->p++; return 1; }

    for (;;) {
        Json_Member member;
        json_skip_ws_(jp);
        if (jp->p >= jp->end || *jp->p != '"') { jp->err = JSON_EUNEXPECTED; return 0; }
        if (!json_parse_string_(jp, &member.key)) return 0;
        json_skip_ws_(jp);
        if (jp->p >= jp->end || *jp->p != ':') { jp->err = JSON_EUNEXPECTED; return 0; }
        jp->p++;
        json_skip_ws_(jp);
        if (!json_parse_value_(jp, &member.value)) return 0;
        arena_da_append(jp->a, &v->object, member);
        json_skip_ws_(jp);
        if (jp->p >= jp->end) { jp->err = JSON_EUNCLOSED; return 0; }
        if (*jp->p == ',') { jp->p++; continue; }
        if (*jp->p == '}') { jp->p++; return 1; }
        jp->err = JSON_EUNEXPECTED;
        return 0;
    }
}

static i32 json_literal_(Json_Parser_ *jp, const char *lit, size_t lit_len)
{
    if ((size_t)(jp->end - jp->p) < lit_len) return 0;
    if (memcmp(jp->p, lit, lit_len) != 0) return 0;
    jp->p += lit_len;
    return 1;
}

static i32 json_parse_value_(Json_Parser_ *jp, Json_Value **out)
{
    Json_Value *v = (Json_Value *)arena_alloc(jp->a, sizeof(Json_Value));

    json_skip_ws_(jp);
    if (jp->p >= jp->end) { jp->err = JSON_EUNEXPECTED; return 0; }

    switch (*jp->p) {
    case '{': if (!json_parse_object_(jp, v)) return 0; break;
    case '[': if (!json_parse_array_(jp, v)) return 0; break;
    case '"':
        v->type = JSON_STRING;
        if (!json_parse_string_(jp, &v->string)) return 0;
        break;
    case 't':
        if (!json_literal_(jp, "true", 4)) { jp->err = JSON_EUNEXPECTED; return 0; }
        v->type = JSON_BOOL; v->boolean = 1;
        break;
    case 'f':
        if (!json_literal_(jp, "false", 5)) { jp->err = JSON_EUNEXPECTED; return 0; }
        v->type = JSON_BOOL; v->boolean = 0;
        break;
    case 'n':
        if (!json_literal_(jp, "null", 4)) { jp->err = JSON_EUNEXPECTED; return 0; }
        v->type = JSON_NULL;
        break;
    default:
        if (*jp->p == '-' || (*jp->p >= '0' && *jp->p <= '9')) {
            v->type = JSON_NUMBER;
            if (!json_parse_number_(jp, &v->number)) return 0;
        } else {
            jp->err = JSON_EUNEXPECTED;
            return 0;
        }
        break;
    }

    *out = v;
    return 1;
}

i32 json_parse(Arena *a, const char *data, size_t size, Json_Value **out)
{
    Json_Parser_ jp;
    jp.a = a;
    jp.p = data;
    jp.end = data + size;
    jp.err = JSON_ESUCCESS;

    if (!json_parse_value_(&jp, out)) return jp.err;

    json_skip_ws_(&jp);
    if (jp.p != jp.end) return JSON_ETRAILING;

    return JSON_ESUCCESS;
}

Json_Value *json_object_get(const Json_Value *value, const char *key)
{
    size_t key_len;
    size_t i;

    if (!value || value->type != JSON_OBJECT) return 0;
    key_len = strlen(key);
    for (i = 0; i < value->object.count; i++) {
        String_View k = value->object.items[i].key;
        if (k.count == key_len && memcmp(k.data, key, key_len) == 0) {
            return value->object.items[i].value;
        }
    }
    return 0;
}

Json_Value *json_array_get(const Json_Value *value, size_t index)
{
    if (!value || value->type != JSON_ARRAY) return 0;
    if (index >= value->array.count) return 0;
    return value->array.items[index];
}

#endif /* JSON_IMPLEMENTATION_ */
#endif /* JSON_IMPLEMENTATION */
