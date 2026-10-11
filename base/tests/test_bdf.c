#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#define STRING_VIEW_IMPLEMENTATION
#define BDF_DEC_IMPLEMENTATION
#include "arena_alloc.h"
#include "bdf_dec.h"
#include <string.h>

static const char *bdf_sample_ =
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
    const Bdf_Glyph *ga, *gb, *missing;

    print_cstr("test_bdf\n");

    ok = bdf_parse(&a, (const u8 *)bdf_sample_, strlen(bdf_sample_), &font);
    TEST_CHECK(ok, "parses a well-formed BDF sample");

    TEST_CHECK(font.bbx_w == 8 && font.bbx_h == 8 && font.bbx_xoff == 0 && font.bbx_yoff == -1,
               "FONTBOUNDINGBOX parsed correctly, including the negative yoff");
    TEST_CHECK(font.ascent == 7 && font.descent == 1, "FONT_ASCENT/FONT_DESCENT parsed from STARTPROPERTIES");
    TEST_CHECK(font.glyph_count == 2, "both STARTCHAR/ENDCHAR blocks became glyphs");

    ga = bdf_find_glyph(&font, 65);
    TEST_CHECK(ga != NULL, "find_glyph locates codepoint 65 ('A')");
    if (ga) {
        TEST_CHECK(ga->width == 8 && ga->height == 8, "'A' BBX dimensions parsed correctly");
        TEST_CHECK(ga->dwidth_x == 8, "'A' DWIDTH parsed correctly");
        /* row 0 = 0x18 = 00011000 - bits set at x=3,4 only */
        TEST_CHECK(bdf_glyph_bit(ga, 3, 0) == 1 && bdf_glyph_bit(ga, 4, 0) == 1,
                   "row 0 (0x18) has the expected bits set at x=3,4");
        TEST_CHECK(bdf_glyph_bit(ga, 0, 0) == 0 && bdf_glyph_bit(ga, 7, 0) == 0,
                   "row 0 (0x18) has the expected bits clear at x=0,7");
        /* row 3 = 0x7E = 01111110 */
        TEST_CHECK(bdf_glyph_bit(ga, 0, 3) == 0 && bdf_glyph_bit(ga, 1, 3) == 1 && bdf_glyph_bit(ga, 7, 3) == 0,
                   "row 3 (0x7E) matches at x=0,1,7");
    }

    gb = bdf_find_glyph(&font, 66);
    TEST_CHECK(gb != NULL, "find_glyph locates codepoint 66 ('B')");
    if (gb) {
        /* row 0 = 0x7C = 01111100 */
        TEST_CHECK(bdf_glyph_bit(gb, 0, 0) == 0 && bdf_glyph_bit(gb, 1, 0) == 1 && bdf_glyph_bit(gb, 6, 0) == 0,
                   "'B' row 0 (0x7C) matches at x=0,1,6");
    }

    missing = bdf_find_glyph(&font, 67);
    TEST_CHECK(missing == NULL, "find_glyph returns NULL for an encoding not present in the font");

    TEST_CHECK(bdf_glyph_bit(ga, -1, 0) == 0 && bdf_glyph_bit(ga, 100, 0) == 0,
               "glyph_bit returns 0 (not a crash) for out-of-range coordinates");

    {
        Arena a2 = {0};
        Bdf_Font bad_font;
        const char *garbage = "not a bdf file at all\nENDCHAR\n";
        i32 bad_ok = bdf_parse(&a2, (const u8 *)garbage, strlen(garbage), &bad_font);
        TEST_CHECK(!bad_ok, "malformed input (ENDCHAR with no matching STARTCHAR) is rejected, not crashed on");
        arena_free(&a2);
    }

    arena_free(&a);
    print_flush();
    return test_failures_ != 0;
}
