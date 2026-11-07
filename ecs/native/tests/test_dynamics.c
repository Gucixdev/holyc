/* test_dynamics.c - the closed-form spring against the analytic solution, stability at absurd dt, composition (N small steps == one
 * big step), the three damping regimes, smooth damp / slide / second-order behaviour, RK4 against a known solution, and
 * bit-identical batch vs scalar. */
#include "ecs_dynamics.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); fails++; } } while (0)
#define PI 3.14159265358979f

/* analytic y(t) of x'' + 2*z*w*x' + w^2*x = 0 from y0, v0 (double precision reference) */
static double analytic(double w, double z, double y0, double v0, double t)
{
	if (z < 1.0 - 1e-9) {
		double wd = w * sqrt(1 - z * z), a = y0, b = (v0 + z * w * y0) / wd;
		return exp(-z * w * t) * (a * cos(wd * t) + b * sin(wd * t));
	}
	if (z > 1.0 + 1e-9) {
		double al = w * sqrt(z * z - 1), r1 = -w * z + al, r2 = -w * z - al;
		double A = (v0 - r2 * y0) / (r1 - r2), B = (r1 * y0 - v0) / (r1 - r2);
		return A * exp(r1 * t) + B * exp(r2 * t);
	}
	return (y0 + (v0 + w * y0) * t) * exp(-w * t);
}

static void sho(float t, const float *y, float *out, size_t n, void *u)
{
	(void)t; (void)n;
	float w = *(float *)u;
	out[0] = y[1];
	out[1] = -w * w * y[0];
}

