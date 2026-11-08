/* globe.c - the planet as an orthographic sphere: climate/political/heat colours, sun/night shading, specular ocean highlight, atmosphere
 * glow, thin borders, the 30-degree graticule, a wireframe form, labels, and screen-cell -> lon/lat picking. The flat map lives in
 * worldmap.c; both share their colouring through mapstyle.h and their label placement through maplabels.c (mapinternal.h). */
#include "lazytui.h"
#include "mapstyle.h"
#include "mapinternal.h"
#include "world.h"

typedef struct { double PW, PH, R, cx, cy, lon0, sint, cost, lx, ly, lz; int flags; const style *st; } gl;

static void setup(gl *g, lt_rect r, const lt_map_opts *o)
{
	double PW = r.w, PH = 2.0 * r.h;
	g->PW = PW; g->PH = PH; g->cx = PW / 2; g->cy = PH / 2;
	g->R = fmin(PW, PH) / 2 / 1.07;
	g->lon0 = o->lon0;
	double t = o->tilt * PI / 180;
	g->sint = sin(t); g->cost = cos(t);
	double a = o->sun_deg * PI / 180;                          /* 0 = the sun over the viewer's upper left; positive turns it right and behind */
	double x = sin(a - 0.9), y = 0.32, z = cos(a - 0.9), l = sqrt(x * x + y * y + z * z);
	g->lx = x / l; g->ly = y / l; g->lz = z / l;
	g->flags = o->flags;
}

/* screen (phys) -> lon/lat on the sphere; false outside the disc */
static bool unproject(const gl *g, double px, double py, double *lon, double *lat, double *nz_out)
{
	double nx = (px - g->cx) / g->R, ny = -(py - g->cy) / g->R, d2 = nx * nx + ny * ny;
	if (d2 > 1.0) return false;
	double nz = sqrt(1.0 - d2);
	double a = nx, p = ny * g->cost + nz * g->sint, q = -ny * g->sint + nz * g->cost;
	*lat = asin(p < -1 ? -1 : p > 1 ? 1 : p) * 180 / PI;
	*lon = fmod(atan2(a, q) * 180 / PI + g->lon0 + 540, 360) - 180;
	if (nz_out) *nz_out = nz;
	return true;
}

static double globe_shader(double px, double py, double rgb[3], void *ctx)
{
	const gl *g = ctx;
	double nx = (px - g->cx) / g->R, ny = -(py - g->cy) / g->R, d2 = nx * nx + ny * ny;
	const double atm = 1.07;
	if (d2 > atm * atm) return 0;
	double d = sqrt(d2);
	if (d > 1.0) {                                            /* the glow: brighter on the lit side, fading outward */
		double f = 1.0 - (d - 1.0) / (atm - 1.0), lit = clamp1(0.25 + (nx / d * g->lx + ny / d * g->ly) * 0.9);
		rgb[0] = 70; rgb[1] = 140; rgb[2] = 255;
		return f * f * lit * 0.85;
	}
	double lon, lat, nz;
	unproject(g, px, py, &lon, &lat, &nz);
	double land = world_land(lon, lat);
	int id = land > 0.5 ? world_id(lon, lat) : -1;
	double base[3];
	surface(g->st, id, lon, lat, land, base);
	if (g->flags & LT_MAP_BORDERS) {
		double dpp = 57.3 / (g->R * (nz > 0.25 ? nz : 0.25)) * 0.3;         /* degrees per sample-ish */
		apply_border(g->st, id, on_border(id, lon, lat, dpp / (cos(lat * PI / 180) > 0.2 ? cos(lat * PI / 180) : 0.2), dpp), base);
	}
	if (g->flags & LT_GLOBE_GRID) {
		double gl_lon = fabs(remainder(lon, 30.0)), gl_lat = fabs(remainder(lat, 30.0)), w = 1.2 / (g->R * (nz + 0.25));
		if (gl_lon < w * 5 || gl_lat < w * 5) for (int c = 0; c < 3; c++) base[c] = base[c] * 0.65 + 220 * 0.35;
	}
	double lam = nx * g->lx + ny * g->ly + nz * g->lz;
	double lit = 0.06 + 0.98 * clamp1(lam * 1.15 + 0.05), spec = 0;
	if (id < 0 && lam > 0) {
		double hx = g->lx, hy = g->ly, hz = g->lz + 1, hl = sqrt(hx * hx + hy * hy + hz * hz);
		double nh = (nx * hx + ny * hy + nz * hz) / hl;
		spec = pow(nh > 0 ? nh : 0, 60.0) * 0.4;
	}
	double rim = pow(1.0 - nz, 3.0) * (0.15 + 0.85 * clamp1(lam + 0.3));
	for (int k = 0; k < 3; k++) { double v = base[k] * lit + spec * 255 + rim * (k == 2 ? 200 : k == 1 ? 120 : 60); rgb[k] = v > 255 ? 255 : v; }
	return 1.0;
}

static bool proj(const gl *g, double lon, double lat, double *px, double *py, double *z_out)
{
	double dl = (lon - g->lon0) * PI / 180, la = lat * PI / 180;
	double p = sin(la), a = cos(la) * sin(dl), q = cos(la) * cos(dl);
	double x = a, yup = p * g->cost - q * g->sint, z = p * g->sint + q * g->cost;
	if (z <= 0.0) return false;
	*px = g->cx + x * g->R; *py = g->cy - yup * g->R;
	if (z_out) *z_out = z;
	return true;
}

