#ifndef HOLYC_VAT_H_
#define HOLYC_VAT_H_

/* VAT (Vertex Animation Texture) addressing: the standard technique
 * for baking mesh animation into a texture instead of a skeleton -
 * one texel per (vertex, frame) pair, vertex index along the texture's
 * width, frame index along its height, texel VALUE holding that
 * vertex's position (or a delta from rest) for that frame. This file
 * is only the UV-addressing math (matcap.h/paralaxmapping.h's own
 * "give the UV, sampling is the caller's job" pattern, same
 * directory-neighbor stance) - no texture/image type dependency. */

#include "base.h"
#include "mathx.h"

typedef struct {
    i32 texture_width;  /* must be >= vertex_count */
    i32 texture_height; /* must be >= frame_count */
} Vat_Layout;

/* +0.5 texel-center offset (the standard trick to sample exactly a
 * texel's own value rather than landing on a boundary between two,
 * which under bilinear filtering would blend in a neighbor's data). */
vec2 vat_uv(const Vat_Layout *layout, i32 vertex_index, i32 frame_index);

#endif /* HOLYC_VAT_H_ */

#ifdef VAT_IMPLEMENTATION
#ifndef VAT_IMPLEMENTATION_
#define VAT_IMPLEMENTATION_

vec2 vat_uv(const Vat_Layout *layout, i32 vertex_index, i32 frame_index)
{
    vec2 uv;
    uv.x = ((f32)vertex_index + 0.5f) / (f32)layout->texture_width;
    uv.y = ((f32)frame_index + 0.5f) / (f32)layout->texture_height;
    return uv;
}

#endif /* VAT_IMPLEMENTATION_ */
#endif /* VAT_IMPLEMENTATION */
