/* worldmap.c - the flat equirectangular map: climate/political/heat/grey colouring, borders, night terminator, the 30-degree graticule,
 * wireframe, zoom/pan geometry (lt_worldmap_project_ex / lt_worldmap_lonlat_at, used by lt_mapview.c), country picking, and a frame cache
 * (lt_snap) so an unchanged map redraws from a cell snapshot instead of re-shading. The globe lives in globe.c; both share mapstyle.h. */
#include "lazytui.h"
#include "mapstyle.h"
#include "mapinternal.h"
#include "world.h"
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ flat map geometry
 * The view: zoom 1 fits the whole 2:1 map into r; higher zooms magnify around the centre (clon, clat), which sits in the middle of r.
 * At zoom 1 the vertical centre is forced to the equator and the horizontal one to 0, so the whole map is centred. */
typedef struct { double x0, y0, mw, mh; const style *st; int flags; double sun_lon; } mp;

static void map_geom(lt_rect r, const lt_map_opts *o, mp *m)
{
	double PW = r.w, PH = 2.0 * r.h;
	double zoom = o && o->zoom >= 1 ? fmin(o->zoom, 512) : 1;
	double clon = o ? fmax(-180, fmin(180, fin(o->clon))) : 0, clat = o ? fmax(-90, fmin(90, fin(o->clat))) : 0;
	m->mw = fmin(PW, 2 * PH) * zoom; m->mh = m->mw / 2;
	m->x0 = PW / 2 - (clon + 180.0) / 360.0 * m->mw;
	m->y0 = PH / 2 - (90.0 - clat) / 180.0 * m->mh;
}

static double map_shader(double px, double py, double rgb[3], void *ctx)
{
	const mp *m = ctx;
	if (px < m->x0 || px >= m->x0 + m->mw || py < m->y0 || py >= m->y0 + m->mh) return 0;
	double lon = (px - m->x0) / m->mw * 360 - 180, lat = 90 - (py - m->y0) / m->mh * 180;
	double land = world_land(lon, lat);
	int id = land > 0.5 ? world_id(lon, lat) : -1;
	double base[3];
	surface(m->st, id, lon, lat, land, base);
	if (m->flags & LT_MAP_BORDERS) { double dpp = 360.0 / m->mw * 0.3; apply_border(m->st, id, on_border(id, lon, lat, dpp, dpp), base); }
	double lit = 1.0;
	if (m->flags & LT_MAP_NIGHT) {                            /* the sun stands over the equator at sun_lon: a soft terminator */
		double z = cos(lat * PI / 180) * cos((lon - m->sun_lon) * PI / 180);
		lit = 0.16 + 0.84 * clamp1(z * 4.0 + 0.5);
	}
	if (m->flags & LT_MAP_GRID) {
		double gl_lon = fabs(remainder(lon, 30.0)), gl_lat = fabs(remainder(lat, 30.0));
		if (gl_lon < 0.45 || gl_lat < 0.45) for (int k = 0; k < 3; k++) base[k] = base[k] * 0.65 + 220 * 0.35;
	}
	for (int k = 0; k < 3; k++) rgb[k] = base[k] * lit;
	return 1.0;
}

bool lt_worldmap_project_ex(lt_rect r, const lt_map_opts *o, double lon, double lat, int *col, int *row)
{
	mp m;
	map_geom(r, o, &m);
	double x = m.x0 + (lon + 180.0) / 360.0 * m.mw, y = m.y0 + (90.0 - lat) / 180.0 * m.mh;
	if (col) *col = r.x + (int)floor(x);
	if (row) *row = r.y + (int)floor(y / 2.0);
	return lon >= -180 && lon <= 180 && lat >= -90 && lat <= 90 && *col >= r.x && *col < r.x + r.w && *row >= r.y && *row < r.y + r.h;
}
bool lt_worldmap_project(lt_rect r, double lon, double lat, int *col, int *row)
{
	int c, w;
	bool in = lt_worldmap_project_ex(r, NULL, lon, lat, &c, &w);
	if (col) *col = c;
	if (row) *row = w;
	return in;
}

