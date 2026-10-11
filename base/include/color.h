#ifndef HOLYC_COLOR_H_
#define HOLYC_COLOR_H_

/* Terminal color utilities that sit ABOVE tui.h's own color handling, not
 * duplicating it: tui.h's Tui_Cell/tui_write_sgr only know the basic
 * 16-color ANSI palette (TUI_BLACK..TUI_WHITE, packed into a u16 with
 * TUI_BOLD/UNDERLINE/REVERSE flag bits) - that's genuinely all termbox
 * (what tui.h is ported from) supports, and it's enough for a plain cell
 * buffer. This file covers what tui.h deliberately doesn't: real 24-bit
 * RGB, its approximation down to a 256-color xterm palette index or the
 * basic 16-color set (for terminals/pipes that don't support truecolor),
 * and the richer SGR escape forms (38;2;R;G;Bm truecolor, 38;5;Nm
 * 256-color) that tui_write_sgr's 4-bit-only encoding can't express.
 * A caller wanting full 24-bit color output writes its own escape bytes
 * via color_write_sgr_rgb below rather than going through Tui_Cell/
 * tui_write_sgr at all - two different color models, not a partial
 * upgrade of one into the other. */

#include "base.h"

typedef struct { u8 r, g, b; } Color_Rgb;

/* xterm 256-color palette layout: 0-15 system colors (not reproduced
 * here, that's tui.h's TUI_* 16-color set), 16-231 a 6x6x6 RGB color
 * cube, 232-255 a 24-step grayscale ramp. color_rgb_to_256 picks
 * whichever of "nearest cube cell" or "nearest grayscale step" is
 * actually closer (squared Euclidean distance in RGB space), not just
 * always the cube - a near-gray RGB value looks visibly better through
 * the dedicated grayscale ramp than through the coarser 6-level cube. */
u8 color_rgb_to_256(Color_Rgb c);
Color_Rgb color_256_to_rgb(u8 index);

/* nearest of the 16 basic ANSI colors (8 normal + 8 bright, indices
 * 0-15, same numbering xterm's own 16-color table uses) - the coarsest
 * fallback, for a terminal/pipe that supports neither truecolor nor
 * 256-color. Return value is a raw 0-15 index, NOT a TUI_* constant
 * (tui.h's TUI_BLACK..TUI_WHITE are a 1-based 8-color subset with
 * different numbering - deliberately not reused here, converting
 * between the two numbering schemes belongs to whichever caller
 * actually bridges color.h's RGB world and tui.h's Tui_Cell world). */
u8 color_rgb_to_16(Color_Rgb c);

/* pure escape-sequence builders, same shape as tui_write_cursor/
 * tui_write_sgr (write into caller's buf, return bytes written, no I/O,
 * fully testable without a real tty). is_bg selects foreground (0) or
 * background (1) - SGR 38 vs 48. */
i32 color_write_sgr_rgb(char *buf, Color_Rgb c, i32 is_bg);
i32 color_write_sgr_256(char *buf, u8 index, i32 is_bg);

#endif /* HOLYC_COLOR_H_ */

#ifdef COLOR_IMPLEMENTATION
#ifndef COLOR_IMPLEMENTATION_
#define COLOR_IMPLEMENTATION_

/* the 6 levels the RGB cube's own components are quantized to (xterm's
 * actual chosen levels, not an even 0/51/102/153/204/255 split) */
static const u8 color_cube_levels_[6] = { 0, 95, 135, 175, 215, 255 };

static i32 color_write_uint_(char *buf, u32 v)
{
    i32 l = 0;
    char tmp[16];
    i32 i;
    do { tmp[l++] = (char)('0' + v % 10); v /= 10; } while (v);
    for (i = 0; i < l; i++) buf[i] = tmp[l - 1 - i];
    return l;
}

static u8 color_nearest_cube_level_(u8 v)
{
    /* which of the 6 fixed levels is closest to v - a small fixed
     * search, not a division, since the levels aren't evenly spaced */
    u8 best = 0;
    i32 best_d = 256;
    i32 i;
    for (i = 0; i < 6; i++) {
        i32 d = (i32)v - (i32)color_cube_levels_[i];
        if (d < 0) d = -d;
        if (d < best_d) { best_d = d; best = (u8)i; }
    }
    return best;
}

