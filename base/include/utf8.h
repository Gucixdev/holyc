#ifndef HOLYC_UTF8_H_
#define HOLYC_UTF8_H_

/* General-purpose UTF-8 codec: decode/encode a single codepoint,
 * count codepoints, validate a whole buffer. tui.h already has its
 * OWN private utf8 decode helpers for cell-width rendering (own local
 * copy, per this project's usual no-cross-IMPLEMENTATION-guard-
 * dependency rule) - this file is the general, standalone,
 * reusable primitive for everything else that needs real UTF-8, not
 * a replacement for tui.h's internal copy.
 *
 * Rejects overlong encodings and the UTF-16 surrogate range
 * (0xD800-0xDFFF), same as any conforming decoder must - a 3-byte
 * sequence that LOOKS structurally valid but encodes a surrogate
 * codepoint is still invalid UTF-8. */

#include "base.h"

/* decodes one codepoint starting at bytes[0]. Returns the number of
 * bytes consumed (1-4) on success, 0 on any malformed sequence
 * (leaves *out_codepoint untouched on failure). */
i32 utf8_decode(const u8 *bytes, size_t len, u32 *out_codepoint);

/* encodes one codepoint into out[0..return value). Returns the number
 * of bytes written (1-4), or 0 if codepoint is out of Unicode's valid
 * range (> 0x10FFFF) or a surrogate (0xD800-0xDFFF) - those never have
 * a valid UTF-8 encoding. */
i32 utf8_encode(u32 codepoint, u8 out[4]);

/* number of codepoints in a NUL-terminated UTF-8 C string (not bytes -
 * see base.h's own str_len/strlen for that). Malformed bytes are
 * skipped one at a time (does not stop counting, same "keep going
 * past bad input" spirit hex_decode/str.h use elsewhere). */
size_t utf8_strlen(const char *s);

i32 utf8_validate(const void *data, size_t len); /* 1 if every byte forms a well-formed sequence, 0 on the first malformed one */

#endif /* HOLYC_UTF8_H_ */

#ifdef UTF8_IMPLEMENTATION
#ifndef UTF8_IMPLEMENTATION_
#define UTF8_IMPLEMENTATION_

static i32 utf8_is_cont_(u8 b) { return (b & 0xC0) == 0x80; }

i32 utf8_decode(const u8 *bytes, size_t len, u32 *out_codepoint)
{
    u8 b0;

    if (len < 1) return 0;
    b0 = bytes[0];

    if (b0 < 0x80) {
        *out_codepoint = b0;
        return 1;
    }

    if ((b0 & 0xE0) == 0xC0) {
        u32 cp;
        if (len < 2 || !utf8_is_cont_(bytes[1])) return 0;
        cp = ((u32)(b0 & 0x1F) << 6) | (u32)(bytes[1] & 0x3F);
        if (cp < 0x80) return 0; /* overlong */
        *out_codepoint = cp;
        return 2;
    }

    if ((b0 & 0xF0) == 0xE0) {
        u32 cp;
        if (len < 3 || !utf8_is_cont_(bytes[1]) || !utf8_is_cont_(bytes[2])) return 0;
        cp = ((u32)(b0 & 0x0F) << 12) | ((u32)(bytes[1] & 0x3F) << 6) | (u32)(bytes[2] & 0x3F);
        if (cp < 0x800) return 0; /* overlong */
        if (cp >= 0xD800 && cp <= 0xDFFF) return 0; /* surrogate range */
        *out_codepoint = cp;
        return 3;
    }

    if ((b0 & 0xF8) == 0xF0) {
        u32 cp;
        if (len < 4 || !utf8_is_cont_(bytes[1]) || !utf8_is_cont_(bytes[2]) || !utf8_is_cont_(bytes[3])) return 0;
        cp = ((u32)(b0 & 0x07) << 18) | ((u32)(bytes[1] & 0x3F) << 12)
           | ((u32)(bytes[2] & 0x3F) << 6) | (u32)(bytes[3] & 0x3F);
        if (cp < 0x10000 || cp > 0x10FFFF) return 0; /* overlong or past Unicode's max */
        *out_codepoint = cp;
        return 4;
    }

    return 0; /* stray continuation byte or an invalid leading byte (0x80-0xBF, 0xF8-0xFF) */
}

i32 utf8_encode(u32 codepoint, u8 out[4])
{
    if (codepoint > 0x10FFFF) return 0;
    if (codepoint >= 0xD800 && codepoint <= 0xDFFF) return 0;

    if (codepoint < 0x80) {
        out[0] = (u8)codepoint;
        return 1;
    }
    if (codepoint < 0x800) {
        out[0] = (u8)(0xC0 | (codepoint >> 6));
        out[1] = (u8)(0x80 | (codepoint & 0x3F));
        return 2;
    }
    if (codepoint < 0x10000) {
        out[0] = (u8)(0xE0 | (codepoint >> 12));
        out[1] = (u8)(0x80 | ((codepoint >> 6) & 0x3F));
        out[2] = (u8)(0x80 | (codepoint & 0x3F));
        return 3;
    }
    out[0] = (u8)(0xF0 | (codepoint >> 18));
    out[1] = (u8)(0x80 | ((codepoint >> 12) & 0x3F));
    out[2] = (u8)(0x80 | ((codepoint >> 6) & 0x3F));
    out[3] = (u8)(0x80 | (codepoint & 0x3F));
    return 4;
}

static size_t utf8_cstr_len_(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

size_t utf8_strlen(const char *s)
{
    size_t byte_len = utf8_cstr_len_(s);
    size_t i = 0;
    size_t count = 0;

    while (i < byte_len) {
        u32 cp;
        i32 n = utf8_decode((const u8 *)s + i, byte_len - i, &cp);
        i += n > 0 ? (size_t)n : 1;
        count++;
    }
    return count;
}

i32 utf8_validate(const void *data, size_t len)
{
    const u8 *bytes = (const u8 *)data;
    size_t i = 0;

    while (i < len) {
        u32 cp;
        i32 n = utf8_decode(bytes + i, len - i, &cp);
        if (n == 0) return 0;
        i += (size_t)n;
    }
    return 1;
}

#endif /* UTF8_IMPLEMENTATION_ */
#endif /* UTF8_IMPLEMENTATION */