/* the longitude / latitude under a cell of the (zoomed) flat map; false outside the map */
bool lt_worldmap_lonlat_at(lt_rect r, const lt_map_opts *o, int col, int row, double *lon, double *lat)
{
	mp m;
	map_geom(r, o, &m);
	double px = col - r.x + 0.5, py = 2.0 * (row - r.y) + 1.0;
	double lo = (px - m.x0) / m.mw * 360 - 180, la = 90 - (py - m.y0) / m.mh * 180;
	if (lon) *lon = lo;
	if (lat) *lat = la;
	return lo >= -180 && lo <= 180 && la >= -90 && la <= 90;
}

int lt_worldmap_country_at_ex(lt_rect r, const lt_map_opts *o, int col, int row)
{
	world_init();
	mp m;
	map_geom(r, o, &m);
	double px = col - r.x + 0.5, py = 2.0 * (row - r.y) + 1.0;
	if (px < m.x0 || px >= m.x0 + m.mw || py < m.y0 || py >= m.y0 + m.mh) return -1;
	double lon = (px - m.x0) / m.mw * 360 - 180, lat = 90 - (py - m.y0) / m.mh * 180;
	return world_land(lon, lat) > 0.5 ? world_id(lon, lat) : -1;
}
int lt_worldmap_country_at(lt_rect r, int col, int row) { return lt_worldmap_country_at_ex(r, NULL, col, row); }

/* ------------------------------------------------------------------ wireframe */
static void wire_edges_map(lt_canvas *c, lt_rect r, const mp *m, const style *st, lt_color line)
{
	#define MX(lon) (r.x + (m->x0 + ((lon) + 180.0) / 360.0 * m->mw))
	#define MY(lat) (r.y + (m->y0 + (90.0 - (lat)) / 180.0 * m->mh) / 2.0)
	for (int pass = 0; pass < 2; pass++)                                      /* the selected country last, in white */
		for (int ci = 0; ci < WNCOUNTRIES; ci++) {
			bool sel = ci == st->sel;
			if ((pass == 1) != sel) continue;
			const wcountry *w = &WCOUNTRIES[ci];
			for (int rr = w->ring0; rr < w->ring0 + w->nrings; rr++) {
				const wring *R = &WRINGS[rr];
				for (int i = 0; i < R->count; i++) {
					int j = (i + 1) % R->count;
					double x0 = WVERTS[2 * (R->start + i)] / 100.0, y0 = WVERTS[2 * (R->start + i) + 1] / 100.0, x1 = WVERTS[2 * (R->start + j)] / 100.0, y1 = WVERTS[2 * (R->start + j) + 1] / 100.0;
					if (fabs(x0 - x1) > 100 || (y0 < -85 && y1 < -85)) continue;
					lt_wire_line(c, MX(x0), MY(y0), MX(x1), MY(y1), sel ? LT_RGB(255, 255, 255) : line);
				}
			}
		}
}

typedef struct { lt_rect r; mp m; } mapctx;
static bool map_to_cell(void *ctx, double lon, double lat, int *col, int *row)
{
	const mapctx *q = ctx;
	double x = q->m.x0 + (lon + 180.0) / 360.0 * q->m.mw, y = q->m.y0 + (90.0 - lat) / 180.0 * q->m.mh;
	*col = q->r.x + (int)floor(x); *row = q->r.y + (int)floor(y / 2.0);
	return true;
}

/* ------------------------------------------------------------------ frame cache
 * A map that has not changed (same size, view, flags, data, ground) is redrawn from a snapshot of the cells: 0.05 ms instead of 10-30 ms. */
