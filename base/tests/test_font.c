#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#define STRING_VIEW_IMPLEMENTATION
#define BDF_DEC_IMPLEMENTATION
#define FONT_IMPLEMENTATION
#include "arena_alloc.h"
#include "bdf_dec.h"
#include "font.h"
#include <string.h>

/* same sample BDF this project's own tests/test_bdf.c already uses -
 * two glyphs, 'A' (65) and 'B' (66), both DWIDTH 8 0, FONT_ASCENT 7,
 * FONT_DESCENT 1. */
static const char *font_bdf_sample_ =
    "STARTFONT 2.1\n"
    "FONT -test-test-medium-r-normal--8-80-75-75-C-80-iso10646-1\n"
    "SIZE 8 75 75\n"
    "FONTBOUNDINGBOX 8 8 0 -1\n"
    "STARTPROPERTIES 2\n"
    "FONT_ASCENT 7\n"
    "FONT_DESCENT 1\n"
    "ENDPROPERTIES\n"
    "CHARS 2\n"
    "STARTCHAR A\n"
    "ENCODING 65\n"
    "SWIDTH 600 0\n"
    "DWIDTH 8 0\n"
    "BBX 8 8 0 -1\n"
    "BITMAP\n"
    "18\n"
    "24\n"
    "42\n"
    "7E\n"
    "42\n"
    "42\n"
    "42\n"
    "00\n"
    "ENDCHAR\n"
    "STARTCHAR B\n"
    "ENCODING 66\n"
    "SWIDTH 600 0\n"
    "DWIDTH 8 0\n"
    "BBX 8 8 0 -1\n"
    "BITMAP\n"
    "7C\n"
    "42\n"
    "42\n"
    "7C\n"
    "42\n"
    "42\n"
    "7C\n"
    "00\n"
    "ENDCHAR\n"
    "ENDFONT\n";

int main(void)
{
    Arena a = {0};
    Bdf_Font font;
    i32 ok;

    print_cstr("test_font\n");

    ok = bdf_parse(&a, (const u8 *)font_bdf_sample_, strlen(font_bdf_sample_), &font);
    TEST_CHECK(ok, "sample BDF font parses successfully");

    /* text_height: ascent + descent = 7 + 1 = 8 */
    TEST_CHECK(font_text_height(&font) == 8, "font_text_height == FONT_ASCENT + FONT_DESCENT (7+1=8)");

    /* text_width of a single known glyph == its own DWIDTH */
    TEST_CHECK(font_text_width(&font, "A", 1) == 8, "font_text_width(\"A\", len=1) == glyph A's DWIDTH (8)");

    /* text_width of two known glyphs sums both advances */
    TEST_CHECK(font_text_width(&font, "AB", 2) == 16, "font_text_width(\"AB\", len=2) == sum of both glyphs' DWIDTH (8+8=16)");

    /* len == -1 means NUL-terminated, matching im_gui.h's own
       text_width(font, str, -1) calling convention exactly */
    TEST_CHECK(font_text_width(&font, "AB", -1) == 16, "font_text_width with len=-1 (NUL-terminated) matches the explicit-length result");

    /* empty string has zero width */
    TEST_CHECK(font_text_width(&font, "", 0) == 0, "font_text_width of an empty string is 0");

    /* a codepoint with no glyph in this font contributes 0, not a crash
       or a garbage width - documented gap, not silently wrong */
    TEST_CHECK(font_text_width(&font, "Z", 1) == 0, "font_text_width of an unmapped codepoint contributes 0, doesn't crash");

    print_flush();
    return test_failures_ != 0;
}
