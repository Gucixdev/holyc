/* ecs_dynamics.c - see ecs_dynamics.h. */
#include "ecs_dynamics.h"

#include <math.h>
#include <string.h>

#include "ecs_math.h"

#define TWO_PI 6.28318530717958647692f

ecs_spring ecs_spring_make(float frequency, float zeta, float dt)
{
	ecs_spring s = { 1.0f, dt, 0.0f, 1.0f };                  /* no spring force: a free particle */
	if (dt <= 0.0f) { s.pv = 0.0f; return s; }
	if (frequency <= 0.0f) return s;
	if (zeta < 0.0f) zeta = 0.0f;
	const float w = TWO_PI * frequency;
	const float eps = 1e-4f;

	if (zeta > 1.0f + eps) {                                  /* over-damped: two real decay rates */
		const float a = w * sqrtf(zeta * zeta - 1.0f);
		const float r1 = -w * zeta + a, r2 = -w * zeta - a;
		const float e1 = expf(r1 * dt), e2 = expf(r2 * dt);
		const float inv = 1.0f / (r1 - r2);
		s.pp = (r1 * e2 - r2 * e1) * inv;
		s.pv = (e1 - e2) * inv;
		s.vp = r1 * r2 * (e2 - e1) * inv;
		s.vv = (r1 * e1 - r2 * e2) * inv;
	} else if (zeta < 1.0f - eps) {                           /* under-damped: decaying oscillation */
		const float wd = w * sqrtf(1.0f - zeta * zeta);
		const float e = expf(-zeta * w * dt);
		const float c = cosf(wd * dt), sn = sinf(wd * dt);
		const float zw_over_wd = zeta * w / wd;
		s.pp = e * (c + zw_over_wd * sn);
		s.pv = e * sn / wd;
		s.vp = -e * (w * w / wd) * sn;
		s.vv = e * (c - zw_over_wd * sn);
	} else {                                                  /* critically damped */
		const float e = expf(-w * dt);
		s.pp = e * (1.0f + w * dt);
		s.pv = e * dt;
		s.vp = -e * w * w * dt;
		s.vv = e * (1.0f - w * dt);
	}
	return s;
}

ecs_spring ecs_spring_make_smooth(float smooth_time, float dt)
{
	if (smooth_time < 0.0001f) smooth_time = 0.0001f;
	/* critical spring: 1 - (1 + w t) e^{-w t}; omega = 2/smooth_time is Unity's SmoothDamp convention (59% at smooth_time) */
	return ecs_spring_make((2.0f / smooth_time) / TWO_PI, 1.0f, dt);
}

ECS_KERNEL void ecs_spring_step(float *x, float *v, const float *target, size_t n, ecs_spring s)
{
	for (size_t i = 0; i < n; i++) {
		float t = target[i];
		float y = x[i] - t;
		float a = s.pp * y, b = s.pv * v[i];
		float c = s.vp * y, d = s.vv * v[i];
		x[i] = (a + b) + t;
		v[i] = c + d;
	}
}

ECS_KERNEL void ecs_spring_step_to(float *x, float *v, float t, size_t n, ecs_spring s)
{
	for (size_t i = 0; i < n; i++) {
		float y = x[i] - t;
		float a = s.pp * y, b = s.pv * v[i];
		float c = s.vp * y, d = s.vv * v[i];
		x[i] = (a + b) + t;
		v[i] = c + d;
	}
}

ECS_KERNEL void ecs_spring_step_clamped(float *x, float *v, const float *target, size_t n, ecs_spring s, float max_speed)
{
	const float m = max_speed > 0.0f ? max_speed : 3.4e38f;
	for (size_t i = 0; i < n; i++) {
		float t = target[i];
		float y = x[i] - t;
		float a = s.pp * y, b = s.pv * v[i];
		float c = s.vp * y, d = s.vv * v[i];
		float nv = c + d;
		nv = nv > m ? m : (nv < -m ? -m : nv);
		x[i] = (a + b) + t;
		v[i] = nv;
	}
}

ECS_KERNEL void ecs_spring_kick(float *v, size_t n, float impulse)
{
	for (size_t i = 0; i < n; i++) v[i] = v[i] + impulse;
}

ECS_KERNEL void ecs_slide_step(float *x, const float *target, size_t n, float rate, float dt)
{
	const float k = 1.0f - expf(-(rate > 0.0f ? rate : 0.0f) * (dt > 0.0f ? dt : 0.0f));
	for (size_t i = 0; i < n; i++) {
		float d = target[i] - x[i];
		float m = d * k;
		x[i] = x[i] + m;
	}
}

ECS_KERNEL void ecs_slide_step_to(float *x, float t, size_t n, float rate, float dt)
{
	const float k = 1.0f - expf(-(rate > 0.0f ? rate : 0.0f) * (dt > 0.0f ? dt : 0.0f));
	for (size_t i = 0; i < n; i++) {
		float d = t - x[i];
		float m = d * k;
		x[i] = x[i] + m;
	}
}

ecs_second_order ecs_second_order_make(float f, float z, float r)
{
	ecs_second_order p;
	if (f < 1e-4f) f = 1e-4f;
	const float pf = 3.14159265358979323846f * f;
	p.k1 = z / pf;
	p.k2 = 1.0f / ((TWO_PI * f) * (TWO_PI * f));
	p.k3 = r * z / (TWO_PI * f);
	return p;
}

ECS_KERNEL void ecs_second_order_step(float *y, float *yd, float *xp, const float *x, size_t n, ecs_second_order p, float dt)
{
	if (dt <= 0.0f) return;
	/* clamp k2 so a large dt cannot make the explicit step diverge */
	const float a = dt * dt * 0.5f + dt * p.k1 * 0.5f, b = dt * p.k1;
	float k2 = p.k2 > a ? p.k2 : a;
	k2 = k2 > b ? k2 : b;
	const float inv = 1.0f / k2;
	for (size_t i = 0; i < n; i++) {
		float xd = (x[i] - xp[i]) / dt;
		xp[i] = x[i];
		float ny = y[i] + dt * yd[i];
		float acc = ((x[i] + p.k3 * xd) - ny - p.k1 * yd[i]) * inv;
		y[i] = ny;
		yd[i] = yd[i] + dt * acc;
	}
}

void ecs_rk4_step(float *state, size_t n, float t, float dt, ecs_deriv_fn f, void *user)
{
	if (n == 0 || n > 64 || !f) return;
	float k1[64], k2[64], k3[64], k4[64], tmp[64];
	f(t, state, k1, n, user);
	for (size_t i = 0; i < n; i++) tmp[i] = state[i] + 0.5f * dt * k1[i];
	f(t + 0.5f * dt, tmp, k2, n, user);
	for (size_t i = 0; i < n; i++) tmp[i] = state[i] + 0.5f * dt * k2[i];
	f(t + 0.5f * dt, tmp, k3, n, user);
	for (size_t i = 0; i < n; i++) tmp[i] = state[i] + dt * k3[i];
	f(t + dt, tmp, k4, n, user);
	for (size_t i = 0; i < n; i++) {
		float inc = (k1[i] + 2.0f * k2[i]) + (2.0f * k3[i] + k4[i]);
		state[i] = state[i] + (dt / 6.0f) * inc;
	}
}