u8 color_rgb_to_256(Color_Rgb c)
{
    u8 ri = color_nearest_cube_level_(c.r);
    u8 gi = color_nearest_cube_level_(c.g);
    u8 bi = color_nearest_cube_level_(c.b);
    u8 cube_index = (u8)(16 + 36 * ri + 6 * gi + bi);
    Color_Rgb cube_rgb;
    i32 cube_d, gray_d, gray_level, gray_value;
    u8 gray_index;

    cube_rgb.r = color_cube_levels_[ri];
    cube_rgb.g = color_cube_levels_[gi];
    cube_rgb.b = color_cube_levels_[bi];
    cube_d = ((i32)c.r - cube_rgb.r) * ((i32)c.r - cube_rgb.r) +
             ((i32)c.g - cube_rgb.g) * ((i32)c.g - cube_rgb.g) +
             ((i32)c.b - cube_rgb.b) * ((i32)c.b - cube_rgb.b);

    /* grayscale ramp: 24 steps, value = 8 + 10*level, level 0..23 */
    gray_level = ((i32)c.r + (i32)c.g + (i32)c.b) / 3;
    gray_level = (gray_level - 8) / 10;
    if (gray_level < 0) gray_level = 0;
    if (gray_level > 23) gray_level = 23;
    gray_value = 8 + 10 * gray_level;
    gray_index = (u8)(232 + gray_level);
    gray_d = ((i32)c.r - gray_value) * ((i32)c.r - gray_value) +
             ((i32)c.g - gray_value) * ((i32)c.g - gray_value) +
             ((i32)c.b - gray_value) * ((i32)c.b - gray_value);

    return (gray_d < cube_d) ? gray_index : cube_index;
}

Color_Rgb color_256_to_rgb(u8 index)
{
    Color_Rgb out;

    if (index < 16) {
        /* system colors: standard-ish xterm defaults for the basic 16 -
         * not authoritative (real terminals let users theme these), but
         * a reasonable fixed answer for round-tripping color_rgb_to_16's
         * own output through this function. */
        static const Color_Rgb basic16_[16] = {
            {0,0,0}, {205,0,0}, {0,205,0}, {205,205,0},
            {0,0,238}, {205,0,205}, {0,205,205}, {229,229,229},
            {127,127,127}, {255,0,0}, {0,255,0}, {255,255,0},
            {92,92,255}, {255,0,255}, {0,255,255}, {255,255,255}
        };
        return basic16_[index];
    }
    if (index < 232) {
        u8 i = (u8)(index - 16);
        out.r = color_cube_levels_[i / 36];
        out.g = color_cube_levels_[(i / 6) % 6];
        out.b = color_cube_levels_[i % 6];
        return out;
    }
    {
        u8 level = (u8)(index - 232);
        u8 v = (u8)(8 + 10 * level);
        out.r = v; out.g = v; out.b = v;
        return out;
    }
}

u8 color_rgb_to_16(Color_Rgb c)
{
    /* same basic16_ table color_256_to_rgb uses for indices 0-15,
     * nearest by squared distance - a small fixed search over 16
     * candidates, no cube/grayscale split needed at this coarseness */
    static const Color_Rgb basic16_[16] = {
        {0,0,0}, {205,0,0}, {0,205,0}, {205,205,0},
        {0,0,238}, {205,0,205}, {0,205,205}, {229,229,229},
        {127,127,127}, {255,0,0}, {0,255,0}, {255,255,0},
        {92,92,255}, {255,0,255}, {0,255,255}, {255,255,255}
    };
    u8 best = 0;
    i32 best_d = 0x7fffffff;
    i32 i;
    for (i = 0; i < 16; i++) {
        i32 dr = (i32)c.r - basic16_[i].r;
        i32 dg = (i32)c.g - basic16_[i].g;
        i32 db = (i32)c.b - basic16_[i].b;
        i32 d = dr * dr + dg * dg + db * db;
        if (d < best_d) { best_d = d; best = (u8)i; }
    }
    return best;
}

i32 color_write_sgr_rgb(char *buf, Color_Rgb c, i32 is_bg)
{
    i32 n = 0;
    buf[n++] = '\033'; buf[n++] = '[';
    buf[n++] = is_bg ? '4' : '3';
    buf[n++] = '8'; buf[n++] = ';'; buf[n++] = '2'; buf[n++] = ';';
    n += color_write_uint_(buf + n, c.r);
    buf[n++] = ';';
    n += color_write_uint_(buf + n, c.g);
    buf[n++] = ';';
    n += color_write_uint_(buf + n, c.b);
    buf[n++] = 'm';
    return n;
}

i32 color_write_sgr_256(char *buf, u8 index, i32 is_bg)
{
    i32 n = 0;
    buf[n++] = '\033'; buf[n++] = '[';
    buf[n++] = is_bg ? '4' : '3';
    buf[n++] = '8'; buf[n++] = ';'; buf[n++] = '5'; buf[n++] = ';';
    n += color_write_uint_(buf + n, index);
    buf[n++] = 'm';
    return n;
}

#endif /* COLOR_IMPLEMENTATION_ */
#endif /* COLOR_IMPLEMENTATION */
