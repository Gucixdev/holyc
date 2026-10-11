#ifndef HOLYC_FRAME_CLOCK_H_
#define HOLYC_FRAME_CLOCK_H_

/* Fixed-timestep-with-accumulator game loop clock - Glenn Fiedler's
 * "Fix Your Timestep!" pattern (gafferongames.com), the same technique
 * Godot's own engine loop uses under the hood to split _process(delta)
 * (variable-rate, once per rendered frame) from _physics_process(delta)
 * (fixed-rate, zero-or-more times per rendered frame, catching up via
 * an accumulator so physics stays deterministic regardless of render
 * frame rate).
 *
 * Owns NO clock of its own - frame_clock_tick takes the raw measured
 * wall-clock delta as a plain f64 argument (from time_util.h's
 * time_now_seconds(), or anything else) rather than reading time
 * itself, the same "pure function of caller-supplied elapsed time"
 * stance theme.h's own theme_fold_rect already takes - real
 * determinism/testability (fabricated delta sequences, not flaky
 * wall-clock-dependent tests), not an accident of scope-cutting.
 *
 * max_frame_delta is Fiedler's own "spiral of death" guard: a huge raw
 * delta (a debugger pause, a frame hitch, a paused/backgrounded
 * process resuming) would otherwise queue an equally huge number of
 * catch-up fixed steps, each one costing real time to run, pushing the
 * NEXT frame's delta even higher - a real, well-documented failure
 * mode this clamp exists specifically to prevent, not a hypothetical
 * one. */

#include "base.h"

typedef struct {
    f64 accumulator;
    f64 fixed_dt;
    f64 max_frame_delta;
} Frame_Clock;

void frame_clock_init(Frame_Clock *fc, f64 fixed_dt, f64 max_frame_delta);

/* call once per rendered frame with the raw measured delta. Clamps it
 * to max_frame_delta (the spiral-of-death guard) and adds the clamped
 * amount to the internal accumulator, then returns that same clamped
 * value - what a caller's own variable-rate per-frame logic should
 * actually use (never the raw, unclamped delta). */
f64 frame_clock_tick(Frame_Clock *fc, f64 raw_delta);

/* call in a loop after frame_clock_tick() until it returns 0 - each 1
 * means exactly one fixed_dt physics step is due (fixed_dt has already
 * been subtracted from the accumulator when this returns 1). */
i32 frame_clock_consume_step(Frame_Clock *fc);

/* accumulator / fixed_dt - always in [0,1) once frame_clock_consume_step
 * has drained every full step this frame. The fractional position
 * between the last completed fixed step and the next one, for
 * interpolating rendered state between two physics snapshots (Fiedler's
 * own `state = current*alpha + previous*(1-alpha)`). */
f64 frame_clock_interpolation_alpha(const Frame_Clock *fc);

#endif /* HOLYC_FRAME_CLOCK_H_ */

#ifdef FRAME_CLOCK_IMPLEMENTATION
#ifndef FRAME_CLOCK_IMPLEMENTATION_
#define FRAME_CLOCK_IMPLEMENTATION_

void frame_clock_init(Frame_Clock *fc, f64 fixed_dt, f64 max_frame_delta)
{
    fc->accumulator = 0.0;
    fc->fixed_dt = fixed_dt;
    fc->max_frame_delta = max_frame_delta;
}

f64 frame_clock_tick(Frame_Clock *fc, f64 raw_delta)
{
    f64 clamped = raw_delta > fc->max_frame_delta ? fc->max_frame_delta : raw_delta;
    if (clamped < 0.0) clamped = 0.0; /* a negative delta (clock going backward) is never physically meaningful */
    fc->accumulator += clamped;
    return clamped;
}

i32 frame_clock_consume_step(Frame_Clock *fc)
{
    if (fc->accumulator < fc->fixed_dt) return 0;
    fc->accumulator -= fc->fixed_dt;
    return 1;
}

f64 frame_clock_interpolation_alpha(const Frame_Clock *fc)
{
    return fc->accumulator / fc->fixed_dt;
}

#endif /* FRAME_CLOCK_IMPLEMENTATION_ */
#endif /* FRAME_CLOCK_IMPLEMENTATION */
