/* mapstyle.h - the shading shared by worldmap.c and globe.c: climate/political/heat colour of a point, borders, the choropleth ramp.
 * A private header (not part of the public API), included by both, each getting its own static copy - the tiny lazy tables (LAT_DESERT)
 * this duplicates are cheap and pure, so that costs nothing but a few KB of .rodata. */
#ifndef LT_MAPSTYLE_H
#define LT_MAPSTYLE_H
#include "lazytui.h"
#include "world.h"
#include <math.h>
#include <string.h>

#define PI 3.14159265358979323846

static double clamp1(double v) { if (!(v > 0)) return 0; return v > 1 ? 1 : v; }         /* NaN -> 0 */
static double fin(double v) { return isfinite(v) ? v : 0; }
static lt_map_opts clean(const lt_map_opts *o)
{
	lt_map_opts c = *o;
	c.sun_lon = fin(c.sun_lon); c.lon0 = fin(c.lon0); c.tilt = fin(c.tilt); c.sun_deg = fin(c.sun_deg); c.heat_lo = fin(c.heat_lo); c.heat_hi = fin(c.heat_hi);
	return c;
}
static double hash2(double a, double b) { double h = sin(a * 12.9898 + b * 78.233) * 43758.5453; return h - floor(h); }
/* the shader runs 100k+ times a frame: cheap integer noise for the land texture, and a table for the latitude-only terms */
static double noise2(int a, int b) { unsigned h = (unsigned)a * 374761393u + (unsigned)b * 668265263u; h = (h ^ (h >> 13)) * 1274126177u; return (double)((h ^ (h >> 16)) & 0xFFFF) / 65536.0; }
static double LAT_DESERT[361];                                     /* by |lat| in quarter degrees */
static bool lat_ready;
static void lat_init(void) { if (lat_ready) return; lat_ready = true; for (int i = 0; i <= 360; i++) { double al = i / 4.0, z = (al - 24) / 9; LAT_DESERT[i] = exp(-z * z) * 0.75; } }

typedef struct {
	const lt_map_opts *o;
	int sel;
	double val[WNCOUNTRIES]; bool has[WNCOUNTRIES];
	lt_color ramp_default[4];
	bool heat;
} style;

static void style_init(style *s, const lt_map_opts *o)
{
	world_init();
	s->o = o;
	s->sel = o->selected ? world_find_iso(o->selected) : -1;
	s->heat = o->heat && o->nheat > 0;
	memset(s->has, 0, sizeof s->has);
	for (int i = 0; s->heat && i < o->nheat; i++) { int id = world_find_iso(o->heat[i].iso); if (id >= 0) { s->val[id] = o->heat[i].value; s->has[id] = true; } }
	s->ramp_default[0] = LT_RGB(38, 62, 146); s->ramp_default[1] = LT_RGB(46, 172, 160); s->ramp_default[2] = LT_RGB(242, 220, 62); s->ramp_default[3] = LT_RGB(228, 58, 40);
}

static void ramp_rgb(const style *s, double f, double rgb[3])
{
	const lt_color *rp = s->o->ramp && s->o->nramp > 1 ? s->o->ramp : s->ramp_default;
	int n = s->o->ramp && s->o->nramp > 1 ? s->o->nramp : 4;
	f = clamp1(f) * (n - 1);
	int i = (int)floor(f);
	if (i >= n - 1) i = n - 2;
	double t = f - i;
	int a[3], b[3];
	lt_color_rgb(rp[i], &a[0], &a[1], &a[2]); lt_color_rgb(rp[i + 1], &b[0], &b[1], &b[2]);
	for (int k = 0; k < 3; k++) rgb[k] = a[k] + (b[k] - a[k]) * t;
}

