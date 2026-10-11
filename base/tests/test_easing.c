#include "test_common.h"
#define EASING_IMPLEMENTATION
#include "math/easing.h"

int main(void)
{
    print_cstr("test_easing\n");

    /* every easing function must satisfy f(0)==0 and f(1)==1 exactly -
       the one universal defining property all of these share, checked
       for real per function rather than assumed */
    TEST_CHECK(ease_in_quad(0.0f) == 0.0f && ease_in_quad(1.0f) == 1.0f, "ease_in_quad endpoints are exact");
    TEST_CHECK(ease_out_quad(0.0f) == 0.0f && ease_out_quad(1.0f) == 1.0f, "ease_out_quad endpoints are exact");
    TEST_CHECK(ease_in_out_quad(0.0f) == 0.0f && ease_in_out_quad(1.0f) == 1.0f, "ease_in_out_quad endpoints are exact");
    TEST_CHECK(ease_in_cubic(0.0f) == 0.0f && ease_in_cubic(1.0f) == 1.0f, "ease_in_cubic endpoints are exact");
    TEST_CHECK(ease_out_cubic(0.0f) == 0.0f && ease_out_cubic(1.0f) == 1.0f, "ease_out_cubic endpoints are exact");
    TEST_CHECK(ease_in_out_cubic(0.0f) == 0.0f && ease_in_out_cubic(1.0f) == 1.0f, "ease_in_out_cubic endpoints are exact");
    TEST_CHECK(ease_in_sine(0.0f) == 0.0f, "ease_in_sine(0) == 0 exactly");
    {
        f32 v = ease_in_sine(1.0f);
        TEST_CHECK(v > 0.999f && v < 1.001f, "ease_in_sine(1) ~= 1");
    }
    TEST_CHECK(ease_out_back(0.0f) == 0.0f, "ease_out_back(0) == 0 exactly");
    {
        f32 v = ease_out_back(1.0f);
        TEST_CHECK(v > 0.999f && v < 1.001f, "ease_out_back(1) ~= 1");
    }

    /* ease_in should start slower than linear (t < 0.5 -> f(t) < t) */
    TEST_CHECK(ease_in_quad(0.3f) < 0.3f, "ease_in_quad starts slower than linear (accelerating from 0)");
    TEST_CHECK(ease_in_cubic(0.3f) < ease_in_quad(0.3f), "ease_in_cubic starts even slower than ease_in_quad (higher power)");

    /* ease_out should start faster than linear (t < 0.5 -> f(t) > t) */
    TEST_CHECK(ease_out_quad(0.3f) > 0.3f, "ease_out_quad starts faster than linear (decelerating toward 1)");

    /* ease_in_out is symmetric around t=0.5: f(0.5) == 0.5 for the quad/cubic polynomial forms */
    {
        f32 v = ease_in_out_quad(0.5f);
        TEST_CHECK(v > 0.499f && v < 0.501f, "ease_in_out_quad(0.5) == 0.5 (symmetric midpoint)");
    }

    /* ease_out_back overshoots past 1.0 partway through - its own defining,
       named characteristic ("back" easing pulls back before settling) */
    {
        i32 saw_overshoot = 0;
        f32 t;
        for (t = 0.5f; t < 1.0f; t += 0.05f) {
            if (ease_out_back(t) > 1.0f) saw_overshoot = 1;
        }
        TEST_CHECK(saw_overshoot, "ease_out_back overshoots past 1.0 somewhere before t=1 (its defining 'back' behavior)");
    }

    print_flush();
    return test_failures_ != 0;
}
