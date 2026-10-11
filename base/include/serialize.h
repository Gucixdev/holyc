#ifndef HOLYC_SERIALIZE_H_
#define HOLYC_SERIALIZE_H_

/* Generic binary reader/writer over a caller-owned buffer: fixed-
 * width integers/floats in little-endian, plus raw byte runs -
 * exactly what turning an in-memory struct into bytes (and back)
 * needs, independent of where those bytes end up (file, network,
 * wherever) - that's the "generic" distinction from src/codec/'s own
 * format-specific encoders (wav.h, qoi.h etc), which each parse
 * a whole named FILE FORMAT; this file has no format of its own, it's
 * the primitive those could be built on. Every write/read here is
 * bounds-checked and returns 0 (leaving the writer/reader position
 * untouched) on failure rather than overrunning the buffer. */

#include "base.h"

typedef struct {
    u8 *data;
    size_t cap;
    size_t len; /* bytes written so far */
} Serialize_Writer;

typedef struct {
    const u8 *data;
    size_t len;
    size_t pos; /* bytes read so far */
} Serialize_Reader;

void serialize_writer_init(Serialize_Writer *w, u8 *buf, size_t cap);

i32 serialize_write_u8(Serialize_Writer *w, u8 v);
i32 serialize_write_u16(Serialize_Writer *w, u16 v);
i32 serialize_write_u32(Serialize_Writer *w, u32 v);
i32 serialize_write_u64(Serialize_Writer *w, u64 v);
i32 serialize_write_f32(Serialize_Writer *w, f32 v);
i32 serialize_write_f64(Serialize_Writer *w, f64 v);
i32 serialize_write_bytes(Serialize_Writer *w, const void *data, size_t n);

void serialize_reader_init(Serialize_Reader *r, const u8 *buf, size_t len);

i32 serialize_read_u8(Serialize_Reader *r, u8 *out);
i32 serialize_read_u16(Serialize_Reader *r, u16 *out);
i32 serialize_read_u32(Serialize_Reader *r, u32 *out);
i32 serialize_read_u64(Serialize_Reader *r, u64 *out);
i32 serialize_read_f32(Serialize_Reader *r, f32 *out);
i32 serialize_read_f64(Serialize_Reader *r, f64 *out);
i32 serialize_read_bytes(Serialize_Reader *r, void *out, size_t n);

#endif /* HOLYC_SERIALIZE_H_ */

#ifdef SERIALIZE_IMPLEMENTATION
#ifndef SERIALIZE_IMPLEMENTATION_
#define SERIALIZE_IMPLEMENTATION_

void serialize_writer_init(Serialize_Writer *w, u8 *buf, size_t cap)
{
    w->data = buf;
    w->cap = cap;
    w->len = 0;
}

i32 serialize_write_u8(Serialize_Writer *w, u8 v)
{
    if (w->len + 1 > w->cap) return 0;
    w->data[w->len] = v;
    w->len += 1;
    return 1;
}

i32 serialize_write_u16(Serialize_Writer *w, u16 v)
{
    if (w->len + 2 > w->cap) return 0;
    w->data[w->len + 0] = (u8)(v & 0xFF);
    w->data[w->len + 1] = (u8)((v >> 8) & 0xFF);
    w->len += 2;
    return 1;
}

i32 serialize_write_u32(Serialize_Writer *w, u32 v)
{
    if (w->len + 4 > w->cap) return 0;
    w->data[w->len + 0] = (u8)(v & 0xFF);
    w->data[w->len + 1] = (u8)((v >> 8) & 0xFF);
    w->data[w->len + 2] = (u8)((v >> 16) & 0xFF);
    w->data[w->len + 3] = (u8)((v >> 24) & 0xFF);
    w->len += 4;
    return 1;
}

i32 serialize_write_u64(Serialize_Writer *w, u64 v)
{
    i32 i;
    if (w->len + 8 > w->cap) return 0;
    for (i = 0; i < 8; i++) {
        w->data[w->len + i] = (u8)((v >> (i * 8)) & 0xFF);
    }
    w->len += 8;
    return 1;
}

i32 serialize_write_f32(Serialize_Writer *w, f32 v)
{
    union { f32 f; u32 u; } bits;
    bits.f = v;
    return serialize_write_u32(w, bits.u);
}

i32 serialize_write_f64(Serialize_Writer *w, f64 v)
{
    union { f64 f; u64 u; } bits;
    bits.f = v;
    return serialize_write_u64(w, bits.u);
}

i32 serialize_write_bytes(Serialize_Writer *w, const void *data, size_t n)
{
    const u8 *src = (const u8 *)data;
    size_t i;
    if (w->len + n > w->cap) return 0;
    for (i = 0; i < n; i++) w->data[w->len + i] = src[i];
    w->len += n;
    return 1;
}

void serialize_reader_init(Serialize_Reader *r, const u8 *buf, size_t len)
{
    r->data = buf;
    r->len = len;
    r->pos = 0;
}

i32 serialize_read_u8(Serialize_Reader *r, u8 *out)
{
    if (r->pos + 1 > r->len) return 0;
    *out = r->data[r->pos];
    r->pos += 1;
    return 1;
}

i32 serialize_read_u16(Serialize_Reader *r, u16 *out)
{
    if (r->pos + 2 > r->len) return 0;
    *out = (u16)(r->data[r->pos + 0] | ((u16)r->data[r->pos + 1] << 8));
    r->pos += 2;
    return 1;
}

i32 serialize_read_u32(Serialize_Reader *r, u32 *out)
{
    if (r->pos + 4 > r->len) return 0;
    *out = (u32)r->data[r->pos + 0]
         | ((u32)r->data[r->pos + 1] << 8)
         | ((u32)r->data[r->pos + 2] << 16)
         | ((u32)r->data[r->pos + 3] << 24);
    r->pos += 4;
    return 1;
}

i32 serialize_read_u64(Serialize_Reader *r, u64 *out)
{
    i32 i;
    u64 v = 0;
    if (r->pos + 8 > r->len) return 0;
    for (i = 0; i < 8; i++) {
        v |= ((u64)r->data[r->pos + i]) << (i * 8);
    }
    *out = v;
    r->pos += 8;
    return 1;
}

i32 serialize_read_f32(Serialize_Reader *r, f32 *out)
{
    union { f32 f; u32 u; } bits;
    if (!serialize_read_u32(r, &bits.u)) return 0;
    *out = bits.f;
    return 1;
}

i32 serialize_read_f64(Serialize_Reader *r, f64 *out)
{
    union { f64 f; u64 u; } bits;
    if (!serialize_read_u64(r, &bits.u)) return 0;
    *out = bits.f;
    return 1;
}

i32 serialize_read_bytes(Serialize_Reader *r, void *out, size_t n)
{
    u8 *dst = (u8 *)out;
    size_t i;
    if (r->pos + n > r->len) return 0;
    for (i = 0; i < n; i++) dst[i] = r->data[r->pos + i];
    r->pos += n;
    return 1;
}

#endif /* SERIALIZE_IMPLEMENTATION_ */
#endif /* SERIALIZE_IMPLEMENTATION */
