/* ecs_dynamics.h - differential-equation motion for UI and game feel, solved in C over arrays: damped springs, critically damped
 * "smooth damp", exponential slide, second-order response with overshoot/anticipation, and a generic RK4 for your own equations.
 *
 * WHY CLOSED FORM
 *  The usual UI spring is `v += (k*(target-x) - c*v)*dt; x += v*dt` - semi-implicit Euler. It explodes when dt is large (a hitch,
 *  a background tab) and its feel changes with frame rate. Here the damped harmonic oscillator
 *        x'' + 2*zeta*omega*x' + omega^2*(x - target) = 0           omega = 2*pi*frequency
 *  is solved EXACTLY for the step: a 2x2 transition matrix (pp,pv,vp,vv) from (frequency, zeta, dt), applied to (x - target, v).
 *  Unconditionally stable for any dt (even 10 seconds), identical feel at 30 and 240 fps, under/critically/over-damped all handled.
 *  The matrix depends only on the parameters, so a whole batch sharing one spring costs one matrix + one tight loop - SIMD-able,
 *  and bit-identical on every ISA like the rest of the library (no fused multiply-add, no fast-math).
 *
 * VOCABULARY
 *  frequency  Hz: how fast it responds (2 = settles in about half a second). 0 = no spring force (the value just coasts).
 *  zeta       damping ratio: 0 never settles, <1 bounces (0.3 jelly, 0.6 lively), 1 critical (fastest with no overshoot), >1 sluggish.
 *  smooth     the same critical spring in Unity's SmoothDamp convention (omega = 2/smooth_time): ~59% of the way after smooth_time,
 *             ~95% after about 2.4x smooth_time. Same numbers as a Unity project, so existing tuning carries over.
 *  slide      first-order: closes a fraction of the gap per second, never overshoots, no velocity state ("slide dump").
 *  second-order (f, zeta, r)  adds r: r = 1 plain, r > 1 overshoots eagerly, r < 0 ANTICIPATES (dips the wrong way first).
 *
 * All arrays are element counts of floats; a Vector2 is two floats, so animate x/y with n=2 or batch many with n=N. */
#ifndef ECS_DYNAMICS_H
#define ECS_DYNAMICS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* x' = pp*(x - t) + pv*v + t ; v' = vp*(x - t) + vv*v. Precompute once per (frequency, zeta, dt), reuse across a batch. */
typedef struct ecs_spring { float pp, pv, vp, vv; } ecs_spring;

ecs_spring ecs_spring_make(float frequency, float zeta, float dt);
ecs_spring ecs_spring_make_smooth(float smooth_time, float dt);          /* critical spring, Unity SmoothDamp convention */

/* Advance n independent scalars toward target[i] (or one shared target value). In place. */
void ecs_spring_step(float *x, float *v, const float *target, size_t n, ecs_spring s);
void ecs_spring_step_to(float *x, float *v, float target, size_t n, ecs_spring s);
/* Same, clamping |v| to max_speed after the step (Unity's maxSpeed; <= 0 disables). */
void ecs_spring_step_clamped(float *x, float *v, const float *target, size_t n, ecs_spring s, float max_speed);

/* Impulse: add to velocity ("kick" a button when it is clicked so it wobbles). */
void ecs_spring_kick(float *v, size_t n, float impulse);

/* First-order slide: x += (target - x) * (1 - exp(-rate*dt)). rate in 1/seconds (10 ~ settles in ~0.3 s). */
void ecs_slide_step(float *x, const float *target, size_t n, float rate, float dt);
void ecs_slide_step_to(float *x, float target, size_t n, float rate, float dt);

/* Second-order dynamics. State per element: y (output), yd (its velocity), xp (previous input, for the input's derivative).
 * Initialise y = xp = starting value, yd = 0. Stable for any dt (the internal k2 is clamped, so no blow-up on a hitch). */
typedef struct ecs_second_order { float k1, k2, k3; } ecs_second_order;
ecs_second_order ecs_second_order_make(float frequency, float zeta, float response);
void ecs_second_order_step(float *y, float *yd, float *xp, const float *x, size_t n, ecs_second_order p, float dt);

/* Generic RK4 for your own equations (a pendulum wobble, a coupled-spring chain, a damped drag): dy/dt = f(t, y).
 * state has n floats, updated in place. n <= 64. f writes the derivative into out[0..n). */
typedef void (*ecs_deriv_fn)(float t, const float *y, float *out, size_t n, void *user);
void ecs_rk4_step(float *state, size_t n, float t, float dt, ecs_deriv_fn f, void *user);

#ifdef __cplusplus
}
#endif
#endif
