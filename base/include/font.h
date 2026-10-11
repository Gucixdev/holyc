#ifndef HOLYC_FONT_H_
#define HOLYC_FONT_H_

/* Font metrics/glyph-lookup layer on top of `src/codec/bdf_dec.h`'s
 * already-parsed `Bdf_Font` - NOT a font loader or rasterizer itself
 * (bdf_dec.h already owns parsing, `bdf_find_glyph`/`bdf_glyph_bit`
 * already own glyph lookup/bitmap sampling). This file's whole job is
 * two functions shaped EXACTLY like `src/uiux/im_gui.h`'s own
 * `Im_Gui_Font`/`text_width`/`text_height` callback interface
 * (`i32 (*text_width)(Im_Gui_Font, const char*, i32)` /
 * `i32 (*text_height)(Im_Gui_Font)`, read that file's own struct for
 * the exact shape) - so a caller can wire `font_text_width`/
 * `font_text_height` straight in as those two callbacks (cast a
 * `const Bdf_Font*` to `Im_Gui_Font`, which is just `void*`), closing
 * the loop between the codec and the GUI layer without either one
 * depending on the other directly. im_gui.h itself is untouched, not
 * included here, and has no dependency on this file - the connection
 * is the CALLER'S choice to make by matching function pointer shapes,
 * not a hard link between the two modules.
 *
 * Real scope note: this is genuinely NOT im_gui.h's own layout system
 * (`Im_Gui_Layout`/`im_gui_layout_row` etc, already fully implemented
 * there, tightly coupled to its per-frame immediate-mode state) - see
 * `src/uiux/layout.h` for the separate, standalone rect-layout
 * algorithm this session also added, which likewise doesn't touch
 * im_gui.h's own layout stack. */

#include "base.h"
#include "bdf_dec.h"

/* sums each character's DWIDTH (horizontal advance) from the font's
 * glyph table. len == -1 means "str is NUL-terminated" (im_gui.h's own
 * convention for this exact parameter - see its im_gui_draw_text call
 * sites). A codepoint with no glyph in this font contributes 0 width,
 * not a crash or an arbitrary fallback - a documented gap matching
 * bdf_dec.h's own "glyphs missing DWIDTH are rejected at parse time,
 * not defaulted" stance on malformed data, extended here to simply
 * "not present in this font" at query time. */
i32 font_text_width(const Bdf_Font *font, const char *str, i32 len);

/* FONT_ASCENT + FONT_DESCENT - the font's own declared line height.
 * 0 if the source BDF file's STARTPROPERTIES didn't specify either
 * (bdf_dec.h's own documented default for that case). */
i32 font_text_height(const Bdf_Font *font);

#endif /* HOLYC_FONT_H_ */

#ifdef FONT_IMPLEMENTATION
#ifndef FONT_IMPLEMENTATION_
#define FONT_IMPLEMENTATION_

i32 font_text_width(const Bdf_Font *font, const char *str, i32 len)
{
    i32 width = 0;
    i32 i = 0;

    while ((len < 0) ? (str[i] != '\0') : (i < len)) {
        const Bdf_Glyph *g = bdf_find_glyph(font, (i32)(u8)str[i]);
        if (g) width += g->dwidth_x;
        i++;
    }

    return width;
}

i32 font_text_height(const Bdf_Font *font)
{
    return font->ascent + font->descent;
}

#endif /* FONT_IMPLEMENTATION_ */
#endif /* FONT_IMPLEMENTATION */
