/* See LICENSE file for copyright and license details. */
/* Real Wayland port (2026-08-24): drw was "X FreeType" (Xft) - real
 * fontconfig pattern matching + real FreeType rasterization + XRender
 * compositing onto an X Drawable. The X-specific piece was only the
 * compositing target; fontconfig/FreeType are already fully backend-
 * agnostic. This keeps both real libraries and replaces the X
 * Drawable/GC with a plain in-memory ARGB8888 pixel buffer that the
 * caller (dmenu.c) backs with a real wl_shm buffer - same real font
 * config strings in config.h still work unchanged. */

#include <stdint.h>

#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H

typedef struct Fnt {
	unsigned int h;      /* real line height, in pixels (already multiplied by scale) */
	int scale;           /* integer upscale of a bitmap (.bdf) font whose strike is smaller than asked for, else 1 */
	FT_Face face;
	FcPattern *pattern;
	struct Fnt *next;
} Fnt;

enum { ColFg, ColBg }; /* Clr scheme index */
typedef uint32_t Clr;  /* 0xAARRGGBB, real premultiplied-alpha-free ARGB8888 */

typedef struct {
	unsigned int w, h;
	uint32_t *pixels;  /* real ARGB8888 framebuffer, w*h pixels, row-major */
	Clr *scheme;
	Fnt *fonts;
} Drw;

/* Drawable abstraction - no Display/Window/screen anymore: a Drw is
 * just a real software framebuffer now. */
Drw *drw_create(unsigned int w, unsigned int h);
void drw_resize(Drw *drw, unsigned int w, unsigned int h);
void drw_free(Drw *drw);

/* Fnt abstraction */
Fnt *drw_fontset_create(Drw *drw, const char *fonts[], size_t fontcount);
void drw_fontset_free(Fnt *set);
unsigned int drw_fontset_getwidth(Drw *drw, const char *text);
unsigned int drw_fontset_getwidth_clamp(Drw *drw, const char *text, unsigned int n);
void drw_font_getexts(Fnt *font, const char *text, unsigned int len, unsigned int *w, unsigned int *h);

/* Colorscheme abstraction - real "#rrggbb" hex parsing (fontconfig/
 * FreeType have no color concept at all, X11-or-not, so this was
 * never really X-specific to begin with; kept name-compatible). */
void drw_clr_create(Drw *drw, Clr *dest, const char *clrname);
Clr *drw_scm_create(Drw *drw, const char *clrnames[], size_t clrcount);

/* Drawing context manipulation */
void drw_setfontset(Drw *drw, Fnt *set);
void drw_setscheme(Drw *drw, Clr *scm);

/* Drawing functions - same real signatures/semantics as before, now
 * writing directly into drw->pixels instead of an X Drawable. */
void drw_rect(Drw *drw, int x, int y, unsigned int w, unsigned int h, int filled, int invert);
int drw_text(Drw *drw, int x, int y, unsigned int w, unsigned int h, unsigned int lpad, const char *text, int invert);
