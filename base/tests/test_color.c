#include "test_common.h"
#define COLOR_IMPLEMENTATION
#include "color.h"

#include <string.h>

int main(void)
{
    print_cstr("test_color\n");

    /* color_rgb_to_256 / color_256_to_rgb round trip on exact palette hits */
    {
        Color_Rgb black = {0, 0, 0};
        Color_Rgb white = {255, 255, 255};
        u8 idx_black = color_rgb_to_256(black);
        u8 idx_white = color_rgb_to_256(white);
        TEST_CHECK(idx_black >= 16, "rgb_to_256: pure black lands in cube or grayscale range, not system colors");
        TEST_CHECK(idx_white >= 16, "rgb_to_256: pure white lands in cube or grayscale range");
    }

    /* an exact cube-corner color round-trips exactly through 256->rgb */
    {
        Color_Rgb pure_red = {255, 0, 0}; /* exact cube corner: level 5,0,0 */
        u8 idx = color_rgb_to_256(pure_red);
        Color_Rgb back = color_256_to_rgb(idx);
        TEST_CHECK(back.r == 255 && back.g == 0 && back.b == 0, "rgb_to_256->256_to_rgb: exact cube corner round-trips exactly");
    }

    /* a pure gray value prefers the grayscale ramp over the coarser cube */
    {
        Color_Rgb mid_gray = {128, 128, 128};
        u8 idx = color_rgb_to_256(mid_gray);
        TEST_CHECK(idx >= 232, "rgb_to_256: a neutral gray picks the grayscale ramp (232-255), not the RGB cube");
    }

    /* color_256_to_rgb: known fixed indices */
    {
        Color_Rgb c16, c231, c232;

        c16 = color_256_to_rgb(16); /* first cube entry: level 0,0,0 */
        TEST_CHECK(c16.r == 0 && c16.g == 0 && c16.b == 0, "256_to_rgb: index 16 (cube origin) is black");

        c231 = color_256_to_rgb(231); /* last cube entry: level 5,5,5 */
        TEST_CHECK(c231.r == 255 && c231.g == 255 && c231.b == 255, "256_to_rgb: index 231 (cube far corner) is white");

        c232 = color_256_to_rgb(232); /* first grayscale step */
        TEST_CHECK(c232.r == 8 && c232.r == c232.g && c232.g == c232.b, "256_to_rgb: index 232 is the darkest grayscale step (8,8,8)");
    }

    /* color_rgb_to_16 */
    {
        Color_Rgb pure_red = {255, 0, 0};
        Color_Rgb pure_black = {0, 0, 0};
        u8 red_idx = color_rgb_to_16(pure_red);
        u8 black_idx = color_rgb_to_16(pure_black);
        TEST_CHECK(red_idx < 16, "rgb_to_16: returns a valid 0-15 index");
        TEST_CHECK(black_idx == 0, "rgb_to_16: pure black maps to index 0");
        TEST_CHECK(red_idx != black_idx, "rgb_to_16: distinct colors map to distinct indices");
    }

    /* SGR escape builders - exact byte output */
    {
        char buf[64];
        i32 n;
        Color_Rgb c = {255, 128, 0};

        n = color_write_sgr_rgb(buf, c, 0);
        buf[n] = '\0';
        TEST_CHECK(strcmp(buf, "\033[38;2;255;128;0m") == 0, "write_sgr_rgb: foreground truecolor escape bytes exact");

        n = color_write_sgr_rgb(buf, c, 1);
        buf[n] = '\0';
        TEST_CHECK(strcmp(buf, "\033[48;2;255;128;0m") == 0, "write_sgr_rgb: background truecolor escape bytes exact");

        n = color_write_sgr_256(buf, 196, 0);
        buf[n] = '\0';
        TEST_CHECK(strcmp(buf, "\033[38;5;196m") == 0, "write_sgr_256: foreground 256-color escape bytes exact");

        n = color_write_sgr_256(buf, 21, 1);
        buf[n] = '\0';
        TEST_CHECK(strcmp(buf, "\033[48;5;21m") == 0, "write_sgr_256: background 256-color escape bytes exact");
    }

    print_flush();
    return test_failures_ != 0;
}
