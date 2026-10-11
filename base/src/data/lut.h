#ifndef HOLYC_LUT_H_
#define HOLYC_LUT_H_

/* Generic runtime-built lookup table - a real, different problem from
 * xmacro.h/fabric.h (data, not code generation): build a table of N
 * entries once by calling a generator function per index (a
 * precomputed sin/cos table, a gamma-correction curve, a CRC table -
 * the "pay the compute cost once, O(1) lookup forever" trade this
 * project already leans on elsewhere - vecindex_4b.h's row/col LUT
 * decomposition, bdf.h's hex-digit LUT - made a first-class, reusable
 * pattern here instead of hand-rolled per call site.
 *
 * Lookup clamps an out-of-range index via bit.h's branchless clamp_i32
 * instead of an if-guard - a LUT's whole point is being the FAST path,
 * so a data-dependent, poorly-predicted bounds branch on every single
 * lookup would undercut half the reason to have a LUT at all. Callers
 * that need to distinguish "clamped" from "exact" should check the
 * index themselves before calling; this always returns SOME value,
 * never fails, matching how every other LUT in this codebase already
 * behaves (a background-grid or hash-table lookup landing outside the
 * table is a real bug elsewhere; here it's an explicit, documented
 * clamp-to-nearest-valid-entry, not silent corruption). */

#include "core/base.h"

typedef f32 (*Lut_F32_Gen_Fn)(i32 index, void *ctx);
typedef u8  (*Lut_U8_Gen_Fn)(i32 index, void *ctx);

typedef struct {
    f32 *data;
    i32 count;
} Lut_F32;

typedef struct {
    u8 *data;
    i32 count;
} Lut_U8;

/* storage: caller-owned, >= count entries. Calls gen(i, ctx) for every
 * index in [0, count) to fill it - a one-time up-front cost, same
 * shape as any other "build once" step in this codebase (arena
 * regions, IVF training, calibration tables). */
void lut_f32_build(Lut_F32 *out, f32 *storage, i32 count, Lut_F32_Gen_Fn gen, void *ctx);
void lut_u8_build(Lut_U8 *out, u8 *storage, i32 count, Lut_U8_Gen_Fn gen, void *ctx);

f32 lut_f32_get(const Lut_F32 *lut, i32 index); /* branchless-clamped index */
u8  lut_u8_get(const Lut_U8 *lut, i32 index);   /* branchless-clamped index */

#endif /* HOLYC_LUT_H_ */

#ifdef LUT_IMPLEMENTATION
#ifndef LUT_IMPLEMENTATION_
#define LUT_IMPLEMENTATION_

void lut_f32_build(Lut_F32 *out, f32 *storage, i32 count, Lut_F32_Gen_Fn gen, void *ctx)
{
    i32 i;
    out->data = storage;
    out->count = count;
    for (i = 0; i < count; i++) storage[i] = gen(i, ctx);
}

void lut_u8_build(Lut_U8 *out, u8 *storage, i32 count, Lut_U8_Gen_Fn gen, void *ctx)
{
    i32 i;
    out->data = storage;
    out->count = count;
    for (i = 0; i < count; i++) storage[i] = gen(i, ctx);
}

f32 lut_f32_get(const Lut_F32 *lut, i32 index)
{
    return lut->data[clamp_i32(index, 0, lut->count - 1)];
}

u8 lut_u8_get(const Lut_U8 *lut, i32 index)
{
    return lut->data[clamp_i32(index, 0, lut->count - 1)];
}

#endif /* LUT_IMPLEMENTATION_ */
#endif /* LUT_IMPLEMENTATION */
