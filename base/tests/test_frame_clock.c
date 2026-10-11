#include "test_common.h"
#define FRAME_CLOCK_IMPLEMENTATION
#include "frame_clock.h"

int main(void)
{
    Frame_Clock fc;

    print_cstr("test_frame_clock\n");

    /* 60Hz fixed step, clamp raw deltas to 0.25s */
    frame_clock_init(&fc, 1.0 / 60.0, 0.25);

    {
        f64 d = frame_clock_tick(&fc, 1.0 / 60.0);
        TEST_CHECK(d > 0.01666 && d < 0.01667, "tick returns the (unclamped) delta when it's under the max");
        TEST_CHECK(frame_clock_consume_step(&fc) == 1, "exactly one fixed step is due after one frame's worth of delta");
        TEST_CHECK(frame_clock_consume_step(&fc) == 0, "no second step is due - the accumulator only had one step's worth");
    }

    /* several small frames accumulate correctly without ever producing
     * a spurious extra step */
    {
        i32 steps = 0;
        i32 i;
        for (i = 0; i < 30; i++) {
            frame_clock_tick(&fc, 1.0 / 60.0);
            while (frame_clock_consume_step(&fc)) steps++;
        }
        TEST_CHECK(steps == 30, "30 frames of exactly one step's worth of delta each produce exactly 30 fixed steps");
    }

    /* a render frame slower than the fixed step (e.g. 30fps against a
     * 60Hz physics step) should produce TWO fixed steps to catch up */
    {
        i32 steps = 0;
        frame_clock_tick(&fc, 2.0 / 60.0);
        while (frame_clock_consume_step(&fc)) steps++;
        TEST_CHECK(steps == 2, "a frame twice as long as the fixed step produces two catch-up physics steps");
    }

    /* the spiral-of-death guard: a huge raw delta (debugger pause, a
     * backgrounded process resuming) must be clamped, not queue an
     * enormous number of catch-up steps */
    {
        i32 steps = 0;
        f64 d = frame_clock_tick(&fc, 100.0); /* a 100-second stall */
        TEST_CHECK(d == 0.25, "a huge raw delta is clamped to max_frame_delta, not passed through raw");
        while (frame_clock_consume_step(&fc)) steps++;
        TEST_CHECK(steps == 15, "the clamped 0.25s produces exactly 15 steps at 60Hz (0.25/(1/60)), not thousands");
    }

    /* interpolation alpha: mid-way through accumulating a step */
    {
        Frame_Clock fc2;
        frame_clock_init(&fc2, 1.0 / 60.0, 0.25);
        frame_clock_tick(&fc2, 0.5 / 60.0); /* half a step's worth */
        while (frame_clock_consume_step(&fc2)) { /* drains nothing, half a step isn't due yet */ }
        {
            f64 alpha = frame_clock_interpolation_alpha(&fc2);
            TEST_CHECK(alpha > 0.49 && alpha < 0.51, "interpolation alpha is ~0.5 after accumulating exactly half a fixed step");
        }
    }

    /* a negative delta (clock went backward) is treated as zero, not
     * subtracted from the accumulator */
    {
        Frame_Clock fc3;
        f64 before, after, d;
        frame_clock_init(&fc3, 1.0 / 60.0, 0.25);
        frame_clock_tick(&fc3, 0.5 / 60.0);
        before = frame_clock_interpolation_alpha(&fc3);
        d = frame_clock_tick(&fc3, -1.0);
        after = frame_clock_interpolation_alpha(&fc3);
        TEST_CHECK(d == 0.0, "a negative raw delta clamps to 0, not passed through negative");
        TEST_CHECK(after == before, "a negative delta doesn't shrink the accumulator");
    }

    print_flush();
    return test_failures_ != 0;
}
