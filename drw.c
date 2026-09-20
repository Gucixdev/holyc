/* See LICENSE file for copyright and license details. */
/* Real Wayland port (2026-08-24) - see drw.h's own top comment. Same
 * real per-codepoint font-fallback logic as the original (fontconfig
 * charset matching when the current font is missing a glyph), same
 * real ellipsis-truncation/invalid-char handling in drw_text - only
 * the actual rendering backend changed (FreeType glyph bitmaps
 * alpha-blended into drw->pixels, not XftDraw onto an X Drawable). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "drw.h"
#include FT_GLYPH_H
#include "util.h"

#define UTF_INVALID 0xFFFD

static FT_Library ftlib;
static int ftlib_refs;

static int
utf8decode(const char *s_in, long *u, int *err)
{
	static const unsigned char lens[] = {
		/* 0XXXX */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
		/* 10XXX */ 0, 0, 0, 0, 0, 0, 0, 0,  /* invalid */
		/* 110XX */ 2, 2, 2, 2,
		/* 1110X */ 3, 3,
		/* 11110 */ 4,
		/* 11111 */ 0,  /* invalid */
	};
	static const unsigned char leading_mask[] = { 0x7F, 0x1F, 0x0F, 0x07 };
	static const unsigned int overlong[] = { 0x0, 0x80, 0x0800, 0x10000 };

	const unsigned char *s = (const unsigned char *)s_in;
	int len = lens[*s >> 3];
	*u = UTF_INVALID;
	*err = 1;
	if (len == 0)
		return 1;

	long cp = s[0] & leading_mask[len - 1];
	for (int i = 1; i < len; ++i) {
		if (s[i] == '\0' || (s[i] & 0xC0) != 0x80)
			return i;
		cp = (cp << 6) | (s[i] & 0x3F);
	}
	if (cp > 0x10FFFF || (cp >> 11) == 0x1B || cp < overlong[len - 1])
		return len;

	*err = 0;
	*u = cp;
	return len;
}

Drw *
drw_create(unsigned int w, unsigned int h)
{
	Drw *drw = ecalloc(1, sizeof(Drw));

	if (!ftlib_refs && FT_Init_FreeType(&ftlib))
		die("drw: FT_Init_FreeType failed");
	ftlib_refs++;

	drw->w = w;
	drw->h = h;
	drw->pixels = ecalloc((size_t)w * h, sizeof(uint32_t));

	return drw;
}

void
drw_resize(Drw *drw, unsigned int w, unsigned int h)
{
	if (!drw)
		return;

	drw->w = w;
	drw->h = h;
	free(drw->pixels);
	drw->pixels = ecalloc((size_t)w * h, sizeof(uint32_t));
}

void
drw_free(Drw *drw)
{
	free(drw->pixels);
	drw_fontset_free(drw->fonts);
	free(drw);
	if (!--ftlib_refs)
		FT_Done_FreeType(ftlib);
}

/* This function is an implementation detail. Library users should use
 * drw_fontset_create instead.
 */
static Fnt *
xfont_create(Drw *drw, const char *fontname, FcPattern *fontpattern)
{
	Fnt *font;
	FT_Face face = NULL;
	FcPattern *pattern = NULL;
	FcPattern *match = NULL;
	FcResult result;
	FcChar8 *file;
	double size = 12;
	int index = 0;

	(void)drw;
	if (fontname) {
		if (!(pattern = FcNameParse((FcChar8 *)fontname))) {
			fprintf(stderr, "error, cannot parse font name to pattern: '%s'\n", fontname);
			return NULL;
		}
		FcConfigSubstitute(NULL, pattern, FcMatchPattern);
		FcDefaultSubstitute(pattern);
		match = FcFontMatch(NULL, pattern, &result);
		if (!match) {
			fprintf(stderr, "error, cannot match font from name: '%s'\n", fontname);
			FcPatternDestroy(pattern);
			return NULL;
		}
	} else if (fontpattern) {
		match = fontpattern;
	} else {
		die("no font specified.");
	}

	if (FcPatternGetString(match, FC_FILE, 0, &file) != FcResultMatch) {
		fprintf(stderr, "error, matched font has no real file path.\n");
		goto fail;
	}
	FcPatternGetInteger(match, FC_INDEX, 0, &index);
	FcPatternGetDouble(match, FC_PIXEL_SIZE, 0, &size);
	if (size <= 0)
		size = 12;

	if (FT_New_Face(ftlib, (const char *)file, index, &face)) {
		fprintf(stderr, "error, cannot load font file: '%s'\n", file);
		goto fail;
	}
	FT_Set_Pixel_Sizes(face, 0, (FT_UInt)size);

	font = ecalloc(1, sizeof(Fnt));
	font->face = face;
	/* real ascent+descent, in pixels (26.6 fixed point -> integer) */
	font->h = (unsigned int)((face->size->metrics.ascender - face->size->metrics.descender) >> 6);
	font->pattern = (match == fontpattern) ? NULL : match;
	if (pattern && pattern != match)
		FcPatternDestroy(pattern);
	return font;

fail:
	if (match && match != fontpattern)
		FcPatternDestroy(match);
	if (pattern && pattern != match)
		FcPatternDestroy(pattern);
	return NULL;
}

