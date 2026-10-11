#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#define DEFLATE_IMPLEMENTATION
#define PNG_DEC_IMPLEMENTATION
#include "alloc/arena_alloc.h"
#include "codec/png_dec.h"
#include "png_test_data.h"

/* pixel formulas mirror exactly what generated tests/png_test_data.h (see
 * the Python script that produced it) - an independent cross-check, not
 * just "does it not crash" */

int main(void)
{
    Arena a = {0};

    print_cstr("test_png\n");

    {
        i32 w, h;
        u8 *rgba = png_decode(&a, png_test_rgb, png_test_rgb_len, &w, &h);
        TEST_CHECK(rgba != NULL, "RGB PNG decodes successfully");
        TEST_CHECK(w == 16 && h == 12, "RGB PNG dimensions match (16x12)");
        if (rgba) {
            i32 ok = 1;
            i32 x, y;
            for (y = 0; y < h; y++) {
                for (x = 0; x < w; x++) {
                    u8 *px = rgba + (size_t)(y * w + x) * 4;
                    u8 er = (u8)(x * 16), eg = (u8)(y * 20), eb = (u8)((x + y) * 8 % 256);
                    if (px[0] != er || px[1] != eg || px[2] != eb || px[3] != 255) ok = 0;
                }
            }
            TEST_CHECK(ok, "every RGB pixel matches the expected gradient formula, alpha forced to 255");
        }
    }

    {
        i32 w, h;
        u8 *rgba = png_decode(&a, png_test_rgba, png_test_rgba_len, &w, &h);
        TEST_CHECK(rgba != NULL, "RGBA PNG decodes successfully");
        TEST_CHECK(w == 10 && h == 8, "RGBA PNG dimensions match (10x8)");
        if (rgba) {
            i32 ok = 1;
            i32 x, y;
            for (y = 0; y < h; y++) {
                for (x = 0; x < w; x++) {
                    u8 *px = rgba + (size_t)(y * w + x) * 4;
                    u8 er = (u8)(x * 25), eg = (u8)(y * 30), eb = 100, ea = (u8)((x + y) * 10 % 256);
                    if (px[0] != er || px[1] != eg || px[2] != eb || px[3] != ea) ok = 0;
                }
            }
            TEST_CHECK(ok, "every RGBA pixel matches the expected formula including varying alpha");
        }
    }

    {
        i32 w, h;
        u8 *rgba = png_decode(&a, png_test_gray, png_test_gray_len, &w, &h);
        TEST_CHECK(rgba != NULL, "grayscale PNG decodes successfully");
        TEST_CHECK(w == 8 && h == 8, "grayscale PNG dimensions match (8x8)");
        if (rgba) {
            i32 ok = 1;
            i32 x, y;
            for (y = 0; y < h; y++) {
                for (x = 0; x < w; x++) {
                    u8 *px = rgba + (size_t)(y * w + x) * 4;
                    u8 ev = (u8)((x * 30 + y * 10) % 256);
                    if (px[0] != ev || px[1] != ev || px[2] != ev || px[3] != 255) ok = 0;
                }
            }
            TEST_CHECK(ok, "every grayscale pixel is replicated across R/G/B with alpha 255");
        }
    }

    {
        i32 w, h;
        u8 *rgba = png_decode(&a, png_test_pal, png_test_pal_len, &w, &h);
        TEST_CHECK(rgba != NULL, "palette PNG decodes successfully");
        TEST_CHECK(w == 6 && h == 6, "palette PNG dimensions match (6x6)");
        if (rgba) {
            i32 ok = 1;
            i32 x, y;
            static const u8 pal_colors[3][3] = { {255,0,0}, {0,255,0}, {0,0,255} };
            for (y = 0; y < h; y++) {
                for (x = 0; x < w; x++) {
                    u8 *px = rgba + (size_t)(y * w + x) * 4;
                    i32 idx = (x + y) % 3;
                    if (px[0] != pal_colors[idx][0] || px[1] != pal_colors[idx][1] ||
                        px[2] != pal_colors[idx][2] || px[3] != 255) ok = 0;
                }
            }
            TEST_CHECK(ok, "every palette-indexed pixel resolves to the correct palette color");
        }
    }

    {
        u8 garbage[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
        i32 w, h;
        u8 *r = png_decode(&a, garbage, sizeof(garbage), &w, &h);
        TEST_CHECK(r == NULL, "non-PNG data (bad signature) is rejected cleanly, not crashed on");
    }

    {
        i32 w, h;
        u8 *r = png_decode(&a, png_test_rgb, 20, &w, &h);
        TEST_CHECK(r == NULL, "truncated PNG (signature + partial IHDR) is rejected cleanly");
    }

    arena_free(&a);
    print_flush();
    return test_failures_ != 0;
}