int main(void)
{
	/* ---- matches the analytic solution in all three regimes ---- */
	const float zetas[] = { 0.0f, 0.2f, 0.6f, 1.0f, 1.5f, 4.0f };
	for (size_t zi = 0; zi < sizeof zetas / sizeof *zetas; zi++) {
		const float z = zetas[zi], f = 2.0f, dt = 1.0f / 60.0f;
		const double w = 2.0 * PI * f;
		float x = 3.0f, v = -1.5f, target = 1.0f;
		ecs_spring s = ecs_spring_make(f, z, dt);
		double worst = 0;
		for (int step = 1; step <= 120; step++) {
			ecs_spring_step_to(&x, &v, target, 1, s);
			double ref = analytic(w, z, 3.0 - 1.0, -1.5, step * (double)dt) + 1.0;
			double e = fabs(ref - x);
			if (e > worst) worst = e;
		}
		CHECK(worst < 2e-3, "spring tracks the analytic solution (3 regimes)");
	}

	/* ---- unconditionally stable ---- */
	{
		const float dts[] = { 0.001f, 0.016f, 0.1f, 1.0f, 10.0f, 1000.0f };
		for (size_t i = 0; i < sizeof dts / sizeof *dts; i++) {
			float x = 100.0f, v = 500.0f;
			ecs_spring s = ecs_spring_make(8.0f, 0.4f, dts[i]);
			for (int k = 0; k < 50; k++) ecs_spring_step_to(&x, &v, 0.0f, 1, s);
			CHECK(isfinite(x) && isfinite(v) && fabsf(x) < 1000.0f, "stays finite and bounded at any dt");
		}
		/* where the naive integrator blows up, ours settles */
		float x = 1.0f, v = 0.0f;
		ecs_spring s = ecs_spring_make(5.0f, 0.5f, 0.5f);       /* a 500 ms hitch with a 5 Hz spring */
		for (int k = 0; k < 40; k++) ecs_spring_step_to(&x, &v, 0.0f, 1, s);
		CHECK(fabsf(x) < 1e-3f, "settles through a long frame");
	}

	/* ---- frame-rate independence: 60 x (1/60) == 1 x (1) ---- */
	{
		float xa = 4.0f, va = 2.0f, xb = 4.0f, vb = 2.0f;
		ecs_spring sa = ecs_spring_make(1.7f, 0.35f, 1.0f / 60.0f), sb = ecs_spring_make(1.7f, 0.35f, 1.0f);
		for (int k = 0; k < 60; k++) ecs_spring_step_to(&xa, &va, -1.0f, 1, sa);
		ecs_spring_step_to(&xb, &vb, -1.0f, 1, sb);
		CHECK(fabsf(xa - xb) < 2e-3f && fabsf(va - vb) < 2e-2f, "60 small steps equal one big step (the exact solution composes)");
	}

	/* ---- damping character ---- */
	{
		float x = 0, v = 0;
		ecs_spring s = ecs_spring_make(3.0f, 0.2f, 1.0f / 120.0f);
		float peak = 0;
		for (int k = 0; k < 240; k++) { ecs_spring_step_to(&x, &v, 1.0f, 1, s); if (x > peak) peak = x; }
		CHECK(peak > 1.4f, "under-damped overshoots a lot");
		x = v = 0;
		s = ecs_spring_make(3.0f, 1.0f, 1.0f / 120.0f);
		peak = 0;
		for (int k = 0; k < 240; k++) { ecs_spring_step_to(&x, &v, 1.0f, 1, s); if (x > peak) peak = x; }
		CHECK(peak <= 1.0001f, "critically damped never overshoots");
		CHECK(fabsf(x - 1.0f) < 1e-3f, "and gets there");
		x = v = 0;
		s = ecs_spring_make(3.0f, 3.0f, 1.0f / 120.0f);
		for (int k = 0; k < 60; k++) ecs_spring_step_to(&x, &v, 1.0f, 1, s);
		float crit_x = 0, crit_v = 0;
		ecs_spring sc = ecs_spring_make(3.0f, 1.0f, 1.0f / 120.0f);
		for (int k = 0; k < 60; k++) ecs_spring_step_to(&crit_x, &crit_v, 1.0f, 1, sc);
		CHECK(x < crit_x, "over-damped is slower than critical");
		/* undamped keeps its energy */
		x = 1; v = 0;
		s = ecs_spring_make(2.0f, 0.0f, 1.0f / 60.0f);
		for (int k = 0; k < 600; k++) ecs_spring_step_to(&x, &v, 0.0f, 1, s);
		float w = 2.0f * PI * 2.0f, energy = 0.5f * v * v + 0.5f * w * w * x * x;
		CHECK(fabsf(energy - 0.5f * w * w) / (0.5f * w * w) < 1e-3f, "zeta 0 conserves energy over 10 s (no numerical damping)");
	}

	/* ---- smooth damp: settle time, max speed ---- */
	{
		float x = 0, v = 0;
		ecs_spring s = ecs_spring_make_smooth(0.3f, 1.0f / 60.0f);
		int s59 = -1, s95 = -1;
		for (int k = 1; k <= 120; k++) {
			ecs_spring_step_to(&x, &v, 1.0f, 1, s);
			if (s59 < 0 && x >= 0.594f) s59 = k;
			if (s95 < 0 && x >= 0.95f) s95 = k;
		}
		CHECK(s59 > 0 && fabsf(s59 / 60.0f - 0.3f) < 0.03f, "59% of the way after smooth_time (Unity SmoothDamp convention)");
		CHECK(s95 > 0 && fabsf(s95 / 60.0f - 0.3f * 2.37f) < 0.05f, "95% after ~2.4x smooth_time");
		x = 0; v = 0;
		float tgt = 100.0f, vmax = 0;
		for (int k = 0; k < 60; k++) {
			ecs_spring_step_clamped(&x, &v, &tgt, 1, s, 5.0f);
			if (fabsf(v) > vmax) vmax = fabsf(v);
		}
		CHECK(vmax <= 5.0001f, "max_speed caps the velocity");
	}

	/* ---- slide: never overshoots, frame-rate independent ---- */
	{
		float xa = 0, xb = 0, t = 10.0f;
		for (int k = 0; k < 60; k++) ecs_slide_step(&xa, &t, 1, 6.0f, 1.0f / 60.0f);
		ecs_slide_step(&xb, &t, 1, 6.0f, 1.0f);
		CHECK(fabsf(xa - xb) < 1e-3f, "slide gives the same result at any step size");
		CHECK(xa < t && xa > 9.9f, "approaches without crossing");
		float c = 0;
		ecs_slide_step_to(&c, 1.0f, 1, 1e6f, 1.0f);
		CHECK(c <= 1.0f && c > 0.999f, "even an enormous rate cannot overshoot");
	}

	/* ---- second order: anticipation and overshoot ---- */
	{
		ecs_second_order anticip = ecs_second_order_make(2.0f, 0.6f, -1.0f), eager = ecs_second_order_make(2.0f, 0.6f, 2.0f);
		float ya = 0, yda = 0, xpa = 0, yb = 0, ydb = 0, xpb = 0, in = 1.0f;
		float min_a = 0, max_b = 0;
		for (int k = 0; k < 180; k++) {
			ecs_second_order_step(&ya, &yda, &xpa, &in, 1, anticip, 1.0f / 60.0f);
			ecs_second_order_step(&yb, &ydb, &xpb, &in, 1, eager, 1.0f / 60.0f);
			if (ya < min_a) min_a = ya;
			if (yb > max_b) max_b = yb;
		}
		CHECK(min_a < -0.01f, "r < 0 dips the wrong way first (anticipation)");
		CHECK(max_b > 1.1f, "r > 1 overshoots");
		CHECK(fabsf(ya - 1.0f) < 0.02f && fabsf(yb - 1.0f) < 0.02f, "both settle on the input");
		float y = 0, yd = 0, xp = 0, big = 1.0f;
		ecs_second_order_step(&y, &yd, &xp, &big, 1, anticip, 5.0f);          /* a 5 s hitch must not blow up */
		CHECK(isfinite(y) && fabsf(y) < 100.0f, "second order is stable through a huge dt");
	}

	/* ---- RK4 vs analytic harmonic oscillator ---- */
	{
		float w = 3.0f, st[2] = { 1.0f, 0.0f }, t = 0, dt = 0.01f;
		for (int k = 0; k < 300; k++) { ecs_rk4_step(st, 2, t, dt, sho, &w); t += dt; }
		CHECK(fabsf(st[0] - cosf(w * t)) < 1e-4f && fabsf(st[1] + w * sinf(w * t)) < 1e-3f, "RK4 follows cos(wt) closely");
	}

	/* ---- batch == scalar, bit for bit; shared and per-element targets agree ---- */
	{
		enum { N = 37 };
		float xb[N], vb[N], tb[N], xs[N], vs[N];
		for (int i = 0; i < N; i++) { xb[i] = xs[i] = (float)(i * 3 % 11) - 5.0f; vb[i] = vs[i] = (float)(i % 7) - 3.0f; tb[i] = 2.0f; }
		ecs_spring s = ecs_spring_make(2.5f, 0.45f, 1.0f / 60.0f);
		for (int k = 0; k < 30; k++) {
			ecs_spring_step(xb, vb, tb, N, s);
			for (int i = 0; i < N; i++) ecs_spring_step_to(&xs[i], &vs[i], 2.0f, 1, s);
		}
		CHECK(memcmp(xb, xs, sizeof xb) == 0 && memcmp(vb, vs, sizeof vb) == 0, "batch and scalar are bit-identical");
	}

	/* ---- edge parameters ---- */
	{
		float x = 5, v = 0;
		ecs_spring s = ecs_spring_make(0.0f, 0.5f, 0.1f);
		ecs_spring_step_to(&x, &v, 0.0f, 1, s);
		CHECK(x == 5.0f, "zero frequency: no force, a still particle stays still");
		x = 5; v = 10;
		ecs_spring_step_to(&x, &v, 0.0f, 1, s);
		CHECK(fabsf(x - 6.0f) < 1e-5f, "zero frequency: a moving particle coasts");
		x = 5; v = 0;
		s = ecs_spring_make(2.0f, 1.0f, 0.0f);
		ecs_spring_step_to(&x, &v, 0.0f, 1, s);
		CHECK(x == 5.0f && v == 0.0f, "dt = 0 changes nothing");
		ecs_spring_kick(&v, 1, 3.0f);
		CHECK(v == 3.0f, "kick adds velocity");
	}

	if (fails) { printf("%d FAILED\n", fails); return 1; }
	printf("test_dynamics: ok\n");
	return 0;
}