#define CACHE_N 6
static struct { unsigned long long key; lt_snap *snap; int w, h; } cache[CACHE_N];
static int cache_next;
static unsigned long long fnv(unsigned long long h, const void *p, size_t n) { const unsigned char *b = p; while (n--) h = (h ^ *b++) * 1099511628211ull; return h; }
static unsigned long long map_key(lt_canvas *c, lt_rect r, const lt_map_opts *o)
{
	unsigned long long h = 1469598103934665603ull;
	const lt_caps *caps = lt_canvas_caps(c);
	lt_color bg = lt_canvas_bg(c, r.x, r.y);
	bool pl = map_lang_polish();
	double d[] = {o->zoom, o->clon, o->clat, o->sun_lon, o->heat_lo, o->heat_hi};
	h = fnv(h, &r.w, sizeof r.w); h = fnv(h, &r.h, sizeof r.h); h = fnv(h, &o->flags, sizeof o->flags); h = fnv(h, d, sizeof d);
	h = fnv(h, &o->line, sizeof o->line); h = fnv(h, &bg, sizeof bg); h = fnv(h, caps, sizeof *caps); h = fnv(h, &pl, sizeof pl);
	if (o->selected) h = fnv(h, o->selected, strlen(o->selected));
	for (int i = 0; o->heat && i < o->nheat; i++) { h = fnv(h, o->heat[i].iso, strlen(o->heat[i].iso)); h = fnv(h, &o->heat[i].value, sizeof(double)); }
	for (int i = 0; o->ramp && i < o->nramp; i++) h = fnv(h, &o->ramp[i], sizeof(lt_color));
	return h;
}

static void worldmap_draw(lt_canvas *c, lt_rect r, const lt_map_opts *o_in);
void lt_worldmap_ex(lt_canvas *c, lt_rect r, const lt_map_opts *o_in)
{
	if (r.w < 8 || r.h < 3 || !o_in) return;
	map_ensure_lang();
	lt_map_opts oc = clean(o_in);
	unsigned long long key = map_key(c, r, &oc);
	for (int i = 0; i < CACHE_N; i++)
		if (cache[i].snap && cache[i].key == key && cache[i].w == r.w && cache[i].h == r.h) { lt_snap_blit(cache[i].snap, c, r.x, r.y); return; }
	worldmap_draw(c, r, o_in);
	int slot = cache_next++ % CACHE_N;
	lt_snap_free(cache[slot].snap);
	cache[slot].snap = lt_snap_take(c, r);
	cache[slot].key = key; cache[slot].w = r.w; cache[slot].h = r.h;
}

static void worldmap_draw(lt_canvas *c, lt_rect r, const lt_map_opts *o_in)
{
	lt_map_opts oc = clean(o_in);
	const lt_map_opts *o = &oc;
	style st;
	style_init(&st, o);
	mp m;
	map_geom(r, o, &m);
	m.st = &st; m.flags = o->flags; m.sun_lon = isfinite(o->sun_lon) ? o->sun_lon : 0;
	lt_color line = o->line != LT_DEFAULT ? o->line : LT_RGB(0, 255, 65), dim;
	dim_of(line, &dim);
	lt_canvas_set_clip(c, r);                                   /* zoomed geometry reaches far outside r: never draw there */
	if (!(o->flags & LT_MAP_WIRE)) lt_shade(c, r, map_shader, &m);
	else {
		#define MXW(lon) (r.x + (m.x0 + ((lon) + 180.0) / 360.0 * m.mw))
		#define MYW(lat) (r.y + (m.y0 + (90.0 - (lat)) / 180.0 * m.mh) / 2.0)
		if (o->flags & LT_MAP_GRID) {
			for (int lon = -180; lon <= 180; lon += 30) lt_wire_line(c, MXW(lon), MYW(90), MXW(lon), MYW(-90), dim);
			for (int lat = -90; lat <= 90; lat += 30) lt_wire_line(c, MXW(-180), MYW(lat), MXW(180), MYW(lat), dim);
		}
		wire_edges_map(c, r, &m, &st, line);
	}
	if (o->flags & LT_MAP_LABELS) {
		mapctx q = {r, m};
		lt_rect area = {r.x + (int)m.x0, r.y + (int)(m.y0 / 2), (int)m.mw, (int)(m.mh / 2)};
		map_draw_labels(c, area, o->flags, m.mw / 14.0, line, map_to_cell, &q);
	}
	lt_canvas_set_clip(c, (lt_rect){0, 0, lt_canvas_w(c), lt_canvas_h(c)});
}

void lt_worldmap(lt_canvas *c, lt_rect r, int flags, double sun_lon_deg, lt_color line)
{
	lt_map_opts o = {0};
	o.flags = flags | ((flags & LT_MAP_WIRE) ? 0 : LT_MAP_BORDERS); o.sun_lon = sun_lon_deg; o.line = line;
	lt_worldmap_ex(c, r, &o);
}
