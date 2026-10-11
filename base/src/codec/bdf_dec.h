#ifndef HOLYC_BDF_DEC_H_
#define HOLYC_BDF_DEC_H_

/* BDF (Glyph Bitmap Distribution Format, Adobe/X11 spec 2.1) parser - no
 * vendored reference used, the format is a simple, well-documented plain
 * text layout (unlike TrueType/OpenType's binary tables), general
 * algorithm/spec knowledge same as rng.h/catmull.h. Built on
 * string_view.h for line/token scanning rather than hand-rolled pointer
 * arithmetic, since BDF is inherently line-oriented (one directive per
 * line, whitespace-separated fields).
 *
 * Directive dispatch goes through perfecthash.h, not an if/else-if chain
 * - a fixed, known keyword set is exactly what that module exists for
 * (see its own header comment), and a switch over a dense integer id
 * compiles to a jump table instead of a sequential chain of string
 * comparisons, matching this project's own branchless/jump-table stance
 * (a switch/case used as a jump table).
 *
 * Only the subset needed to actually rasterize text is parsed:
 * FONTBOUNDINGBOX, FONT_ASCENT/FONT_DESCENT (if present in
 * STARTPROPERTIES), and per-glyph ENCODING/DWIDTH/BBX/BITMAP. Everything
 * else (COMMENT, the FONT name line's XLFD fields, SWIDTH, most
 * STARTPROPERTIES entries) is skipped, not because it's hard to parse but
 * because nothing in this project reads it yet - a real gap to note, not
 * a silent one. Glyphs missing DWIDTH or BBX are rejected (malformed
 * file), not defaulted to zero - a font that doesn't know a glyph's
 * advance width or bitmap dimensions can't be laid out correctly anyway. */

#include "core/base.h"
#include "core/arena_alloc.h"
#include "core/str.h"

typedef struct {
    i32 codepoint;
    i32 width, height;      /* BBX: bitmap dimensions in pixels */
    i32 xoff, yoff;          /* BBX: offset from the font's origin */
    i32 dwidth_x;             /* DWIDTH: horizontal advance in pixels */
    u8 *bitmap;                /* [row_bytes * height], row_bytes = ceil(width/8), MSB-first per row */
} Bdf_Glyph;

typedef struct {
    i32 bbx_w, bbx_h, bbx_xoff, bbx_yoff; /* font-wide FONTBOUNDINGBOX */
    i32 ascent, descent;                    /* 0 if the file's STARTPROPERTIES didn't specify them */
    Bdf_Glyph *glyphs;                       /* sorted by codepoint, for bdf_find_glyph's binary search */
    size_t glyph_count;
} Bdf_Font;

/* returns 1 on success, 0 on a malformed file (*out_font is left
 * unspecified on failure - don't use it). */
i32 bdf_parse(Arena *a, const u8 *data, size_t size, Bdf_Font *out_font);

/* binary search by codepoint - NULL if the font has no glyph for it. */
const Bdf_Glyph *bdf_find_glyph(const Bdf_Font *font, i32 codepoint);

/* (x, y) in [0,width) x [0,height), row-major from the glyph's own top-left
 * (BBX's own local coordinate space, not the font-wide origin - offset by
 * xoff/yoff yourself when compositing). Out-of-range (x, y) returns 0. */
i32 bdf_glyph_bit(const Bdf_Glyph *g, i32 x, i32 y);

#endif /* HOLYC_BDF_DEC_H_ */

#ifdef BDF_DEC_IMPLEMENTATION
#ifndef BDF_DEC_IMPLEMENTATION_
#define BDF_DEC_IMPLEMENTATION_

#define PERFECTHASH_IMPLEMENTATION
#include "perfecthash.h"

#define BDF_KW_FONTBOUNDINGBOX 0
#define BDF_KW_FONT_ASCENT     1
#define BDF_KW_FONT_DESCENT    2
#define BDF_KW_CHARS           3
#define BDF_KW_STARTCHAR       4
#define BDF_KW_ENCODING        5
#define BDF_KW_DWIDTH          6
#define BDF_KW_BBX             7
#define BDF_KW_BITMAP          8
#define BDF_KW_ENDCHAR         9
#define BDF_KW_ENDFONT         10
#define BDF_KW_COUNT_          11

static const char * const bdf_keywords_[BDF_KW_COUNT_] = {
    "FONTBOUNDINGBOX", "FONT_ASCENT", "FONT_DESCENT", "CHARS", "STARTCHAR",
    "ENCODING", "DWIDTH", "BBX", "BITMAP", "ENDCHAR", "ENDFONT"
};

