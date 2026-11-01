/* maplabels.c - placing continent/ocean/country/city names on a map or globe without overlaps: importance order, a collision list per
 * row, cities only once the view is close enough. Shared by worldmap.c and globe.c through mapinternal.h (each passes its own
 * lon/lat -> cell projection as `to_cell`). Also owns the one real language switch (lt_map_set_language): both callers must agree on it. */
#include "lazytui.h"
#include "mapinternal.h"
#include "world.h"
#include "places.h"
#include <stdlib.h>
#include <string.h>

static bool polish = false, lang_set = false;
void lt_map_set_language(const char *lang) { polish = lang && (lang[0] == 'p' || lang[0] == 'P') && (lang[1] == 'l' || lang[1] == 'L'); lang_set = true; }
void map_ensure_lang(void) { if (!lang_set) { const char *l = getenv("LC_ALL"); if (!l || !*l) l = getenv("LANG"); lt_map_set_language(l ? l : "en"); } }
bool map_lang_polish(void) { return polish; }

static int label_text(lt_canvas *c, int x, int y, const char *s, lt_color fg, int attr, bool cell_bg)
{
	lt_rect all = {0, 0, lt_canvas_w(c), lt_canvas_h(c)};
	int used = 0;
	for (const char *p = s; *p;) {
		uint32_t q;
		p += lt_utf8_decode(p, &q);
		lt_color bg = cell_bg ? lt_canvas_bg(c, x + used, y) : LT_DEFAULT, f = fg;
		int r, g, b;
		if (cell_bg && lt_color_rgb(bg, &r, &g, &b) && (r * 299 + g * 587 + b * 114) / 1000 > 150) f = LT_RGB(14, 24, 16);   /* dark text on bright cells */
		lt_put(c, all, x + used, y, q, LT_STYLE(f, bg, attr));
		used++;
	}
	return used;
}

typedef struct { int x0, x1, y; } span;

static bool place_label(lt_canvas *c, lt_rect area, span *placed, int *np, int cap, int col, int row, const char *const names[2], bool city, lt_color fg, int attr, bool cell_bg, lt_color mark)
{
	for (int tries = 0; tries < 2; tries++) {
		const char *nm = names[tries];
		if (!nm || !*nm) continue;
		int w = lt_utf8_width(nm), x0 = city ? col + 2 : col - w / 2, x1 = x0 + w;
		if (x0 < area.x || x1 > area.x + area.w || row < area.y || row >= area.y + area.h || *np >= cap) continue;
		bool clash = false;
		for (int k = 0; k < *np && !clash; k++) {
			if (placed[k].y != row) continue;
			if (x0 - 1 < placed[k].x1 && x1 + 1 > placed[k].x0) clash = true;
			if (city && col - 1 < placed[k].x1 && col + 1 > placed[k].x0) clash = true;
		}
		if (clash) continue;
		placed[*np] = (span){city ? col : x0, x1, row}; (*np)++;
		if (city) lt_put(c, (lt_rect){0, 0, lt_canvas_w(c), lt_canvas_h(c)}, col, row, lt_canvas_caps(c)->unicode ? 0x25CF : 'o', LT_STYLE(mark, cell_bg ? lt_canvas_bg(c, col, row) : LT_DEFAULT, LT_BOLD));
		label_text(c, x0, row, nm, fg, attr, cell_bg);
		return true;
	}
	return false;
}

static int cmp_country(const void *a, const void *b)
{
	const wcountry *x = &WCOUNTRIES[*(const int *)a], *y = &WCOUNTRIES[*(const int *)b];
	if (x->minlabel != y->minlabel) return x->minlabel < y->minlabel ? -1 : 1;
	return x->pop_m > y->pop_m ? -1 : x->pop_m < y->pop_m;
}

void map_draw_labels(lt_canvas *c, lt_rect area, int flags, double zoom, lt_color wire, to_cell_fn to_cell, void *ctx)
{
	map_ensure_lang();
	bool wirem = (flags & LT_MAP_WIRE) != 0, cell_bg = !wirem;
	span placed[400];
	int np = 0;
	/* continents and oceans first, then countries by importance, then cities */
	for (size_t i = 0; i < sizeof PLACES / sizeof PLACES[0]; i++) {
		const place *pl = &PLACES[i];
		if (pl->level != 1 || !strcmp(pl->en, "AUSTRALIA")) continue;                 /* Australia is also a country */
		int col, row;
		if (!to_cell(ctx, pl->lon, pl->lat, &col, &row)) continue;
		bool sea = strstr(pl->en, "OCEAN") != NULL;
		const char *names[2] = {polish ? pl->pl : pl->en, pl->sh};
		place_label(c, area, placed, &np, 400, col, row, names, false, wirem ? wire : sea ? LT_RGB(170, 205, 255) : LT_RGB(255, 255, 255), LT_BOLD, cell_bg, LT_DEFAULT);
	}
	bool cities = (flags & LT_MAP_CITIES) && zoom >= 6.0;
	if (cities)                                                                             /* the capital of the reader's country first (Warsaw) */
		for (size_t i = 0; i < sizeof PLACES / sizeof PLACES[0]; i++) {
			const place *pl = &PLACES[i];
			if (pl->level != 2 || (pl->sh && *pl->sh)) continue;
			int col, row;
			if (!to_cell(ctx, pl->lon, pl->lat, &col, &row)) continue;
			const char *names[2] = {polish ? pl->pl : pl->en, NULL};
			place_label(c, area, placed, &np, 400, col, row, names, true, wirem ? wire : LT_RGB(255, 226, 130), LT_BOLD, cell_bg, wirem ? wire : LT_RGB(255, 90, 70));
		}
	int order[WNCOUNTRIES], no = 0;
	for (int i = 0; i < WNCOUNTRIES; i++) if (WCOUNTRIES[i].minlabel <= zoom) order[no++] = i;
	qsort(order, (size_t)no, sizeof(int), cmp_country);
	for (int k = 0; k < no; k++) {
		const wcountry *w = &WCOUNTRIES[order[k]];
		int col, row;
		if (!to_cell(ctx, w->lx, w->ly, &col, &row)) continue;
		if (!strcmp(w->iso, "ATA") || !strcmp(w->iso, "ATF") || !strcmp(w->iso, "FLK")) continue;             /* covered by the continent label / too remote */
		const char *full = polish ? w->pl : w->en;
		const char *names[2] = {lt_utf8_width(full) <= 16 ? full : w->iso, w->iso};
		place_label(c, area, placed, &np, 400, col, row, names, false, wirem ? wire : LT_RGB(236, 240, 226), 0, cell_bg, LT_DEFAULT);
	}
	if (cities)
		for (size_t i = 0; i < sizeof PLACES / sizeof PLACES[0]; i++) {
			const place *pl = &PLACES[i];
			if (pl->level == 1 || pl->level == 2 || (pl->sh && *pl->sh)) continue;                     /* cities are the entries without a country code */
			int col, row;
			if (!to_cell(ctx, pl->lon, pl->lat, &col, &row)) continue;
			const char *names[2] = {polish ? pl->pl : pl->en, NULL};
			place_label(c, area, placed, &np, 400, col, row, names, true, wirem ? wire : LT_RGB(255, 226, 130), LT_BOLD, cell_bg, wirem ? wire : LT_RGB(255, 90, 70));
		}
}
