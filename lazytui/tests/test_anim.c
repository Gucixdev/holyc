/* test_anim.c - the ODE-based animation: a lone spring converges to its target and reports rest, RK4 is stable at a huge dt,
 * the presets differ, and lt_anim drives several bound doubles at once. */
#include "lazytui.h"
#include <math.h>
#include <stdio.h>

static int fails;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); fails++; } } while (0)
static void identity_ode(double t, const double *in, double *out, int n, void *ctx) { (void)t; (void)ctx; for (int i = 0; i < n; i++) out[i] = in[i]; }

int main(void)
{
	/* a spring released towards 100 eventually settles there and reports at-rest */
	lt_spring sp = lt_spring_make(0, 300, 34.6);            /* roughly critically damped */
	sp.target = 100;
	CHECK(!lt_spring_at_rest(&sp), "freshly displaced, not at rest");
	for (int i = 0; i < 2000 && !lt_spring_at_rest(&sp); i++) lt_spring_step(&sp, 1.0 / 240.0);
	CHECK(lt_spring_at_rest(&sp), "settles within a generous number of steps");
	CHECK(fabs(sp.x - 100) < 0.01, "and lands close to the target");

	/* presets exist and differ from one another */
	lt_spring smooth = lt_spring_preset(0, "smooth"), snappy = lt_spring_preset(0, "snappy"), bouncy = lt_spring_preset(0, "bouncy");
	CHECK(smooth.k != snappy.k || smooth.c != snappy.c, "smooth and snappy are different springs");
	CHECK(bouncy.c < smooth.c, "bouncy is under-damped relative to the critically-damped presets");
	smooth.target = 1; snappy.target = 1; bouncy.target = 1;
	bool overshoot = false;
	for (int i = 0; i < 3000; i++) { lt_spring_step(&bouncy, 1.0 / 240.0); if (bouncy.x > 1.02) overshoot = true; }
	CHECK(overshoot, "the bouncy preset overshoots its target at least once");

	/* stability: a very large dt must not explode or produce NaN (the integrator substeps internally) */
	lt_spring wild = lt_spring_make(0, 500, 10);
	wild.target = 50;
	lt_spring_step(&wild, 2.0);
	CHECK(isfinite(wild.x) && isfinite(wild.v), "a huge dt does not blow up the state");

	/* raw RK4 on a known ODE: dy/dt = y, y(0)=1 -> y(1) ~= e */
	{
		double y = 1;
		for (int i = 0; i < 1000; i++) lt_ode_rk4(identity_ode, i * 0.001, &y, 1, 0.001, NULL);
		CHECK(fabs(y - M_E) < 0.01, "RK4 integrates dy/dt=y close to e over 1000 small steps");
	}

	/* lt_anim: several bound values move independently and report "still animating" until they settle */
	lt_anim an;
	lt_anim_init(&an);
	double vx = 0, vy = 0;
	int ix = lt_anim_bind(&an, &vx, lt_spring_preset(0, "smooth"));
	int iy = lt_anim_bind(&an, &vy, lt_spring_preset(0, "firm"));
	lt_anim_to(&an, ix, 10);
	lt_anim_to(&an, iy, -5);
	bool moving = false;
	for (int i = 0; i < 2000; i++) if (lt_anim_tick(&an, 1.0 / 60.0)) moving = true; else break;
	CHECK(moving, "lt_anim_tick reports movement while springs are in flight");
	CHECK(fabs(vx - 10) < 0.05 && fabs(vy - (-5)) < 0.05, "both bound values converge to their own targets");
	CHECK(!lt_anim_tick(&an, 1.0 / 60.0), "once settled, ticking reports no more movement");
	lt_anim_snap(&an, ix, 42);
	CHECK(fabs(vx - 42) < 1e-9, "snap jumps immediately, no animation");

	printf(fails ? "anim: %d failed\n" : "anim: ok\n", fails);
	return fails != 0;
}
