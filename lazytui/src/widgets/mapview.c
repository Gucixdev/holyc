/* mapview.c - the interaction of a slippy map: wheel zooms around the cursor, dragging grabs the map, keys zoom / pan / reset,
 * a click that did not drag picks a country. The state is three numbers (zoom, centre lon/lat); lt_mapview_apply puts them into
 * the lt_map_opts of the next lt_worldmap_ex call. */
#include "internal.h"
#include <math.h>

#define ZMAX 64.0

void lt_mapview_init(lt_mapview *v) { v->zoom = 1; v->clon = v->clat = 0; v->dragging = v->moved = false; v->gx = v->gy = 0; v->glon = v->glat = 0; v->country = -1; }
void lt_mapview_apply(const lt_mapview *v, lt_map_opts *o) { o->zoom = v->zoom; o->clon = v->clon; o->clat = v->clat; }

static void clamp_view(lt_mapview *v)
{
	if (!(v->zoom >= 1)) v->zoom = 1;
	if (v->zoom > ZMAX) v->zoom = ZMAX;
	double mlon = 180.0 * (1.0 - 1.0 / v->zoom), mlat = 90.0 * (1.0 - 1.0 / v->zoom);      /* at zoom 1 the whole map is centred and stays put */
	if (!isfinite(v->clon)) v->clon = 0;
	if (!isfinite(v->clat)) v->clat = 0;
	v->clon = fmax(-mlon, fmin(mlon, v->clon));
	v->clat = fmax(-mlat, fmin(mlat, v->clat));
}

/* zoom by `factor` keeping the point under cell (col,row) where it is */
void lt_mapview_zoom_at(lt_mapview *v, lt_rect r, const lt_map_opts *base, double factor, int col, int row)
{
	lt_map_opts o = base ? *base : (lt_map_opts){0};
	double blon, blat, alon, alat;
	lt_mapview_apply(v, &o);
	lt_worldmap_lonlat_at(r, &o, col, row, &blon, &blat);
	v->zoom *= factor;
	clamp_view(v);
	lt_mapview_apply(v, &o);
	lt_worldmap_lonlat_at(r, &o, col, row, &alon, &alat);
	v->clon += blon - alon; v->clat += blat - alat;
	clamp_view(v);
}

lt_wres lt_mapview_event(lt_mapview *v, lt_rect r, const lt_map_opts *base, const lt_event *ev)
{
	lt_map_opts o = base ? *base : (lt_map_opts){0};
	if (ev->kind == LT_EV_MOUSE) {
		int k = ev->key & 0xFFFFFF;
		lt_mapview_apply(v, &o);
		if (k == LT_K_WHEEL_UP || k == LT_K_WHEEL_DOWN) {
			if (!lt_in(r, ev->x, ev->y)) return LT_W_IGNORED;
			lt_mapview_zoom_at(v, r, base, k == LT_K_WHEEL_UP ? 1.35 : 1 / 1.35, ev->x, ev->y);
			return LT_W_CHANGED;
		}
		if (k == LT_K_DCLICK) { if (!lt_in(r, ev->x, ev->y)) return LT_W_IGNORED; lt_mapview_zoom_at(v, r, base, 2.0, ev->x, ev->y); return LT_W_CHANGED; }
		if (k == LT_K_CLICK) {
			if (!lt_in(r, ev->x, ev->y)) return LT_W_IGNORED;
			v->dragging = true; v->moved = false; v->gx = ev->x; v->gy = ev->y;
			return LT_W_HANDLED;
		}
		if (k == LT_K_DRAG && v->dragging) {
			if (ev->x == v->gx && ev->y == v->gy) return LT_W_HANDLED;
			double lon0, lat0, lon1, lat1;
			lt_worldmap_lonlat_at(r, &o, v->gx, v->gy, &lon0, &lat0);                       /* grab: the point under the press follows the pointer */
			lt_worldmap_lonlat_at(r, &o, ev->x, ev->y, &lon1, &lat1);
			v->clon += lon0 - lon1; v->clat += lat0 - lat1;
			clamp_view(v);
			v->gx = ev->x; v->gy = ev->y; v->moved = true;
			return LT_W_CHANGED;
		}
		if (k == LT_K_RELEASE && v->dragging) {
			v->dragging = false;
			if (!v->moved) { v->country = lt_worldmap_country_at_ex(r, &o, ev->x, ev->y); return LT_W_ACTIVATE; }
			return LT_W_HANDLED;
		}
		return LT_W_IGNORED;
	}
	if (ev->kind != LT_EV_KEY || (ev->key & (LT_MOD_CTRL | LT_MOD_ALT))) return LT_W_IGNORED;
	int k = ev->key & 0xFFFFFF;
	double step = 20.0 / v->zoom;
	switch (k) {
	case '+': case '=': lt_mapview_zoom_at(v, r, base, 1.35, r.x + r.w / 2, r.y + r.h / 2); return LT_W_CHANGED;
	case '-': case '_': lt_mapview_zoom_at(v, r, base, 1 / 1.35, r.x + r.w / 2, r.y + r.h / 2); return LT_W_CHANGED;
	case '0': case LT_K_HOME: v->zoom = 1; v->clon = v->clat = 0; return LT_W_CHANGED;
	case LT_K_LEFT: v->clon -= step; break;
	case LT_K_RIGHT: v->clon += step; break;
	case LT_K_UP: v->clat += step / 2; break;
	case LT_K_DOWN: v->clat -= step / 2; break;
	default: return LT_W_IGNORED;
	}
	clamp_view(v);
	return LT_W_CHANGED;
}
