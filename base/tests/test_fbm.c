#include "test_common.h"
#define SIMPLEX_IMPLEMENTATION
#define FBM_IMPLEMENTATION
#include "math/noise.h"
#include <math.h>

int main(void)
{
    f32 single, one_octave, two_octaves, many_octaves;
    f32 x = 1.5f, y = 2.5f;
    i32 seed = 1337;

    print_cstr("test_fbm\n");

    /* 1-octave fbm at lacunarity=2, gain=0.5 should equal a single
       simplex_noise_2d call at the base frequency (octave loop runs once,
       weight 1.0, frequency 1.0 - no compounding to differ from) */
    single = simplex_noise_2d(seed, x, y);
    one_octave = fbm_2d(seed, x, y, 1, 2.0f, 0.5f);
    TEST_CHECK(single == one_octave, "fbm_2d with 1 octave equals a single simplex_noise_2d call exactly");

    /* adding more octaves changes the result (real additional detail
       being summed in, not a no-op) */
    two_octaves = fbm_2d(seed, x, y, 2, 2.0f, 0.5f);
    TEST_CHECK(two_octaves != one_octave, "fbm_2d with 2 octaves differs from 1 octave (real octave summation happening)");

    many_octaves = fbm_2d(seed, x, y, 6, 2.0f, 0.5f);
    TEST_CHECK(many_octaves != two_octaves, "fbm_2d with 6 octaves differs from 2 octaves (each added octave changes the result)");

    /* determinism: same inputs produce the same output every time */
    {
        f32 a = fbm_2d(seed, x, y, 4, 2.0f, 0.5f);
        f32 b = fbm_2d(seed, x, y, 4, 2.0f, 0.5f);
        TEST_CHECK(a == b, "fbm_2d is deterministic for the same seed/coords/params");
    }

    /* output stays finite and in a sane bounded range across many octaves
       and many sample points - a real geometric-series amplitude sum with
       gain<1 must converge, not blow up */
    {
        i32 ok = 1;
        i32 i;
        for (i = 0; i < 50; i++) {
            f32 v = fbm_2d(seed + i, (f32)i * 0.37f, (f32)i * 1.13f, 8, 2.0f, 0.5f);
            if (v != v) ok = 0; /* NaN check */
            if (v > 3.0f || v < -3.0f) ok = 0; /* generous bound - base noise is ~[-1,1], geometric sum of octaves stays well under 3 */
        }
        TEST_CHECK(ok, "fbm_2d output stays finite and boundedly-scaled across many octaves/points (no runaway amplitude)");
    }

    /* different seeds produce different output for the same coordinates */
    {
        f32 a = fbm_2d(1, x, y, 4, 2.0f, 0.5f);
        f32 b = fbm_2d(2, x, y, 4, 2.0f, 0.5f);
        TEST_CHECK(a != b, "fbm_2d output differs for different seeds at the same coordinates");
    }

    print_flush();
    return test_failures_ != 0;
}
