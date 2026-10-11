/* mapinternal.h - the piece of map/globe labelling shared by worldmap.c and globe.c (maplabels.c), and the language state
 * (lt_map_set_language) that must be one real singleton rather than a per-file static, so it lives in maplabels.c too. */
#ifndef LT_MAPINTERNAL_H
#define LT_MAPINTERNAL_H
#include "lazytui.h"

typedef bool (*to_cell_fn)(void *ctx, double lon, double lat, int *col, int *row);

void map_ensure_lang(void);              /* call once per draw before anything that reads map_lang_polish() */
bool map_lang_polish(void);
void map_draw_labels(lt_canvas *c, lt_rect area, int flags, double zoom, lt_color wire, to_cell_fn to_cell, void *ctx);
#endif
