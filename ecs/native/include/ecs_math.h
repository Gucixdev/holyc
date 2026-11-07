/* ecs_math.h - bulk float math for the columns an ECS iterates: add/scale/clamp/lerp/approach, reductions, and SoA vec3 kernels.
 *
 * DESIGN
 *  - Everything is a plain loop over contiguous float arrays. There are no hand-written intrinsics: the kernels are compiled once per
 *    ISA level (x86-64: avx2+fma / sse4.2 / baseline, via GCC/Clang target_clones - the right one is picked at load time) and the
 *    compiler vectorizes them. Other targets (NEON, WASM, MSVC) get the same loops vectorized for whatever the build targets, or scalar.
 *  - DETERMINISTIC: the library is built with -ffp-contract=off (no silent a*b+c fusing) and every reduction uses a fixed 8-lane
 *    accumulator layout, so avx2, sse4.2 and scalar produce BIT-IDENTICAL results. That matters for replays, netcode and tests that
 *    compare against a reference - do not "optimize" it with -ffast-math.
 *  - Arrays may alias exactly (dst == a is fine for the elementwise ops, that's how you update in place) but must not partially overlap.
 *  - Alignment is not required (loads are unaligned-safe), but columns from this library are 32-byte aligned and padded to a multiple
 *    of ECS_LANES floats, which lets the vector body run with no scalar tail on aligned data.
 *  - n is an element count (not bytes). n == 0 is a no-op. NaNs propagate; nothing here traps. */
#ifndef ECS_MATH_H
#define ECS_MATH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Marks a function as a SIMD kernel: on x86-64/Linux the compiler emits avx2+fma / sse4.2 / baseline clones and picks one at load time.
 * Elsewhere (or with ECS_NO_MULTIVERSION) it expands to nothing and the build's own target flags apply. Use it on your own hot loops. */
#if defined(__GNUC__) && defined(__x86_64__) && !defined(ECS_NO_MULTIVERSION) && defined(__linux__)
#define ECS_KERNEL __attribute__((target_clones("avx2,fma", "sse4.2", "default")))
#else
#define ECS_KERNEL
#endif

#define ECS_LANES 8          /* floats per 32-byte vector: the padding/alignment unit for SIMD columns */
#define ECS_SIMD_ALIGN 32

/* ---- elementwise: dst[i] = ... ------------------------------------------------------------------------------------------------- */
void ecs_f32_fill(float *dst, float v, size_t n);
void ecs_f32_add(float *dst, const float *a, const float *b, size_t n);
void ecs_f32_sub(float *dst, const float *a, const float *b, size_t n);
void ecs_f32_mul(float *dst, const float *a, const float *b, size_t n);
void ecs_f32_scale(float *dst, const float *a, float s, size_t n);               /* dst = a * s */
void ecs_f32_axpy(float *dst, const float *x, float s, size_t n);                /* dst += x * s  (mul then add, never fused) */
void ecs_f32_clamp(float *dst, const float *a, float lo, float hi, size_t n);
void ecs_f32_clamp_arr(float *dst, const float *a, const float *lo, const float *hi, size_t n);
void ecs_f32_lerp(float *dst, const float *a, const float *b, float t, size_t n);
void ecs_f32_min(float *dst, const float *a, const float *b, size_t n);
void ecs_f32_max(float *dst, const float *a, const float *b, size_t n);
/* Move each dst[i] toward target[i] by at most step, never overshooting (the regen / chase / ease-out primitive). */
void ecs_f32_approach(float *dst, const float *target, float step, size_t n);

/* ---- reductions: fixed 8-lane order, bit-identical on every ISA ---------------------------------------------------------------- */
float ecs_f32_sum(const float *a, size_t n);
float ecs_f32_dot(const float *a, const float *b, size_t n);
float ecs_f32_min_value(const float *a, size_t n);        /* +INFINITY for n == 0 */
float ecs_f32_max_value(const float *a, size_t n);        /* -INFINITY for n == 0 */

/* ---- masks / compaction -------------------------------------------------------------------------------------------------------- */
/* out[i] = (a[i] <= v) ? 0xFFFFFFFF : 0. Returns how many matched. */
size_t ecs_f32_mask_le(uint32_t *out, const float *a, float v, size_t n);
/* Writes the indices i where a[i] <= v to out (capacity >= n), in ascending order; returns the count. */
size_t ecs_f32_select_le(uint32_t *out_idx, const float *a, float v, size_t n);

/* ---- SoA vec3 (x[], y[], z[] are separate columns) ------------------------------------------------------------------------------ */
void ecs_v3_integrate(float *px, float *py, float *pz, const float *vx, const float *vy, const float *vz, float dt, size_t n);
void ecs_v3_dist2_to(float *out, const float *px, const float *py, const float *pz, float cx, float cy, float cz, size_t n);
void ecs_v3_length(float *out, const float *x, const float *y, const float *z, size_t n);
void ecs_v3_normalize(float *x, float *y, float *z, size_t n);        /* zero-length stays zero */
/* Indices of points within `radius` of c, ascending, into out_idx (capacity >= n). Returns the count. */
size_t ecs_v3_within_radius(uint32_t *out_idx, const float *px, const float *py, const float *pz, float cx, float cy, float cz,
			    float radius, size_t n);

/* ---- scalar helpers (inline: cheap, no dispatch) -------------------------------------------------------------------------------- */
static inline float ecs_clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline float ecs_lerpf(float a, float b, float t) { return a + (b - a) * t; }
static inline float ecs_smoothstepf(float e0, float e1, float x)
{
	float t = ecs_clampf((x - e0) / (e1 - e0), 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}
/* Piecewise-linear soft-cap curve - the shape of every "levels -> stat" table in a souls-like: fast gains up to `soft`, slower up to
 * `hard`, almost flat after. y(0)=y0, y(soft)=y_soft, y(hard)=y_hard, then slope continues at 1/4 of the last segment's. */
float ecs_softcap(float level, float soft, float hard, float y0, float y_soft, float y_hard);

#ifdef __cplusplus
}
#endif
#endif