static void
xfont_free(Fnt *font)
{
	if (!font)
		return;
	if (font->pattern)
		FcPatternDestroy(font->pattern);
	if (font->face)
		FT_Done_Face(font->face);
	free(font);
}

Fnt *
drw_fontset_create(Drw *drw, const char *fonts[], size_t fontcount)
{
	Fnt *cur, *ret = NULL;
	size_t i;

	if (!drw || !fonts)
		return NULL;

	for (i = 1; i <= fontcount; i++) {
		if ((cur = xfont_create(drw, fonts[fontcount - i], NULL))) {
			cur->next = ret;
			ret = cur;
		}
	}
	return (drw->fonts = ret);
}

void
drw_fontset_free(Fnt *font)
{
	if (font) {
		drw_fontset_free(font->next);
		xfont_free(font);
	}
}

/* Real "#rrggbb"/"#rgb" hex parsing - fontconfig/FreeType have no
 * concept of a colormap-allocated color at all, X11 or not, so this
 * was never really an X11-specific step; a plain ARGB8888 value is
 * exactly what direct pixel blitting needs. */
void
drw_clr_create(Drw *drw, Clr *dest, const char *clrname)
{
	unsigned int r = 0, g = 0, b = 0;

	if (!drw || !dest || !clrname)
		return;

	if (clrname[0] == '#' && strlen(clrname) == 7) {
		sscanf(clrname + 1, "%02x%02x%02x", &r, &g, &b);
	} else {
		fprintf(stderr, "warning: only #rrggbb colors are real here, got '%s'\n", clrname);
	}
	*dest = 0xff000000u | (r << 16) | (g << 8) | b;
}

Clr *
drw_scm_create(Drw *drw, const char *clrnames[], size_t clrcount)
{
	size_t i;
	Clr *ret;

	if (!drw || !clrnames || clrcount < 2 || !(ret = ecalloc(clrcount, sizeof(Clr))))
		return NULL;

	for (i = 0; i < clrcount; i++)
		drw_clr_create(drw, &ret[i], clrnames[i]);
	return ret;
}

void
drw_setfontset(Drw *drw, Fnt *set)
{
	if (drw)
		drw->fonts = set;
}

void
drw_setscheme(Drw *drw, Clr *scm)
{
	if (drw)
		drw->scheme = scm;
}

static void
putpixel(Drw *drw, int x, int y, Clr c)
{
	if (x < 0 || y < 0 || (unsigned)x >= drw->w || (unsigned)y >= drw->h)
		return;
	drw->pixels[y * (int)drw->w + x] = c;
}

/* Real alpha-blend of `fg` over whatever's already at (x,y), using
 * `a` (0-255, a FreeType glyph bitmap's own coverage byte) - the
 * direct-pixel-buffer replacement for XRender compositing a glyph. */
static void
blendpixel(Drw *drw, int x, int y, Clr fg, unsigned int a)
{
	uint32_t bg, or_, og, ob, nr, ng, nb;

	if (x < 0 || y < 0 || (unsigned)x >= drw->w || (unsigned)y >= drw->h || a == 0)
		return;
	if (a >= 255) {
		putpixel(drw, x, y, fg);
		return;
	}
	bg = drw->pixels[y * (int)drw->w + x];
	or_ = (bg >> 16) & 0xff; og = (bg >> 8) & 0xff; ob = bg & 0xff;
	nr = ((fg >> 16) & 0xff); ng = ((fg >> 8) & 0xff); nb = fg & 0xff;
	nr = (nr * a + or_ * (255 - a)) / 255;
	ng = (ng * a + og * (255 - a)) / 255;
	nb = (nb * a + ob * (255 - a)) / 255;
	drw->pixels[y * (int)drw->w + x] = 0xff000000u | (nr << 16) | (ng << 8) | nb;
}

