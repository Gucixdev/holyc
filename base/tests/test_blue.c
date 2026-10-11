#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#define RNG_IMPLEMENTATION
#define BLUE_IMPLEMENTATION
#include "arena_alloc.h"
#include "noise.h"
#include <math.h>

int main(void)
{
    Arena a = {0};
    Blue_Point *pts;
    size_t n, i, j;

    print_cstr("test_blue\n");

    n = blue_noise_sample_2d(&a, 42, 100.0f, 100.0f, 5.0f, 30, &pts);
    TEST_CHECK(n > 0, "sampling a 100x100 area produces at least one point");

    /* Poisson-disk packing over a 100x100 area with min_dist=5 should
     * land somewhere around area/(min_dist^2) ~= 400 points (loose upper
     * bound - circle-packing density caps it well below that, but a
     * degenerate implementation producing e.g. only 2-3 points, or tens
     * of thousands, would fail this) */
    TEST_CHECK(n > 50 && n < 2000, "point count is in a plausible range for this area/min_dist");

    {
        i32 all_in_bounds = 1;
        for (i = 0; i < n; i++) {
            if (pts[i].x < 0.0f || pts[i].x >= 100.0f || pts[i].y < 0.0f || pts[i].y >= 100.0f) all_in_bounds = 0;
        }
        TEST_CHECK(all_in_bounds, "every point lies within [0,width) x [0,height)");
    }

    {
        f32 min_dist_sq = 5.0f * 5.0f;
        i32 violated = 0;
        for (i = 0; i < n && !violated; i++) {
            for (j = i + 1; j < n; j++) {
                f32 dx = pts[i].x - pts[j].x;
                f32 dy = pts[i].y - pts[j].y;
                /* small epsilon for float rounding, not a real relaxation of the constraint */
                if (dx * dx + dy * dy < min_dist_sq - 0.01f) { violated = 1; break; }
            }
        }
        TEST_CHECK(!violated, "no two points are closer than min_dist (the core Poisson-disk property)");
    }

    {
        i32 q[4] = {0, 0, 0, 0}; /* quadrant occupancy */
        i32 quadrants_hit;
        for (i = 0; i < n; i++) {
            i32 qi = (pts[i].x >= 50.0f ? 1 : 0) + (pts[i].y >= 50.0f ? 2 : 0);
            q[qi] = 1;
        }
        quadrants_hit = q[0] + q[1] + q[2] + q[3];
        TEST_CHECK(quadrants_hit == 4, "points spread across all four quadrants (no gross clustering)");
    }

    {
        Blue_Point *pts_again;
        size_t n_again;
        i32 identical;

        n_again = blue_noise_sample_2d(&a, 42, 100.0f, 100.0f, 5.0f, 30, &pts_again);
        identical = (n_again == n);
        if (identical) {
            for (i = 0; i < n; i++) {
                if (pts[i].x != pts_again[i].x || pts[i].y != pts_again[i].y) { identical = 0; break; }
            }
        }
        TEST_CHECK(identical, "same seed reproduces the identical point set");
    }

    {
        Blue_Point *pts_diff;
        size_t n_diff;
        i32 differs;

        n_diff = blue_noise_sample_2d(&a, 1337, 100.0f, 100.0f, 5.0f, 30, &pts_diff);
        differs = (n_diff != n);
        if (!differs) {
            for (i = 0; i < n; i++) {
                if (pts[i].x != pts_diff[i].x || pts[i].y != pts_diff[i].y) { differs = 1; break; }
            }
        }
        TEST_CHECK(differs, "a different seed produces a different point set");
    }

    arena_free(&a);
    print_flush();
    return test_failures_ != 0;
}