bool lt_globe_project(lt_rect r, double lon_deg, double tilt_deg, double lon, double lat, int *col, int *row)
{
	lt_map_opts o = {0};
	o.lon0 = lon_deg; o.tilt = tilt_deg;
	gl g;
	setup(&g, r, &o);
	double px, py, z;
	if (!proj(&g, lon, lat, &px, &py, &z) || z <= 0.02) return false;
	if (col) *col = r.x + (int)floor(px);
	if (row) *row = r.y + (int)floor(py / 2.0);
	return true;
}

int lt_globe_country_at(lt_rect r, const lt_map_opts *o_in, int col, int row)
{
	world_init();
	lt_map_opts oc = clean(o_in);
	gl g;
	setup(&g, r, &oc);
	double lon, lat;
	if (!unproject(&g, col - r.x + 0.5, 2.0 * (row - r.y) + 1.0, &lon, &lat, NULL)) return -1;
	return world_land(lon, lat) > 0.5 ? world_id(lon, lat) : -1;
}

typedef struct { lt_rect r; gl g; } globectx;
static bool globe_to_cell(void *ctx, double lon, double lat, int *col, int *row)
{
	const globectx *q = ctx;
	double px, py, z;
	if (!proj(&q->g, lon, lat, &px, &py, &z) || z < 0.5) return false;          /* not near the limb: labels would be squeezed */
	*col = q->r.x + (int)floor(px); *row = q->r.y + (int)floor(py / 2.0);
	return true;
}

static void seg(lt_canvas *c, lt_rect r, const gl *g, double lo0, double la0, double lo1, double la1, lt_color col)
{
	int steps = (int)(fmax(fabs(lo1 - lo0), fabs(la1 - la0)) / 2.0) + 1;
	double px = 0, py = 0;
	bool have = false;
	for (int i = 0; i <= steps; i++) {
		double f = (double)i / steps, x1, y1, z;
		if (proj(g, lo0 + (lo1 - lo0) * f, la0 + (la1 - la0) * f, &x1, &y1, &z)) {
			if (have) lt_wire_line(c, r.x + px, r.y + py / 2.0, r.x + x1, r.y + y1 / 2.0, col);
			px = x1; py = y1; have = true;
		} else have = false;
	}
}

void lt_globe_ex(lt_canvas *c, lt_rect r, const lt_map_opts *o_in)
{
	if (r.w < 4 || r.h < 2 || !o_in) return;
	lt_map_opts oc = clean(o_in);
	const lt_map_opts *o = &oc;
	style st;
	style_init(&st, o);
	gl g;
	setup(&g, r, o);
	g.st = &st;
	lt_color line = o->line != LT_DEFAULT ? o->line : LT_RGB(0, 255, 65), dim;
	dim_of(line, &dim);
	if (!(o->flags & LT_MAP_WIRE)) lt_shade(c, r, globe_shader, &g);
	else {
		g.R = fmin(g.PW, g.PH) / 2 * 0.98;
		for (int lon = -180; lon < 180; lon += 30) for (int lat = -90; lat < 90; lat += 6) seg(c, r, &g, lon, lat, lon, lat + 6, dim);
		for (int lat = -60; lat <= 60; lat += 30) for (int lon = -180; lon < 180; lon += 6) seg(c, r, &g, lon, lat, lon + 6, lat, dim);
		for (int pass = 0; pass < 2; pass++)
			for (int ci = 0; ci < WNCOUNTRIES; ci++) {
				bool sel = ci == st.sel;
				if ((pass == 1) != sel) continue;
				const wcountry *w = &WCOUNTRIES[ci];
				for (int rr = w->ring0; rr < w->ring0 + w->nrings; rr++) {
					const wring *R = &WRINGS[rr];
					for (int i = 0; i < R->count; i++) {
						int j = (i + 1) % R->count;
						double x0 = WVERTS[2 * (R->start + i)] / 100.0, y0 = WVERTS[2 * (R->start + i) + 1] / 100.0, x1 = WVERTS[2 * (R->start + j)] / 100.0, y1 = WVERTS[2 * (R->start + j) + 1] / 100.0;
						if (fabs(x0 - x1) > 100 || (y0 < -85 && y1 < -85)) continue;
						seg(c, r, &g, x0, y0, x1, y1, sel ? LT_RGB(255, 255, 255) : line);
					}
				}
			}
		lt_wire_circle(c, (lt_rect){r.x + (int)((g.PW - 2 * g.R) / 2), r.y + (int)((g.PH - 2 * g.R) / 4), (int)(2 * g.R + 0.5), (int)(g.R + 0.5)}, line);
	}
	if (o->flags & LT_MAP_LABELS) {
		globectx q = {r, g};
		g.R = fmin(g.PW, g.PH) / 2 / 1.07;
		q.g = g;
		map_draw_labels(c, r, o->flags, g.R / 5.0, line, globe_to_cell, &q);
	}
}

/* the simple entry points */
void lt_globe(lt_canvas *c, lt_rect r, double lon_deg, double tilt_deg, double sun_deg, int flags)
{
	lt_map_opts o = {0};
	o.flags = flags | LT_MAP_BORDERS; o.lon0 = lon_deg; o.tilt = tilt_deg; o.sun_deg = sun_deg;
	lt_globe_ex(c, r, &o);
}
void lt_globe_wire(lt_canvas *c, lt_rect r, double lon_deg, double tilt_deg, lt_color line)
{
	lt_map_opts o = {0};
	o.flags = LT_MAP_WIRE; o.lon0 = lon_deg; o.tilt = tilt_deg; o.line = line;
	lt_globe_ex(c, r, &o);
}