void
drw_rect(Drw *drw, int x, int y, unsigned int w, unsigned int h, int filled, int invert)
{
	int i, j;
	Clr c;

	if (!drw || !drw->scheme)
		return;
	c = invert ? drw->scheme[ColBg] : drw->scheme[ColFg];
	if (filled) {
		for (j = y; j < y + (int)h; j++)
			for (i = x; i < x + (int)w; i++)
				putpixel(drw, i, j, c);
	} else {
		for (i = x; i < x + (int)w; i++) {
			putpixel(drw, i, y, c);
			putpixel(drw, i, y + (int)h - 1, c);
		}
		for (j = y; j < y + (int)h; j++) {
			putpixel(drw, x, j, c);
			putpixel(drw, x + (int)w - 1, j, c);
		}
	}
}

static void
drawglyph(Drw *drw, FT_Face face, long codepoint, int pen_x, int baseline_y, Clr fg)
{
	FT_UInt gi = FT_Get_Char_Index(face, (FT_ULong)codepoint);
	FT_Bitmap *bm;
	int gx, gy, bx, by;

	if (FT_Load_Glyph(face, gi, FT_LOAD_RENDER))
		return;
	bm = &face->glyph->bitmap;
	gx = pen_x + face->glyph->bitmap_left;
	gy = baseline_y - face->glyph->bitmap_top;
	for (by = 0; by < (int)bm->rows; by++)
		for (bx = 0; bx < (int)bm->width; bx++)
			blendpixel(drw, gx + bx, gy + by, fg, bm->buffer[by * bm->pitch + bx]);
}

static unsigned int
glyphadvance(FT_Face face, long codepoint)
{
	FT_UInt gi = FT_Get_Char_Index(face, (FT_ULong)codepoint);
	if (FT_Load_Glyph(face, gi, FT_LOAD_DEFAULT))
		return 0;
	return (unsigned int)(face->glyph->advance.x >> 6);
}

