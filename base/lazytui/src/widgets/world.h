/* world.h - the country data (Natural Earth 1:110m, public domain) as a raster and as polygons. Internal. */
#ifndef LT_WORLD_H
#define LT_WORLD_H
#include "worlddata.h"
#include <stdbool.h>

#define WGRID_W 720                 /* 0.5 degree cells: column 0 starts at 180W, row 0 at 90N */
#define WGRID_H 360
#define W_SEA 255
void world_init(void);
int world_id(double lon, double lat);               /* country index or -1 (sea) */
double world_land(double lon, double lat);          /* bilinear land fraction: soft coasts */
int world_find_iso(const char *iso);
#endif
