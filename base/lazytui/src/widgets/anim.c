/* anim.c - animation as ordinary differential equations. A value chases its target like a damped spring,
 * x'' = -k (x - target) - c x', integrated with RK4 in sub-steps no longer than 1/240 s: stable for any frame time, and
 * retargeting in mid flight keeps the velocity, so motion never jumps. lt_ode_rk4 is the general integrator that the
 * spring and any custom motion (a marquee phase, a pendulum, ...) share. */
#include "lazytui.h"
#include <math.h>
#include <string.h>

#define MAX_DIM 32
#define MAX_STEP (1.0 / 240.0)

void lt_ode_rk4(lt_ode_fn f, double t, double *y, int n, double dt, void *ctx)
{
	if (n > MAX_DIM) n = MAX_DIM;
	double k1[MAX_DIM], k2[MAX_DIM], k3[MAX_DIM], k4[MAX_DIM], tmp[MAX_DIM];
	f(t, y, k1, n, ctx);
	for (int i = 0; i < n; i++) tmp[i] = y[i] + 0.5 * dt * k1[i];
	f(t + 0.5 * dt, tmp, k2, n, ctx);
	for (int i = 0; i < n; i++) tmp[i] = y[i] + 0.5 * dt * k2[i];
	f(t + 0.5 * dt, tmp, k3, n, ctx);
	for (int i = 0; i < n; i++) tmp[i] = y[i] + dt * k3[i];
	f(t + dt, tmp, k4, n, ctx);
	for (int i = 0; i < n; i++) y[i] += dt / 6.0 * (k1[i] + 2 * k2[i] + 2 * k3[i] + k4[i]);
}

lt_spring lt_spring_make(double x, double k, double c) { return (lt_spring){x, 0, x, k, c}; }

lt_spring lt_spring_preset(double x, const char *name)
{
	if (name && !strcmp(name, "snappy")) return lt_spring_make(x, 400, 36);                 /* zeta 0.9: a hair of overshoot */
	if (name && !strcmp(name, "bouncy")) return lt_spring_make(x, 200, 8);                  /* zeta 0.28: clearly rings */
	if (name && !strcmp(name, "firm")) return lt_spring_make(x, 300, 2 * sqrt(300.0));                   /* fast and critically damped: no overshoot at all */
	if (name && !strcmp(name, "slow")) return lt_spring_make(x, 40, 2 * sqrt(40.0) * 1.1);
	return lt_spring_make(x, 120, 2 * sqrt(120.0));                                         /* smooth: critically damped */
}

static void spring_f(double t, const double *y, double *d, int n, void *ctx)
{
	const lt_spring *s = ctx;
	(void)t; (void)n;
	d[0] = y[1];
	d[1] = -s->k * (y[0] - s->target) - s->c * y[1];
}

bool lt_spring_at_rest(const lt_spring *s) { return fabs(s->x - s->target) < 1e-3 && fabs(s->v) < 1e-3; }

void lt_spring_step(lt_spring *s, double dt)
{
	if (dt <= 0) return;
	if (dt > 0.25) dt = 0.25;                                  /* a stalled process must not make it explode */
	int steps = (int)ceil(dt / MAX_STEP);
	double h = dt / steps;
	double y[2] = {s->x, s->v};
	for (int i = 0; i < steps; i++) lt_ode_rk4(spring_f, 0, y, 2, h, s);
	s->x = y[0]; s->v = y[1];
	if (lt_spring_at_rest(s)) { s->x = s->target; s->v = 0; }
}

void lt_anim_init(lt_anim *a) { memset(a, 0, sizeof *a); }

int lt_anim_bind(lt_anim *a, double *out, lt_spring initial)
{
	if (a->n >= LT_ANIM_MAX) return -1;
	a->sp[a->n] = initial; a->out[a->n] = out;
	*out = initial.x;
	return a->n++;
}
void lt_anim_to(lt_anim *a, int id, double target) { if (id >= 0 && id < a->n) a->sp[id].target = target; }
void lt_anim_snap(lt_anim *a, int id, double v)
{
	if (id < 0 || id >= a->n) return;
	a->sp[id].x = a->sp[id].target = v; a->sp[id].v = 0;
	*a->out[id] = v;
}

bool lt_anim_tick(lt_anim *a, double dt)
{
	bool moving = false;
	for (int i = 0; i < a->n; i++) {
		if (lt_spring_at_rest(&a->sp[i])) continue;
		lt_spring_step(&a->sp[i], dt);
		*a->out[i] = a->sp[i].x;
		if (!lt_spring_at_rest(&a->sp[i])) moving = true;
	}
	return moving;
}
