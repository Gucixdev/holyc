/* color.h - colour maths for colorado: hex, HSL, CIE Lab, WCAG relative luminance and contrast ratio.
 * stb-style single header, header-only (all functions static; no libc beyond <math.h>/<stdio.h>/<string.h>).
 *
 *   co_hex_parse("#1e1e2e", &c)   -> 0 ok / -1 bad    (accepts #rgb, #rrggbb, rrggbb)
 *   co_contrast(a, b)             WCAG 2.x ratio 1..21
 *   co_ensure(c, bg, min)         nudge c's lightness away from bg until contrast >= min (best effort)
 */
#ifndef COLORADO_COLOR_H
#define COLORADO_COLOR_H
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

typedef struct { unsigned char r, g, b; } co_rgb;

static inline double co_clamp(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline co_rgb co_rgb_of(int r, int g, int b) { co_rgb c = { (unsigned char)r, (unsigned char)g, (unsigned char)b }; return c; }
static inline int co_eq(co_rgb a, co_rgb b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

static int co_hex_parse(const char *s, co_rgb *o) {
	unsigned v[6]; int n = 0;
	if (*s == '#') s++;
	for (; *s && n < 7; s++, n++) { if (!isxdigit((unsigned char)*s)) return -1; v[n < 6 ? n : 5] = (unsigned)(isdigit((unsigned char)*s) ? *s - '0' : (tolower((unsigned char)*s) - 'a' + 10)); }
	if (n == 3) { *o = co_rgb_of(v[0] * 17, v[1] * 17, v[2] * 17); return 0; }
	if (n == 6) { *o = co_rgb_of(v[0] * 16 + v[1], v[2] * 16 + v[3], v[4] * 16 + v[5]); return 0; }
	return -1;
}
static void co_hex(co_rgb c, char out[8]) { snprintf(out, 8, "#%02x%02x%02x", c.r, c.g, c.b); }
/* hex digits without '#', for formats that want rrggbb */
__attribute__((unused)) static void co_hex6(co_rgb c, char out[7]) { snprintf(out, 7, "%02x%02x%02x", c.r, c.g, c.b); }

/* ---- WCAG ---- */
static double co_lin(double v) { v /= 255.0; return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4); }
static double co_lum(co_rgb c) { return 0.2126 * co_lin(c.r) + 0.7152 * co_lin(c.g) + 0.0722 * co_lin(c.b); }
static double co_contrast(co_rgb a, co_rgb b) {
	double la = co_lum(a), lb = co_lum(b);
	if (la < lb) { double t = la; la = lb; lb = t; }
	return (la + 0.05) / (lb + 0.05);
}

/* ---- HSL (h 0..360, s,l 0..1) ---- */
static void co_to_hsl(co_rgb c, double *h, double *s, double *l) {
	double r = c.r / 255.0, g = c.g / 255.0, b = c.b / 255.0;
	double mx = fmax(r, fmax(g, b)), mn = fmin(r, fmin(g, b)), d = mx - mn;
	*l = (mx + mn) / 2; *h = 0; *s = 0;
	if (d > 1e-9) {
		*s = d / (1 - fabs(2 * *l - 1));
		if (mx == r) *h = fmod((g - b) / d, 6); else if (mx == g) *h = (b - r) / d + 2; else *h = (r - g) / d + 4;
		*h *= 60; if (*h < 0) *h += 360;
	}
}
static co_rgb co_from_hsl(double h, double s, double l) {
	h = fmod(h, 360); if (h < 0) h += 360; s = co_clamp(s, 0, 1); l = co_clamp(l, 0, 1);
	double c = (1 - fabs(2 * l - 1)) * s, x = c * (1 - fabs(fmod(h / 60, 2) - 1)), m = l - c / 2, r = 0, g = 0, b = 0;
	if (h < 60) { r = c; g = x; } else if (h < 120) { r = x; g = c; } else if (h < 180) { g = c; b = x; }
	else if (h < 240) { g = x; b = c; } else if (h < 300) { r = x; b = c; } else { r = c; b = x; }
	return co_rgb_of((int)lround((r + m) * 255), (int)lround((g + m) * 255), (int)lround((b + m) * 255));
}

/* ---- CIE Lab (D65) ---- */
static void co_to_lab(co_rgb c, double lab[3]) {
	double r = co_lin(c.r), g = co_lin(c.g), b = co_lin(c.b);
	double x = (0.4124564 * r + 0.3575761 * g + 0.1804375 * b) / 0.95047, y = 0.2126729 * r + 0.7151522 * g + 0.0721750 * b, z = (0.0193339 * r + 0.1191920 * g + 0.9503041 * b) / 1.08883;
#define CO_F(t) ((t) > 0.008856 ? cbrt(t) : 7.787 * (t) + 16.0 / 116)
	double fx = CO_F(x), fy = CO_F(y), fz = CO_F(z);
#undef CO_F
	lab[0] = 116 * fy - 16; lab[1] = 500 * (fx - fy); lab[2] = 200 * (fy - fz);
}
static co_rgb co_from_lab(const double lab[3]) {
	double fy = (lab[0] + 16) / 116, fx = fy + lab[1] / 500, fz = fy - lab[2] / 200;
#define CO_FI(t) ((t) * (t) * (t) > 0.008856 ? (t) * (t) * (t) : ((t) - 16.0 / 116) / 7.787)
	double x = CO_FI(fx) * 0.95047, y = CO_FI(fy), z = CO_FI(fz) * 1.08883;
#undef CO_FI
	double lr = 3.2404542 * x - 1.5371385 * y - 0.4985314 * z, lg = -0.9692660 * x + 1.8760108 * y + 0.0415560 * z, lb = 0.0556434 * x - 0.2040259 * y + 1.0572252 * z;
#define CO_G(v) (255 * co_clamp((v) <= 0.0031308 ? 12.92 * (v) : 1.055 * pow(v, 1 / 2.4) - 0.055, 0, 1))
	return co_rgb_of((int)lround(CO_G(lr)), (int)lround(CO_G(lg)), (int)lround(CO_G(lb)));
#undef CO_G
}
static double co_lab_dist2(const double a[3], const double b[3]) {
	double d0 = a[0] - b[0], d1 = a[1] - b[1], d2 = a[2] - b[2];
	return d0 * d0 + d1 * d1 + d2 * d2;
}

static co_rgb co_mix(co_rgb a, co_rgb b, double t) {
	return co_rgb_of((int)lround(a.r + (b.r - a.r) * t), (int)lround(a.g + (b.g - a.g) * t), (int)lround(a.b + (b.b - a.b) * t));
}
/* move c's HSL lightness away from bg (up on dark bg, down on light bg) until contrast >= min; stops at white/black */
static co_rgb co_ensure(co_rgb c, co_rgb bg, double min) {
	double h, s, l; co_to_hsl(c, &h, &s, &l);
	int up = co_lum(bg) < 0.4;
	for (int i = 0; i < 100 && co_contrast(c, bg) < min; i++) {
		l += up ? 0.01 : -0.01;
		if (l > 1) l = 1;
		if (l < 0) l = 0;
		c = co_from_hsl(h, s, l);
		if (l >= 1 || l <= 0) break;
	}
	if (co_contrast(c, bg) < min) { co_rgb e = up ? co_rgb_of(255, 255, 255) : co_rgb_of(0, 0, 0); if (co_contrast(e, bg) > co_contrast(c, bg)) c = e; }
	return c;
}
#endif
