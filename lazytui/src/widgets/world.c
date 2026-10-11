/* world.c - rasterises the country polygons once (scanline fill, even-odd over all rings of a country, so holes work) into a 0.5-degree
 * grid of country indexes. Lookups and the border/heat/label features are built on that grid. */
#include "world.h"
#include "lazytui.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static uint8_t grid[WGRID_H][WGRID_W];
static bool ready;

static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

static void fill_country(int ci)
{
	const wcountry *c = &WCOUNTRIES[ci];
	double *xs = malloc(4096 * sizeof *xs);
	int cap = 4096;
	double ymin = 1e9, ymax = -1e9;
	for (int r = c->ring0; r < c->ring0 + c->nrings; r++)
		for (int i = 0; i < WRINGS[r].count; i++) { double y = WVERTS[2 * (WRINGS[r].start + i) + 1] / 100.0; ymin = fmin(ymin, y); ymax = fmax(ymax, y); }
	int row0 = (int)floor((90.0 - ymax) * 2) - 1, row1 = (int)ceil((90.0 - ymin) * 2) + 1;
	if (row0 < 0) row0 = 0;
	if (row1 >= WGRID_H) row1 = WGRID_H - 1;
	for (int row = row0; row <= row1; row++) {
		double lat = 90.0 - (row + 0.5) / 2.0;
		int n = 0;
		for (int r = c->ring0; r < c->ring0 + c->nrings; r++) {
			const wring *R = &WRINGS[r];
			for (int i = 0, j = R->count - 1; i < R->count; j = i++) {
				double x0 = WVERTS[2 * (R->start + j)] / 100.0, y0 = WVERTS[2 * (R->start + j) + 1] / 100.0;
				double x1 = WVERTS[2 * (R->start + i)] / 100.0, y1 = WVERTS[2 * (R->start + i) + 1] / 100.0;
				if ((y0 > lat) == (y1 > lat)) continue;
				if (n == cap) { cap *= 2; xs = realloc(xs, (size_t)cap * sizeof *xs); }
				xs[n++] = x0 + (lat - y0) * (x1 - x0) / (y1 - y0);
			}
		}
		qsort(xs, (size_t)n, sizeof *xs, cmp_d);
		for (int k = 0; k + 1 < n; k += 2) {
			int a = (int)ceil((xs[k] + 180.0) * 2 - 0.5), b = (int)floor((xs[k + 1] + 180.0) * 2 - 0.5);
			if (a < 0) a = 0;
			if (b >= WGRID_W) b = WGRID_W - 1;
			for (int col = a; col <= b; col++) grid[row][col] = (uint8_t)ci;
		}
	}
	free(xs);
}

/* soft[y][x]: the land/sea classification differs somewhere in the 3x3 neighbourhood, so the bilinear land fraction is not simply 0 or 1 */
static uint8_t soft[WGRID_H][WGRID_W];

void world_init(void)
{
	if (ready) return;
	memset(grid, W_SEA, sizeof grid);
	for (int i = 0; i < WNCOUNTRIES; i++) fill_country(i);
	for (int y = 0; y < WGRID_H; y++)
		for (int x = 0; x < WGRID_W; x++) {
			bool land = grid[y][x] != W_SEA, diff = false;
			for (int dy = -1; dy <= 1 && !diff; dy++)
				for (int dx = -1; dx <= 1; dx++) {
					int yy = y + dy < 0 ? 0 : y + dy >= WGRID_H ? WGRID_H - 1 : y + dy, xx = (x + dx + WGRID_W) % WGRID_W;
					if ((grid[yy][xx] != W_SEA) != land) { diff = true; break; }
				}
			soft[y][x] = diff;
		}
	ready = true;
}

int world_id(double lon, double lat)
{
	if (!isfinite(lon) || !isfinite(lat)) return -1;
	if (lon >= 180.0 || lon < -180.0) lon = fmod(lon, 360.0);              /* fmod is slow and the common case is already in range */
	int x = (int)floor((lon + 180.0) * 2), y = (int)floor((90.0 - lat) * 2);
	x = ((x % WGRID_W) + WGRID_W) % WGRID_W;
	if (y < 0) y = 0;
	if (y >= WGRID_H) y = WGRID_H - 1;
	uint8_t v = grid[y][x];
	return v == W_SEA ? -1 : v;
}

double world_land(double lon, double lat)
{
	if (!isfinite(lon) || !isfinite(lat)) return 0;
	if (lon >= 180.0 || lon < -180.0) lon = fmod(lon, 360.0);
	int nx = (int)floor((lon + 180.0) * 2), ny = (int)floor((90.0 - lat) * 2);              /* the cell the point is in */
	nx = ((nx % WGRID_W) + WGRID_W) % WGRID_W; ny = ny < 0 ? 0 : ny >= WGRID_H ? WGRID_H - 1 : ny;
	if (!soft[ny][nx]) return grid[ny][nx] != W_SEA;                                         /* deep inside land or sea: no blending needed */
	double fx = (lon + 180.0) * 2 - 0.5, fy = (90.0 - lat) * 2 - 0.5;
	int x0 = (int)floor(fx), y0 = (int)floor(fy);
	double tx = fx - x0, ty = fy - y0, v = 0;
	for (int j = 0; j < 2; j++)
		for (int i = 0; i < 2; i++) {
			int xx = ((x0 + i) % WGRID_W + WGRID_W) % WGRID_W, yy = y0 + j;
			yy = yy < 0 ? 0 : yy >= WGRID_H ? WGRID_H - 1 : yy;
			v += (grid[yy][xx] != W_SEA) * (i ? tx : 1 - tx) * (j ? ty : 1 - ty);
		}
	return v;
}

int world_find_iso(const char *iso)
{
	if (!iso) return -1;
	for (int i = 0; i < WNCOUNTRIES; i++) if (!strcmp(WCOUNTRIES[i].iso, iso)) return i;
	return -1;
}

int lt_country_count(void) { return WNCOUNTRIES; }
int lt_country_by_iso(const char *iso) { return world_find_iso(iso); }
bool lt_country_info(int i, lt_country *out)
{
	if (i < 0 || i >= WNCOUNTRIES || !out) return false;
	static const char *conts[] = WCONTINENTS;
	const wcountry *c = &WCOUNTRIES[i];
	*out = (lt_country){c->iso, c->en, c->pl, conts[c->continent], c->pop_m, c->gdp_bn, c->lx, c->ly};
	return true;
}