/* -1 for anything not in bdf_keywords_ (BDF has plenty of directives -
 * COMMENT, FONT, SIZE, SWIDTH, most STARTPROPERTIES entries - this file
 * doesn't act on; a miss here just means "ignore this line", not a parse
 * error). perfect_hash_lookup returns *some* in-range slot even for an
 * unknown key (same contract every minimal perfect hash makes), so the
 * strcmp against the slot's real keyword is the actual membership test.
 *
 * slot_to_id maps a hash slot back to its BDF_KW_* id - perfect_hash_build
 * assigns each key to whichever slot its own hash+displacement search
 * lands on, NOT the key's position in bdf_keywords_[], so slot and id are
 * two different index spaces that must be bridged explicitly (built once
 * in bdf_parse below, the same pattern test_perfecthash.c's own
 * slot_to_key verification array uses) - indexing bdf_keywords_ directly
 * by the raw slot, as an earlier version of this function did, silently
 * returns the WRONG keyword for every key whose slot doesn't happen to
 * equal its array index (10 of 11 keywords here) and was a real, caught
 * bug, not a hypothetical one. */
static i32 bdf_keyword_id_(const Perfect_Hash *ph, const i32 *slot_to_id, String_View keyword)
{
    char buf[24];
    size_t slot;
    i32 id;
    if (keyword.count >= sizeof(buf)) return -1;
    memcpy(buf, keyword.data, keyword.count);
    buf[keyword.count] = '\0';
    slot = perfect_hash_lookup(ph, buf);
    id = slot_to_id[slot];
    if (strcmp(buf, bdf_keywords_[id]) != 0) return -1;
    return id;
}

/* branchless sign: is_neg is 0 or 1, never a condition the CPU has to
 * predict - 1 - 2*is_neg maps {0,1} -> {1,-1} with no comparison, and
 * chopping `is_neg` characters (0 or 1) unconditionally consumes the
 * '-' when present and nothing when absent, instead of an if guarding
 * the chop. */
static i32 bdf_chop_i32_(String_View *sv)
{
    i32 is_neg;
    i32 sign;
    *sv = sv_trim_left(*sv);
    is_neg = (sv->count > 0 && sv->data[0] == '-');
    sign = 1 - 2 * is_neg;
    sv_chop_left(sv, (size_t)is_neg);
    return sign * (i32)sv_chop_u64(sv);
}

/* 256-entry LUT instead of a 3-way range-check chain - one indexed load,
 * no branch, and it naturally covers the "not a hex digit" case (-1) as
 * just another table entry instead of a fourth comparison. */
static const i8 bdf_hex_lut_[256] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    0,1,2,3,4,5,6,7,8,9,-1,-1,-1,-1,-1,-1,
    -1,10,11,12,13,14,15,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,10,11,12,13,14,15,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
};

static i32 bdf_hex_digit_(char c)
{
    return bdf_hex_lut_[(u8)c];
}

static i32 bdf_glyph_cmp_(const void *a, const void *b)
{
    i32 ca = ((const Bdf_Glyph *)a)->codepoint;
    i32 cb = ((const Bdf_Glyph *)b)->codepoint;
    return (ca > cb) - (ca < cb);
}

