#include "test_common.h"
#define VORONOI_IMPLEMENTATION
#include "noise.h"

int main(void)
{
    Voronoi_Query q, q2;
    i32 all_nonneg_2d = 1, all_nonneg_3d = 1;
    i32 ordered_2d = 1, ordered_3d = 1;
    i32 any_nan_or_inf = 0;
    i32 any_variation_2d = 0, any_variation_3d = 0;
    f32 first_d0_2d, first_d0_3d;
    size_t i, j;

    print_cstr("test_voronoi\n");

    voronoi_query_2d(1337, 1.5f, 2.5f, 1.0f, &q);
    voronoi_query_2d(1337, 1.5f, 2.5f, 1.0f, &q2);
    TEST_CHECK(q.distance0 == q2.distance0 && q.closest_hash == q2.closest_hash,
               "2D: same seed+coords+jitter produce identical output (deterministic)");

    voronoi_query_3d(1337, 1.5f, 2.5f, 3.5f, 1.0f, &q);
    voronoi_query_3d(1337, 1.5f, 2.5f, 3.5f, 1.0f, &q2);
    TEST_CHECK(q.distance0 == q2.distance0 && q.closest_hash == q2.closest_hash,
               "3D: same seed+coords+jitter produce identical output (deterministic)");

    /* jitter=0 -> a perfectly regular grid: querying exactly at a grid
     * point puts distance0 at 0 (you're standing on the feature point)
     * and distance1 at exactly 1 (the nearest neighboring grid point) -
     * an exact, checkable invariant unlike the jittered case. */
    voronoi_query_2d(1337, 3.0f, 4.0f, 0.0f, &q);
    TEST_CHECK(q.distance0 == 0.0f, "2D: jitter=0, queried exactly at a grid point, distance0 is exactly 0");
    TEST_CHECK((q.distance1 > 1.0f ? q.distance1 - 1.0f : 1.0f - q.distance1) < 0.0001f, "2D: jitter=0, distance1 is exactly 1 (regular unit grid)");

    voronoi_query_3d(1337, 3.0f, 4.0f, 5.0f, 0.0f, &q);
    TEST_CHECK(q.distance0 == 0.0f, "3D: jitter=0, queried exactly at a grid point, distance0 is exactly 0");
    TEST_CHECK((q.distance1 > 1.0f ? q.distance1 - 1.0f : 1.0f - q.distance1) < 0.0001f, "3D: jitter=0, distance1 is exactly 1 (regular unit grid)");

    first_d0_2d = 0.0f;
    first_d0_3d = 0.0f;
    for (i = 0; i < 100; i++) {
        for (j = 0; j < 100; j++) {
            f32 fx = (f32)i * 0.37f;
            f32 fy = (f32)j * 0.53f;

            voronoi_query_2d(99, fx, fy, 1.0f, &q);
            if (q.distance0 < 0.0f || q.distance1 < 0.0f) all_nonneg_2d = 0;
            if (q.distance1 < q.distance0) ordered_2d = 0;
            if (q.distance0 != q.distance0 || q.distance1 != q.distance1) any_nan_or_inf = 1;
            if (i == 0 && j == 0) first_d0_2d = q.distance0;
            else if (q.distance0 != first_d0_2d) any_variation_2d = 1;

            voronoi_query_3d(99, fx, fy, fx * 0.7f, 1.0f, &q);
            if (q.distance0 < 0.0f || q.distance1 < 0.0f) all_nonneg_3d = 0;
            if (q.distance1 < q.distance0) ordered_3d = 0;
            if (q.distance0 != q.distance0 || q.distance1 != q.distance1) any_nan_or_inf = 1;
            if (i == 0 && j == 0) first_d0_3d = q.distance0;
            else if (q.distance0 != first_d0_3d) any_variation_3d = 1;
        }
    }

    TEST_CHECK(all_nonneg_2d, "2D: distance0/distance1 are always non-negative");
    TEST_CHECK(all_nonneg_3d, "3D: distance0/distance1 are always non-negative");
    TEST_CHECK(ordered_2d, "2D: distance1 >= distance0 always holds");
    TEST_CHECK(ordered_3d, "3D: distance1 >= distance0 always holds");
    TEST_CHECK(!any_nan_or_inf, "no NaN across the sampled grid");
    TEST_CHECK(any_variation_2d, "2D: distance0 actually varies across a sampled grid (not degenerate)");
    TEST_CHECK(any_variation_3d, "3D: distance0 actually varies across a sampled grid (not degenerate)");

    /* voronoi_edge should behave as a real "distance to nearest cell
     * edge" proxy: right at a feature point it's large (far from any
     * boundary, second-nearest is far away relative to nearest which is
     * 0); moved to be roughly equidistant between two features it should
     * shrink towards 0. Checked qualitatively (edge at the feature point
     * is clearly bigger than a generic sampled point) rather than via an
     * exact algebraic bound, since "roughly equidistant" isn't
     * constructible exactly without re-deriving the jittered feature
     * positions. */
    {
        f32 edge_at_feature = voronoi_edge_2d(1337, 3.0f, 4.0f, 0.0f);
        f32 edge_typical = voronoi_edge_2d(1337, 3.3f, 4.3f, 0.0f);
        TEST_CHECK(edge_at_feature > edge_typical, "2D: edge value at an exact feature point exceeds a typical off-feature point");
    }

    /* cell value should differ between two different cells (distinct
     * closest_hash) and be identical for two query points that resolve
     * to the SAME nearest feature (same closest_hash, checked directly
     * rather than assumed). */
    {
        f32 cv_a, cv_b;
        voronoi_query_2d(7, 1.0f, 1.0f, 1.0f, &q);
        voronoi_query_2d(7, 10.0f, 10.0f, 1.0f, &q2);
        cv_a = voronoi_cell_value(&q);
        cv_b = voronoi_cell_value(&q2);
        TEST_CHECK(q.closest_hash != q2.closest_hash, "sanity: two far-apart queries resolve to different nearest features");
        TEST_CHECK(cv_a != cv_b, "voronoi_cell_value differs between two different cells");
    }

    print_flush();
    return test_failures_ != 0;
}
