#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#define JPEG_DEC_IMPLEMENTATION
#include "alloc/arena_alloc.h"
#include "codec/jpeg_dec.h"
#include "jpeg_test_data.h"

/* JPEG is lossy, so exact-value comparison against the source pixels
 * isn't meaningful - instead these compare against PIL's own decode of
 * the identical file (an independent, mature JPEG decoder) as the
 * correctness oracle, within a small per-channel tolerance to allow for
 * ordinary IDCT/rounding differences between implementations that both
 * correctly implement the same baseline spec. */
#define TOLERANCE 6

static i32 close_enough(u8 a, u8 b)
{
    i32 d = (i32)a - (i32)b;
    if (d < 0) d = -d;
    return d <= TOLERANCE;
}

int main(void)
{
    Arena a = {0};

    print_cstr("test_jpeg\n");

    {
        i32 w, h;
        u8 *rgba = jpeg_decode(&a, jpeg_test_rgb, jpeg_test_rgb_len, &w, &h);
        TEST_CHECK(rgba != NULL, "RGB JPEG decodes successfully");
        TEST_CHECK(w == 32 && h == 24, "RGB JPEG dimensions match (32x24)");
        if (rgba) {
            i32 ok = 1;
            i32 max_diff = 0;
            i32 x, y;
            for (y = 0; y < h; y++) {
                for (x = 0; x < w; x++) {
                    u8 *px = rgba + (size_t)(y * w + x) * 4;
                    const u8 *ref = jpeg_test_rgb_ref + (size_t)(y * w + x) * 3;
                    i32 dr = (i32)px[0] - (i32)ref[0];
                    i32 dg = (i32)px[1] - (i32)ref[1];
                    i32 db = (i32)px[2] - (i32)ref[2];
                    if (dr < 0) dr = -dr;
                    if (dg < 0) dg = -dg;
                    if (db < 0) db = -db;
                    if (dr > max_diff) max_diff = dr;
                    if (dg > max_diff) max_diff = dg;
                    if (db > max_diff) max_diff = db;
                    if (!close_enough(px[0], ref[0]) || !close_enough(px[1], ref[1]) || !close_enough(px[2], ref[2])) ok = 0;
                    if (px[3] != 255) ok = 0;
                }
            }
            TEST_CHECK(ok, "every RGB pixel is within tolerance of PIL's reference decode");
            print_cstr("  (max per-channel diff vs reference: "); print_i64(max_diff); print_cstr(")\n");
        }
    }

    {
        i32 w, h;
        u8 *rgba = jpeg_decode(&a, jpeg_test_gray, jpeg_test_gray_len, &w, &h);
        TEST_CHECK(rgba != NULL, "grayscale JPEG decodes successfully");
        TEST_CHECK(w == 16 && h == 16, "grayscale JPEG dimensions match (16x16)");
        if (rgba) {
            i32 ok = 1;
            i32 x, y;
            for (y = 0; y < h; y++) {
                for (x = 0; x < w; x++) {
                    u8 *px = rgba + (size_t)(y * w + x) * 4;
                    u8 ref = jpeg_test_gray_ref[y * w + x];
                    if (!close_enough(px[0], ref) || !close_enough(px[1], ref) || !close_enough(px[2], ref)) ok = 0;
                    if (px[3] != 255) ok = 0;
                }
            }
            TEST_CHECK(ok, "every grayscale pixel is within tolerance of PIL's reference decode");
        }
    }

    {
        u8 garbage[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
        i32 w, h;
        u8 *r = jpeg_decode(&a, garbage, sizeof(garbage), &w, &h);
        TEST_CHECK(r == NULL, "non-JPEG data (no SOI marker) is rejected cleanly, not crashed on");
    }

    {
        i32 w, h;
        u8 *r = jpeg_decode(&a, jpeg_test_rgb, 10, &w, &h);
        TEST_CHECK(r == NULL, "truncated JPEG (headers cut off) is rejected cleanly");
    }

    arena_free(&a);
    print_flush();
    return test_failures_ != 0;
}