int
drw_text(Drw *drw, int x, int y, unsigned int w, unsigned int h, unsigned int lpad, const char *text, int invert)
{
	int ty, ellipsis_x = 0;
	unsigned int tmpw, ew, ellipsis_w = 0, ellipsis_len, hash, h0, h1;
	Fnt *usedfont, *curfont, *nextfont;
	int utf8strlen, utf8charlen, utf8err, render = x || y || w || h;
	long utf8codepoint = 0;
	const char *utf8str;
	FcCharSet *fccharset;
	FcPattern *fcpattern;
	FcPattern *match;
	FcResult result;
	int charexists = 0, overflow = 0;
	static unsigned int nomatches[128], ellipsis_width, invalid_width;
	static const char invalid[] = "?";

	if (!drw || (render && (!drw->scheme || !w)) || !text || !drw->fonts)
		return 0;

	if (!render) {
		w = invert ? invert : ~invert;
	} else {
		drw_rect(drw, x, y, w, h, 1, invert ? 0 : 1);
		if (w < lpad)
			return x + w;
		x += lpad;
		w -= lpad;
	}

	usedfont = drw->fonts;
	if (!ellipsis_width && render)
		ellipsis_width = drw_fontset_getwidth(drw, "...");
	if (!invalid_width && render)
		invalid_width = drw_fontset_getwidth(drw, invalid);
	while (1) {
		ew = ellipsis_len = utf8err = utf8charlen = utf8strlen = 0;
		utf8str = text;
		nextfont = NULL;
		while (*text) {
			utf8charlen = utf8decode(text, &utf8codepoint, &utf8err);
			for (curfont = drw->fonts; curfont; curfont = curfont->next) {
				charexists = charexists || FT_Get_Char_Index(curfont->face, (FT_ULong)utf8codepoint) != 0;
				if (charexists) {
					drw_font_getexts(curfont, text, (unsigned)utf8charlen, &tmpw, NULL);
					if (ew + ellipsis_width <= w) {
						ellipsis_x = x + (int)ew;
						ellipsis_w = w - ew;
						ellipsis_len = (unsigned)utf8strlen;
					}

					if (ew + tmpw > w) {
						overflow = 1;
						if (!render)
							x += (int)tmpw;
						else
							utf8strlen = (int)ellipsis_len;
					} else if (curfont == usedfont) {
						text += utf8charlen;
						utf8strlen += utf8err ? 0 : utf8charlen;
						ew += utf8err ? 0 : tmpw;
					} else {
						nextfont = curfont;
					}
					break;
				}
			}

			if (overflow || !charexists || nextfont || utf8err)
				break;
			else
				charexists = 0;
		}

		if (utf8strlen) {
			if (render) {
				const char *p = utf8str;
				int px = x;
				ty = y + (int)(h - usedfont->h) / 2 + (int)(usedfont->face->size->metrics.ascender >> 6);
				while (p < utf8str + utf8strlen) {
					long cp; int err, n = utf8decode(p, &cp, &err);
					drawglyph(drw, usedfont->face, cp, px, ty, drw->scheme[invert ? ColBg : ColFg]);
					px += (int)glyphadvance(usedfont->face, cp);
					p += n;
				}
			}
			x += (int)ew;
			w -= ew;
		}
		if (utf8err && (!render || invalid_width < w)) {
			if (render)
				drw_text(drw, x, y, w, h, 0, invalid, invert);
			x += (int)invalid_width;
			w -= invalid_width;
		}
		if (render && overflow)
			drw_text(drw, ellipsis_x, y, ellipsis_w, h, 0, "...", invert);

		if (!*text || overflow)
			break;
		else if (nextfont) {
			charexists = 0;
			usedfont = nextfont;
		} else {
			charexists = 1;

			hash = (unsigned int)utf8codepoint;
			hash = ((hash >> 16) ^ hash) * 0x21F0AAAD;
			hash = ((hash >> 15) ^ hash) * 0xD35A2D97;
			h0 = ((hash >> 15) ^ hash) % LENGTH(nomatches);
			h1 = (hash >> 17) % LENGTH(nomatches);
			if (nomatches[h0] == (unsigned int)utf8codepoint || nomatches[h1] == (unsigned int)utf8codepoint)
				goto no_match;

			fccharset = FcCharSetCreate();
			FcCharSetAddChar(fccharset, (FcChar32)utf8codepoint);

			if (!drw->fonts->pattern) {
				/* First font wasn't loaded from a real fontconfig
				 * pattern (e.g. it came in as a raw FcPattern
				 * already) - no real charset base to fall back
				 * from, same real constraint the original had. */
				goto no_match;
			}

			fcpattern = FcPatternDuplicate(drw->fonts->pattern);
			FcPatternAddCharSet(fcpattern, FC_CHARSET, fccharset);
			FcPatternAddBool(fcpattern, FC_SCALABLE, FcTrue);

			FcConfigSubstitute(NULL, fcpattern, FcMatchPattern);
			FcDefaultSubstitute(fcpattern);
			match = FcFontMatch(NULL, fcpattern, &result);

			FcCharSetDestroy(fccharset);
			FcPatternDestroy(fcpattern);

			if (match) {
				usedfont = xfont_create(drw, NULL, match);
				if (usedfont && FT_Get_Char_Index(usedfont->face, (FT_ULong)utf8codepoint) != 0) {
					for (curfont = drw->fonts; curfont->next; curfont = curfont->next)
						;
					curfont->next = usedfont;
				} else {
					if (usedfont)
						xfont_free(usedfont);
					else
						FcPatternDestroy(match);
					nomatches[nomatches[h0] ? h1 : h0] = (unsigned int)utf8codepoint;
no_match:
					usedfont = drw->fonts;
				}
			} else {
				nomatches[nomatches[h0] ? h1 : h0] = (unsigned int)utf8codepoint;
				usedfont = drw->fonts;
			}
		}
	}

	return x + (render ? (int)w : 0);
}

unsigned int
drw_fontset_getwidth(Drw *drw, const char *text)
{
	if (!drw || !drw->fonts || !text)
		return 0;
	return (unsigned int)drw_text(drw, 0, 0, 0, 0, 0, text, 0);
}

unsigned int
drw_fontset_getwidth_clamp(Drw *drw, const char *text, unsigned int n)
{
	unsigned int tmp = 0;
	if (drw && drw->fonts && text && n)
		tmp = (unsigned int)drw_text(drw, 0, 0, 0, 0, 0, text, (int)n);
	return MIN(n, tmp);
}

void
drw_font_getexts(Fnt *font, const char *text, unsigned int len, unsigned int *w, unsigned int *h)
{
	unsigned int i = 0, width = 0;
	long cp; int err, n;

	if (!font || !text)
		return;

	while (i < len && text[i]) {
		n = utf8decode(text + i, &cp, &err);
		width += glyphadvance(font->face, cp);
		i += (unsigned)n;
	}
	if (w)
		*w = width;
	if (h)
		*h = font->h;
}
