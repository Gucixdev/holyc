/* theme.c - appearance: borders, colours, focus behaviour, snap distance, fullscreen background.
 * Part of the dwl compositor; values were config.h of dwl (https://codeberg.org/dwl/dwl, see src/LICENSES/). Edit and rebuild. */
#include "dwl.h"

/* Taken from https://github.com/djpohly/dwl/issues/466 */
#define COLOR(hex)    { ((hex >> 24) & 0xFF) / 255.0f, \
                        ((hex >> 16) & 0xFF) / 255.0f, \
                        ((hex >> 8) & 0xFF) / 255.0f, \
                        (hex & 0xFF) / 255.0f }
int sloppyfocus               = 1;  /* focus follows mouse */
int adaptive_sync             = 1;  /* variable refresh rate where the output supports it */
int tearing_allowed           = 1;  /* fullscreen clients may ask for tearing (tearing-control-v1): lowest latency for games */
const int bypass_surface_visibility = 0;  /* 1 means idle inhibitors will disable idle tracking even if it's surface isn't visible  */
unsigned int borderpx         = 1;  /* border pixel of windows */
unsigned int snap             = 32; /* snap pixel */
float rootcolor[]             = COLOR(0x222222ff);
float bordercolor[]           = COLOR(0x444444ff);
float focuscolor[]            = COLOR(0x005577ff);
float urgentcolor[]           = COLOR(0xff0000ff);
/* This conforms to the xdg-protocol. Set the alpha to zero to restore the old behavior */
const float fullscreen_bg[]         = {0.0f, 0.0f, 0.0f, 1.0f}; /* You can also use glsl colors */