static void hsv(double h, double sat, double v, double rgb[3])
{
	h = h - floor(h);
	double c = v * sat, x = c * (1 - fabs(fmod(h * 6, 2) - 1)), m = v - c, r, g, b;
	int i = (int)(h * 6);
	switch (i % 6) { case 0: r = c; g = x; b = 0; break; case 1: r = x; g = c; b = 0; break; case 2: r = 0; g = c; b = x; break; case 3: r = 0; g = x; b = c; break; case 4: r = x; g = 0; b = c; break; default: r = c; g = 0; b = x; }
	rgb[0] = (r + m) * 255; rgb[1] = (g + m) * 255; rgb[2] = (b + m) * 255;
}

/* the colour of the surface at a point: sea, or the land of country `id` */
static void surface0(const style *s, int id, double lon, double lat, double land, double rgb[3])
{
	double alat = fabs(lat);
	if (id < 0 || land <= 0.5) {
		rgb[0] = 12; rgb[1] = 62; rgb[2] = 150;
		if (alat > 70) { double ice = clamp1((alat - 70) / 6); for (int k = 0; k < 3; k++) rgb[k] = rgb[k] * (1 - ice) + 232 * ice; }
		return;
	}
	if (s->heat) {
		if (s->has[id]) { double f = s->o->heat_hi > s->o->heat_lo ? (s->val[id] - s->o->heat_lo) / (s->o->heat_hi - s->o->heat_lo) : 0; ramp_rgb(s, f, rgb); }
		else { rgb[0] = 74; rgb[1] = 80; rgb[2] = 78; }                                       /* no data */
	} else if (s->o->flags & LT_MAP_POLITICAL) {
		hsv(hash2(id, 3.1), 0.42 + 0.25 * hash2(id, 9.7), 0.62 + 0.3 * hash2(id, 1.3), rgb);
	} else {
		lat_init();
		double desert = LAT_DESERT[(int)(alat * 4 + 0.5)], ice = clamp1((alat - 62) / 12);
		double n = noise2((int)floor(lon * 1.7), (int)floor(lat * 1.7)) * 0.16;
		double gr[3] = {40 + 25 * n, 112 + 40 * n, 52 + 20 * n}, tan[3] = {176, 150, 92};
		for (int k = 0; k < 3; k++) { rgb[k] = gr[k] * (1 - desert) + tan[k] * desert; rgb[k] = rgb[k] * (1 - ice) + 238 * ice; }
	}
	if (id == s->sel) for (int k = 0; k < 3; k++) rgb[k] = rgb[k] * 0.62 + 255 * 0.38;
}

static void surface(const style *s, int id, double lon, double lat, double land, double rgb[3])
{
	surface0(s, id, lon, lat, land, rgb);
	if (s->o->flags & LT_MAP_GREY) {                                 /* greymap: the same shading, no hue */
		double y = 0.299 * rgb[0] + 0.587 * rgb[1] + 0.114 * rgb[2];
		if (id < 0 || land <= 0.5) y = y * 0.5;
		else y = 70 + y * 0.75;
		for (int k = 0; k < 3; k++) rgb[k] = y;
	}
}

/* a thin border: this point and one a pixel away belong to different countries */
static bool on_border(int id, double lon, double lat, double dx, double dy)
{
	if (id < 0) return false;
	int a = world_id(lon + dx, lat), b = world_id(lon, lat + dy), c = world_id(lon - dx, lat), d = world_id(lon, lat - dy);
	return (a >= 0 && a != id) || (b >= 0 && b != id) || (c >= 0 && c != id) || (d >= 0 && d != id);
}

static void apply_border(const style *s, int id, bool border, double rgb[3])
{
	if (!border) return;
	bool sel = id == s->sel;
	for (int k = 0; k < 3; k++) rgb[k] = sel ? 255 : rgb[k] * 0.42 + 8;
}

static void dim_of(lt_color line, lt_color *dim)
{
	int rr, gg, bb;
	*dim = line;
	if (lt_color_rgb(line, &rr, &gg, &bb)) *dim = LT_RGB(rr * 45 / 100, gg * 45 / 100, bb * 45 / 100);
}
#endif