i32 bdf_parse(Arena *a, const u8 *data, size_t size, Bdf_Font *out_font)
{
    String_View rest = sv_from_parts((const char *)data, size);
    Bdf_Font font;
    Perfect_Hash kw_hash;
    i32 slot_to_id[BDF_KW_COUNT_];
    size_t glyph_capacity = 0;
    size_t glyph_used = 0;
    i32 in_glyph = 0;
    i32 end_of_font = 0;
    Bdf_Glyph cur;
    i32 have_dwidth, have_bbx;
    i32 bitmap_rows_left = 0;
    size_t bitmap_row_bytes = 0;
    size_t kw_i;

    if (!perfect_hash_build(a, &kw_hash, bdf_keywords_, BDF_KW_COUNT_)) return 0;
    for (kw_i = 0; kw_i < BDF_KW_COUNT_; kw_i++) {
        slot_to_id[perfect_hash_lookup(&kw_hash, bdf_keywords_[kw_i])] = (i32)kw_i;
    }

    font.bbx_w = font.bbx_h = font.bbx_xoff = font.bbx_yoff = 0;
    font.ascent = font.descent = 0;
    font.glyphs = NULL;
    font.glyph_count = 0;
    have_dwidth = have_bbx = 0;
    cur.bitmap = NULL;

    while (rest.count > 0 && !end_of_font) {
        String_View line = sv_trim(sv_chop_by_delim(&rest, '\n'));
        String_View keyword;

        if (line.count == 0) continue;

        if (bitmap_rows_left > 0) {
            size_t i;
            u8 *row = cur.bitmap + (size_t)(cur.height - bitmap_rows_left) * bitmap_row_bytes;
            String_View hex = line;

            for (i = 0; i < bitmap_row_bytes; i++) {
                i32 hi = (hex.count >= 2) ? bdf_hex_digit_(hex.data[0]) : -1;
                i32 lo = (hex.count >= 2) ? bdf_hex_digit_(hex.data[1]) : -1;
                if (hi < 0 || lo < 0) return 0;
                row[i] = (u8)((hi << 4) | lo);
                sv_chop_left(&hex, 2);
            }
            bitmap_rows_left--;
            continue;
        }

        keyword = sv_chop_by_delim(&line, ' ');

        switch (bdf_keyword_id_(&kw_hash, slot_to_id, keyword)) {
        case BDF_KW_FONTBOUNDINGBOX:
            font.bbx_w = bdf_chop_i32_(&line);
            font.bbx_h = bdf_chop_i32_(&line);
            font.bbx_xoff = bdf_chop_i32_(&line);
            font.bbx_yoff = bdf_chop_i32_(&line);
            break;
        case BDF_KW_FONT_ASCENT:
            font.ascent = bdf_chop_i32_(&line);
            break;
        case BDF_KW_FONT_DESCENT:
            font.descent = bdf_chop_i32_(&line);
            break;
        case BDF_KW_CHARS:
            glyph_capacity = (size_t)bdf_chop_i32_(&line);
            font.glyphs = (Bdf_Glyph *)arena_alloc(a, glyph_capacity * sizeof(Bdf_Glyph));
            break;
        case BDF_KW_STARTCHAR:
            if (glyph_used >= glyph_capacity) return 0; /* more glyphs than CHARS declared */
            in_glyph = 1;
            have_dwidth = have_bbx = 0;
            cur.codepoint = -1;
            cur.width = cur.height = cur.xoff = cur.yoff = 0;
            cur.dwidth_x = 0;
            cur.bitmap = NULL;
            break;
        case BDF_KW_ENCODING:
            if (in_glyph) cur.codepoint = bdf_chop_i32_(&line);
            break;
        case BDF_KW_DWIDTH:
            if (!in_glyph) break;
            cur.dwidth_x = bdf_chop_i32_(&line);
            have_dwidth = 1;
            break;
        case BDF_KW_BBX:
            if (!in_glyph) break;
            cur.width = bdf_chop_i32_(&line);
            cur.height = bdf_chop_i32_(&line);
            cur.xoff = bdf_chop_i32_(&line);
            cur.yoff = bdf_chop_i32_(&line);
            have_bbx = 1;
            break;
        case BDF_KW_BITMAP:
            if (!in_glyph || !have_bbx || cur.width < 0 || cur.height < 0) return 0;
            bitmap_row_bytes = (size_t)((cur.width + 7) / 8);
            cur.bitmap = (u8 *)arena_alloc(a, bitmap_row_bytes * (size_t)cur.height);
            bitmap_rows_left = cur.height;
            break;
        case BDF_KW_ENDCHAR:
            if (!in_glyph || !have_dwidth || !have_bbx || cur.codepoint < 0) return 0;
            font.glyphs[glyph_used++] = cur;
            in_glyph = 0;
            break;
        case BDF_KW_ENDFONT:
            end_of_font = 1;
            break;
        default:
            /* COMMENT, FONT, SIZE, SWIDTH, STARTFONT, STARTPROPERTIES/
             * ENDPROPERTIES and its other entries - see file header */
            break;
        }
    }

    font.glyph_count = glyph_used;
    qsort(font.glyphs, font.glyph_count, sizeof(Bdf_Glyph), bdf_glyph_cmp_);

    *out_font = font;
    return 1;
}

const Bdf_Glyph *bdf_find_glyph(const Bdf_Font *font, i32 codepoint)
{
    size_t lo = 0, hi = font->glyph_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        i32 c = font->glyphs[mid].codepoint;
        if (c == codepoint) return &font->glyphs[mid];
        if (c < codepoint) lo = mid + 1; else hi = mid;
    }
    return NULL;
}

i32 bdf_glyph_bit(const Bdf_Glyph *g, i32 x, i32 y)
{
    size_t row_bytes;
    if (x < 0 || y < 0 || x >= g->width || y >= g->height) return 0;
    row_bytes = (size_t)((g->width + 7) / 8);
    return (g->bitmap[(size_t)y * row_bytes + (size_t)(x / 8)] >> (7 - (x % 8))) & 1;
}

#endif /* BDF_DEC_IMPLEMENTATION_ */
#endif /* BDF_DEC_IMPLEMENTATION */
