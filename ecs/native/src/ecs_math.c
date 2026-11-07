/* ecs_math.c - see ecs_math.h. Plain loops; ECS_KERNEL turns each into per-ISA clones with load-time dispatch on x86-64. */
#include "ecs_math.h"

#include <math.h>


#define N8(n) ((n) & ~(size_t)7)

ECS_KERNEL void ecs_f32_fill(float *dst, float v, size_t n) { for (size_t i = 0; i < n; i++) dst[i] = v; }
ECS_KERNEL void ecs_f32_add(float *dst, const float *a, const float *b, size_t n) { for (size_t i = 0; i < n; i++) dst[i] = a[i] + b[i]; }
ECS_KERNEL void ecs_f32_sub(float *dst, const float *a, const float *b, size_t n) { for (size_t i = 0; i < n; i++) dst[i] = a[i] - b[i]; }
ECS_KERNEL void ecs_f32_mul(float *dst, const float *a, const float *b, size_t n) { for (size_t i = 0; i < n; i++) dst[i] = a[i] * b[i]; }
ECS_KERNEL void ecs_f32_scale(float *dst, const float *a, float s, size_t n) { for (size_t i = 0; i < n; i++) dst[i] = a[i] * s; }
ECS_KERNEL void ecs_f32_axpy(float *dst, const float *x, float s, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		float m = x[i] * s;
		dst[i] = dst[i] + m;
	}
}
ECS_KERNEL void ecs_f32_clamp(float *dst, const float *a, float lo, float hi, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		float v = a[i];
		v = v < lo ? lo : v;
		dst[i] = v > hi ? hi : v;
	}
}
ECS_KERNEL void ecs_f32_clamp_arr(float *dst, const float *a, const float *lo, const float *hi, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		float v = a[i];
		v = v < lo[i] ? lo[i] : v;
		dst[i] = v > hi[i] ? hi[i] : v;
	}
}
ECS_KERNEL void ecs_f32_lerp(float *dst, const float *a, const float *b, float t, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		float d = b[i] - a[i];
		float m = d * t;
		dst[i] = a[i] + m;
	}
}
ECS_KERNEL void ecs_f32_min(float *dst, const float *a, const float *b, size_t n) { for (size_t i = 0; i < n; i++) dst[i] = a[i] < b[i] ? a[i] : b[i]; }
ECS_KERNEL void ecs_f32_max(float *dst, const float *a, const float *b, size_t n) { for (size_t i = 0; i < n; i++) dst[i] = a[i] > b[i] ? a[i] : b[i]; }
ECS_KERNEL void ecs_f32_approach(float *dst, const float *target, float step, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		float d = target[i] - dst[i];
		float up = d < step ? d : step;                 /* d > 0: advance by min(d, step) */
		float dn = d > -step ? d : -step;               /* d < 0: advance by max(d, -step) */
		dst[i] = dst[i] + (d >= 0.0f ? up : dn);
	}
}

/* Reductions: 8 independent lane accumulators, combined in one fixed tree. Same arithmetic order on every ISA. */
static float reduce8_sum(const float l[8]) { return ((l[0] + l[4]) + (l[1] + l[5])) + ((l[2] + l[6]) + (l[3] + l[7])); }

ECS_KERNEL float ecs_f32_sum(const float *a, size_t n)
{
	float l[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
	size_t i = 0, m = N8(n);
	for (; i < m; i += 8)
		for (int k = 0; k < 8; k++) l[k] += a[i + k];
	float s = reduce8_sum(l);
	for (; i < n; i++) s += a[i];
	return s;
}
ECS_KERNEL float ecs_f32_dot(const float *a, const float *b, size_t n)
{
	float l[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
	size_t i = 0, m = N8(n);
	for (; i < m; i += 8)
		for (int k = 0; k < 8; k++) {
			float p = a[i + k] * b[i + k];
			l[k] += p;
		}
	float s = reduce8_sum(l);
	for (; i < n; i++) {
		float p = a[i] * b[i];
		s += p;
	}
	return s;
}
ECS_KERNEL float ecs_f32_min_value(const float *a, size_t n)
{
	float r = INFINITY;
	for (size_t i = 0; i < n; i++) r = a[i] < r ? a[i] : r;
	return r;
}
ECS_KERNEL float ecs_f32_max_value(const float *a, size_t n)
{
	float r = -INFINITY;
	for (size_t i = 0; i < n; i++) r = a[i] > r ? a[i] : r;
	return r;
}

ECS_KERNEL size_t ecs_f32_mask_le(uint32_t *out, const float *a, float v, size_t n)
{
	uint32_t c = 0;
	for (size_t i = 0; i < n; i++) {
		uint32_t m = a[i] <= v ? 0xFFFFFFFFu : 0u;
		out[i] = m;
		c += m & 1u;
	}
	return c;
}
size_t ecs_f32_select_le(uint32_t *out_idx, const float *a, float v, size_t n)
{
	size_t c = 0;
	for (size_t i = 0; i < n; i++) {
		out_idx[c] = (uint32_t)i;          /* branchless compaction: always write, advance only on a hit */
		c += (size_t)(a[i] <= v);
	}
	return c;
}

ECS_KERNEL void ecs_v3_integrate(float *px, float *py, float *pz, const float *vx, const float *vy, const float *vz, float dt, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		float dx = vx[i] * dt, dy = vy[i] * dt, dz = vz[i] * dt;
		px[i] = px[i] + dx;
		py[i] = py[i] + dy;
		pz[i] = pz[i] + dz;
	}
}
ECS_KERNEL void ecs_v3_dist2_to(float *out, const float *px, const float *py, const float *pz, float cx, float cy, float cz, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		float dx = px[i] - cx, dy = py[i] - cy, dz = pz[i] - cz;
		float xx = dx * dx, yy = dy * dy, zz = dz * dz;
		out[i] = (xx + yy) + zz;
	}
}
ECS_KERNEL void ecs_v3_length(float *out, const float *x, const float *y, const float *z, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		float xx = x[i] * x[i], yy = y[i] * y[i], zz = z[i] * z[i];
		out[i] = sqrtf((xx + yy) + zz);
	}
}
ECS_KERNEL void ecs_v3_normalize(float *x, float *y, float *z, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		float xx = x[i] * x[i], yy = y[i] * y[i], zz = z[i] * z[i];
		float l = sqrtf((xx + yy) + zz);
		float inv = l > 0.0f ? 1.0f / l : 0.0f;
		x[i] *= inv;
		y[i] *= inv;
		z[i] *= inv;
	}
}
size_t ecs_v3_within_radius(uint32_t *out_idx, const float *px, const float *py, const float *pz, float cx, float cy, float cz,
			    float radius, size_t n)
{
	float r2 = radius * radius;
	size_t c = 0;
	for (size_t i = 0; i < n; i++) {
		float dx = px[i] - cx, dy = py[i] - cy, dz = pz[i] - cz;
		float xx = dx * dx, yy = dy * dy, zz = dz * dz;
		out_idx[c] = (uint32_t)i;
		c += (size_t)(((xx + yy) + zz) <= r2);
	}
	return c;
}

float ecs_softcap(float level, float soft, float hard, float y0, float y_soft, float y_hard)
{
	if (level <= 0.0f) return y0;
	if (level <= soft) return ecs_lerpf(y0, y_soft, level / soft);
	if (level <= hard) return ecs_lerpf(y_soft, y_hard, (level - soft) / (hard - soft));
	float tail = (y_hard - y_soft) / (hard - soft) * 0.25f;      /* a quarter of the last segment's slope */
	return y_hard + (level - hard) * tail;
}
