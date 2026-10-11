/* lazytui.h - the public API of lazytui: a small C library for terminal UIs on plain ANSI escapes.
 *
 * Plain C99 + POSIX + libm, no other dependency. Every piece works on its own:
 *   solver  - Cassowary constraint solver (lt_solver_*)         layout - boxes/splits on top of it (lt_layout_*)
 *   anim    - ODE based animation: springs, RK4 (lt_spring_*, lt_anim_*)
 *   canvas  - cell grid, colour quantising, diff renderer (lt_canvas_*)
 *   term    - raw tty, capability detection, key/mouse/paste decoding (lt_tty_*)
 *   widgets - progressbar, pacmanbar, ringbar, zonebar, keybar, textbar, panel (lt_progress_*, ...)
 *   ui      - event loop with hooks and animation ticks (lt_ui_*)
 * See DESIGN.md for the reasoning behind each part.
 */
#ifndef LAZYTUI_H
#define LAZYTUI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>                                   /* struct tm, for lt_clock_draw */

#ifdef __cplusplus
extern "C" {
#endif

#define LT_VERSION "0.1.0"

/* ================================================================== colours and styles */
typedef uint32_t lt_color;                       /* 0 = terminal default, 0x01000000|n = palette n, 0x02000000|rgb = truecolour */
#define LT_DEFAULT 0u
#define LT_PAL(n) (0x01000000u | ((uint32_t)(n) & 255u))
#define LT_RGB(r, g, b) (0x02000000u | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
enum { LT_BOLD = 1, LT_DIM = 2, LT_ITALIC = 4, LT_UNDERLINE = 8, LT_BLINK = 16, LT_REVERSE = 32, LT_STRIKE = 64 };
typedef struct { lt_color fg, bg; uint8_t attr; } lt_style;
#define LT_STYLE(fg, bg, attr) ((lt_style){(fg), (bg), (attr)})

/* the terminal's abilities: filled by lt_caps_detect() from TERM, COLORTERM, the locale; every widget adapts to it */
typedef struct {
	bool unicode;              /* drawing characters (blocks, box lines, braille) are safe; false on the Linux console */
	bool altscreen;            /* alternate screen exists */
	bool kitty;                /* kitty keyboard protocol answered (held modifiers) - set by lt_tty_open */
	bool mouse;
	int colors;                /* 8, 16, 256 or 1<<24 */
} lt_caps;
lt_caps lt_caps_detect(void);

/* ---- arena and priority queue: the two allocation / scheduling primitives the library itself is built on (and that callers may use) ----
 * lt_arena: bump allocator. lt_arena_alloc is a pointer add (16-byte aligned); lt_arena_reset frees everything at once; save/restore give a stack
 * discipline for temporaries. lt_scratch() is the library-wide scratch arena the widgets use for per-draw temporaries.
 * lt_pq: binary min-heap on a double key (ties FIFO) with an int id and a pointer per entry; remove cancels by id. */
typedef struct lt_arena lt_arena;
typedef struct { void *chunk; size_t used; } lt_arena_mark;
lt_arena *lt_arena_new(size_t chunk_size);
void *lt_arena_alloc(lt_arena *a, size_t n);
void *lt_arena_calloc(lt_arena *a, size_t n);
char *lt_arena_strdup(lt_arena *a, const char *s);
lt_arena_mark lt_arena_save(const lt_arena *a);
void lt_arena_restore(lt_arena *a, lt_arena_mark m);
void lt_arena_reset(lt_arena *a);
size_t lt_arena_used(const lt_arena *a);
void lt_arena_free(lt_arena *a);
lt_arena *lt_scratch(void);
typedef struct lt_pq lt_pq;
lt_pq *lt_pq_new(void);
void lt_pq_free(lt_pq *q);
size_t lt_pq_len(const lt_pq *q);
void lt_pq_clear(lt_pq *q);
bool lt_pq_push(lt_pq *q, double key, int id, void *ptr);
bool lt_pq_peek(const lt_pq *q, double *key, int *id, void **ptr);
bool lt_pq_pop(lt_pq *q, double *key, int *id, void **ptr);
bool lt_pq_remove(lt_pq *q, int id);

/* theme: every widget takes its colours from one of these, so a whole UI is re-coloured by swapping it */
typedef enum { LT_T_TEXT, LT_T_DIM, LT_T_ACCENT, LT_T_OK, LT_T_WARN, LT_T_ERR, LT_T_BAR, LT_T_BAR_BG, LT_T_BORDER, LT_T_TITLE,
               LT_T_KEYCAP, LT_T_MOD_CTRL, LT_T_MOD_ALT, LT_T_MOD_SHIFT, LT_T_LABEL, LT_T_SELECT, LT_T_COUNT } lt_slot;
#define LT_GRAD_MAX 8
typedef struct { lt_style s[LT_T_COUNT]; lt_color grad[LT_GRAD_MAX]; int ngrad; } lt_theme;   /* grad: low -> high, walked by graphs and bars; ngrad 0 = none */
lt_theme lt_theme_default(void);                 /* = matrix */
lt_theme lt_theme_matrix(void);                  /* phosphor green on black: the sci-fi / Matrix look */
lt_theme lt_theme_terminal(void);                /* the terminal's own colours, light-blue gradient */
lt_theme lt_theme_temple(void);                  /* TempleOS: white ground, blue text */
lt_theme lt_theme_mono(void);                    /* black on white, no gradient */
lt_theme lt_theme_mono_reverse(void);            /* white on black, no gradient */
/* registry of every built-in theme (matrix, terminal, temple, mono, dracula, nord, gruvbox-*, solarized-*, tokyo-night, one-*, monokai,
 * rose-pine, catppuccin-{mocha,macchiato,frappe,latte}); dark and light variants where the palette has both */
int lt_theme_count(void);
const char *lt_theme_name(int i);
lt_theme lt_theme_at(int i);                     /* wraps around */
bool lt_theme_by_name(const char *name, lt_theme *out);
lt_color lt_theme_ramp(const lt_theme *t, double f);   /* the gradient at 0..1 (LT_DEFAULT when the theme has none) */
lt_theme lt_theme_parse(const char *spec);       /* "base=nord,accent=#ff8800,grad=#00f;#0f0,..." on top of the default */

/* ================================================================== cassowary solver */
typedef struct lt_solver lt_solver;
typedef int lt_var;                              /* handle of a variable; 0 is invalid */
typedef enum { LT_LE, LT_GE, LT_EQ } lt_op;
#define LT_REQUIRED 1001001000.0
#define LT_STRONG 1000000.0
#define LT_MEDIUM 1000.0
#define LT_WEAK 1.0
typedef struct { lt_var var; double coeff; } lt_term;

lt_solver *lt_solver_new(void);
void lt_solver_free(lt_solver *s);
lt_var lt_var_new(lt_solver *s);
/* sum(coeff*var) + constant  OP  0  with the given strength. Returns false when a REQUIRED constraint cannot hold
 * (the solver is then unusable: build a new one). Prefer LT_STRONG over LT_REQUIRED for anything the terminal size
 * could make impossible (minimum sizes). */
bool lt_solver_add(lt_solver *s, const lt_term *terms, int n, double constant, lt_op op, double strength);
bool lt_solver_edit(lt_solver *s, lt_var v, double strength);      /* makes v suggestable */
bool lt_solver_suggest(lt_solver *s, lt_var v, double value);
double lt_solver_value(const lt_solver *s, lt_var v);

/* ================================================================== layout on top of the solver */
typedef enum { LT_ROW, LT_COL, LT_STACK } lt_dir;             /* children side by side, one under the other, or on top of each other */
typedef enum { LT_SZ_FILL, LT_SZ_FIXED, LT_SZ_MIN, LT_SZ_MAX, LT_SZ_PCT, LT_SZ_CONTENT } lt_szkind;
typedef struct { lt_szkind kind; double v; double weight; } lt_size;
#define LT_FILL(weight) ((lt_size){LT_SZ_FILL, 0, (weight)})
#define LT_FIXED(n) ((lt_size){LT_SZ_FIXED, (n), 1})
#define LT_MIN(n) ((lt_size){LT_SZ_MIN, (n), 1})            /* at least n, then shares the rest like FILL(1) */
#define LT_MAX(n) ((lt_size){LT_SZ_MAX, (n), 1})            /* at most n, otherwise like FILL(1) */
#define LT_PCT(p) ((lt_size){LT_SZ_PCT, (p), 1})            /* p percent of the parent along the direction */
typedef struct { int x, y, w, h; } lt_rect;
typedef struct lt_layout lt_layout;
typedef struct lt_node lt_node;

lt_layout *lt_layout_new(void);
void lt_layout_free(lt_layout *l);
lt_node *lt_layout_root(lt_layout *l, lt_dir dir);
lt_node *lt_node_add(lt_node *parent, const char *name, lt_dir dir, lt_size size);   /* name may be NULL; dir is for its own children */
void lt_node_gap(lt_node *n, int gap);           /* cells between children */
void lt_node_pad(lt_node *n, int top, int right, int bottom, int left);
bool lt_layout_solve(lt_layout *l, int w, int h);/* cheap to repeat: only the two edit variables change while the tree is the same */
lt_rect lt_node_rect(const lt_node *n);
lt_node *lt_layout_find(lt_layout *l, const char *name);         /* anchors: look a node up by name */

/* ================================================================== ODE based animation */
/* a damped spring x'' = -k (x - target) - c x'  integrated with RK4 in fixed sub-steps: stable for any dt */
typedef struct { double x, v, target, k, c; } lt_spring;
lt_spring lt_spring_make(double x, double stiffness, double damping);
lt_spring lt_spring_preset(double x, const char *name);          /* "smooth" (critically damped) "firm" (stiff, critically damped) "snappy" "bouncy" "slow" */
void lt_spring_step(lt_spring *s, double dt);
bool lt_spring_at_rest(const lt_spring *s);                      /* |x - target| and |v| below 1e-3 */
/* the general form: state' = f(t, state), one RK4 step */
typedef void (*lt_ode_fn)(double t, const double *y, double *dydt, int n, void *ctx);
void lt_ode_rk4(lt_ode_fn f, double t, double *y, int n, double dt, void *ctx);

#define LT_ANIM_MAX 64
typedef struct { lt_spring sp[LT_ANIM_MAX]; double *out[LT_ANIM_MAX]; int n; } lt_anim;
void lt_anim_init(lt_anim *a);
int lt_anim_bind(lt_anim *a, double *out, lt_spring initial);   /* *out follows the spring; returns its id */
void lt_anim_to(lt_anim *a, int id, double target);
void lt_anim_snap(lt_anim *a, int id, double value);
bool lt_anim_tick(lt_anim *a, double dt);                        /* true while anything is still moving: keep drawing */

/* ================================================================== canvas: cells, drawing, diff rendering */
typedef struct lt_canvas lt_canvas;
lt_canvas *lt_canvas_new(int w, int h, lt_caps caps);
void lt_canvas_free(lt_canvas *c);
void lt_canvas_resize(lt_canvas *c, int w, int h);
void lt_canvas_clear(lt_canvas *c, lt_style st);
/* a snapshot is a copy of a rectangle of cells: take it after drawing something costly, blit it while that thing's inputs stay the same */
typedef struct lt_snap lt_snap;
lt_snap *lt_snap_take(const lt_canvas *c, lt_rect r);
void lt_snap_blit(const lt_snap *s, lt_canvas *c, int x, int y);
void lt_snap_free(lt_snap *s);
void lt_canvas_copy(lt_canvas *dst, const lt_canvas *src);       /* same size or resizes dst */
/* a clip rectangle every drawing call respects (in addition to the rect it is given): draw a taller content into a window */
void lt_canvas_set_clip(lt_canvas *c, lt_rect r);
void lt_canvas_reset_clip(lt_canvas *c);
int lt_canvas_w(const lt_canvas *c);
int lt_canvas_h(const lt_canvas *c);
const lt_caps *lt_canvas_caps(const lt_canvas *c);
lt_color lt_canvas_bg(const lt_canvas *c, int x, int y);
lt_color lt_canvas_fg(const lt_canvas *c, int x, int y);
uint32_t lt_canvas_char(const lt_canvas *c, int x, int y);      /* the character in a cell (0 = right half of a wide one) */        /* background colour of a cell (LT_DEFAULT if unknown/outside) */
void lt_put(lt_canvas *c, lt_rect clip, int x, int y, uint32_t rune, lt_style st);
int lt_text(lt_canvas *c, lt_rect clip, int x, int y, const char *utf8, lt_style st, int maxw);   /* returns columns used */
void lt_fill(lt_canvas *c, lt_rect r, uint32_t rune, lt_style st);
int lt_utf8_width(const char *s);
size_t lt_utf8_encode(uint32_t r, char *out);
size_t lt_utf8_decode(const char *s, uint32_t *r);               /* bytes used, U+FFFD on bad input */
/* the escape sequences that turn `prev` into `c` (prev may be NULL = draw everything); origin is the screen row of c's row 0 */
size_t lt_canvas_diff_buf(const lt_canvas *c, const lt_canvas *prev, int origin, char **buf, size_t *cap);   /* like lt_canvas_diff, into a reusable buffer; returns the length */
char *lt_canvas_diff(const lt_canvas *c, const lt_canvas *prev, int origin_row, size_t *len);
char *lt_canvas_dump(const lt_canvas *c);                        /* the text of every row, for tests; caller frees */
uint32_t lt_ascii_of(uint32_t rune);                             /* the ASCII stand-in of a drawing character */

/* ================================================================== terminal */
typedef enum { LT_EV_NONE, LT_EV_KEY, LT_EV_MOUSE, LT_EV_PASTE, LT_EV_RESIZE, LT_EV_MODS, LT_EV_TICK } lt_evkind;
#define LT_MOD_ALT (1 << 24)
#define LT_MOD_CTRL (1 << 25)
#define LT_MOD_SHIFT (1 << 26)
enum { LT_K_ENTER = 0x110000, LT_K_TAB, LT_K_BTAB, LT_K_ESC, LT_K_BSPACE, LT_K_UP, LT_K_DOWN, LT_K_LEFT, LT_K_RIGHT, LT_K_HOME, LT_K_END,
       LT_K_PGUP, LT_K_PGDN, LT_K_INS, LT_K_DEL, LT_K_F1, LT_K_F2, LT_K_F3, LT_K_F4, LT_K_F5, LT_K_F6, LT_K_F7, LT_K_F8, LT_K_F9,
       LT_K_F10, LT_K_F11, LT_K_F12, LT_K_CLICK, LT_K_DCLICK, LT_K_RCLICK, LT_K_WHEEL_UP, LT_K_WHEEL_DOWN,
       LT_K_DRAG, LT_K_RELEASE, LT_K_MOVE };      /* drag: mouse moved with the left button down; release: it went up; move: the
                                                      mouse moved with NO button held - only reported when lt_tty_opts.motion is
                                                      set (off by default: every consumer would otherwise pay for a redraw-worthy
                                                      event on every pixel of mouse travel, wanted or not) */
typedef struct { lt_evkind kind; int key; int x, y; int mods; char *paste; } lt_event;   /* key = code point or LT_K_* plus LT_MOD_*; mods = held set for LT_EV_MODS */
typedef struct { bool alt_screen; bool mouse; bool kitty; bool motion; int inline_rows; } lt_tty_opts;   /* inline_rows > 0: draw
                                                      below the cursor instead; motion: also report LT_K_MOVE (hover, no button
                                                      held) - needs `mouse` too, ignored otherwise */
typedef struct lt_tty lt_tty;
lt_tty *lt_tty_open(lt_tty_opts o);
void lt_tty_close(lt_tty *t);
lt_caps lt_tty_caps(const lt_tty *t);
void lt_tty_size(const lt_tty *t, int *rows, int *cols);
int lt_tty_origin(const lt_tty *t);                            /* first screen row (0-based) of the drawing area */
int lt_tty_wait(lt_tty *t, int timeout_ms, lt_event *ev);      /* 1 = event, 0 = timeout; -1 = forever */
int lt_tty_mods(const lt_tty *t);                              /* modifiers held right now (needs the kitty protocol) */
void lt_tty_write(lt_tty *t, const char *s, size_t n);
int lt_key_parse(const char *name);                              /* "ctrl-r", "alt-enter", "f5" -> key code; -1 if unknown */
void lt_key_name(int key, char *out, size_t n);                  /* the reverse, in the same syntax */

/* ================================================================== widgets */
typedef struct { lt_style bar, empty, text; bool percent; const char *label; } lt_progress_opts;
/* value in 0..1. Sub-cell precision with eighth blocks; "[===>   ]" without unicode */
void lt_progressbar(lt_canvas *c, lt_rect r, const lt_theme *t, double value, const char *label, bool percent);
/* One pacman download line, character for character what `pacman` prints with ILoveCandy ("Chomp"), for `cols` columns:
 *   " name<pad>   22.0 MiB  11.8 MiB/s 00:00 [-------------------C  o  o  o  ]  96%"
 * name (truncated with "..."), size done, rate, eta (mm:ss or hh:mm:ss, "--:--" when eta_s < 0), the bar and the percent.
 * The info part is 60% of the width (at least 50 columns); the bar needs 9+ more columns, the percent 5+.
 * The mouth (C/c) toggles whenever the bar advances, so keep one lt_pacman_state per transfer (zero it to start over). */
typedef struct { int lasthash, mouth; } lt_pacman_state;
size_t lt_pacman_line(char *out, size_t cap, int cols, const char *name, double done, double total, double rate, int eta_s, lt_pacman_state *st);
void lt_pacmanbar(lt_canvas *c, lt_rect r, const lt_theme *t, const char *name, double done, double total, double rate, int eta_s, lt_pacman_state *st);
/* text that flows through the widget like a ring buffer (the text, three blanks, again), moved by phase (cells, any
 * ever-growing number): it fills the whole rectangle 1:1 and has no bar under it. color LT_DEFAULT = the theme's accent. */
void lt_ringbar(lt_canvas *c, lt_rect r, const lt_theme *t, double phase, const char *text, lt_color color);
/* proportional coloured segments, ends aligned exactly: weights[i] / sum(weights) of the width. Pure colour, no label text on the bar
 * itself - z[i].label is only read by lt_zonebar_ex's legend (or your own display). */
typedef struct { const char *label; double weight; lt_slot colour; } lt_zone;
void lt_zonebar(lt_canvas *c, lt_rect r, const lt_theme *t, const lt_zone *z, int n);
/* the bar, then (with_legend) one compact line below it: a coloured square, its label and its percentage, per zone, side by side -
 * the same information the bar's zones carry, just named instead of guessed at from colour alone. Needs r.h >= 2 to show it. */
void lt_zonebar_ex(lt_canvas *c, lt_rect r, const lt_theme *t, const lt_zone *z, int n, bool with_legend);
/* history graphs like bsc's: values[0..n) oldest first, scaled lo..hi. Samples fill from the left and scroll once the widget
 * is full. lt_sparkline is one row of eighth blocks (a zero still shows the low bar); lt_graph is an area graph over r.h
 * rows. NaN = no sample (blank). Colour grows from `slot` towards the accent with the value. */
void lt_sparkline(lt_canvas *c, lt_rect r, const lt_theme *t, const double *values, int n, double lo, double hi, lt_slot slot);
void lt_graph(lt_canvas *c, lt_rect r, const lt_theme *t, const double *values, int n, double lo, double hi, lt_slot slot);

/* the planet: an orthographic globe with continents (built-in coarse map), a sun, an atmosphere. lon_deg = the longitude facing the
 * viewer (increase it to spin the Earth eastward past the viewer), tilt_deg = how far the north pole leans towards the viewer
 * (23.4 is the real axial tilt), sun_deg = where the sun stands (0 = upper left of the viewer, positive moves it right and behind;
 * ~150 shows the night side). lt_globe_project gives the cell of a lon/lat on that globe (false on the far side), for markers. */
#define LT_GLOBE_GRID 2          /* the same bit as LT_MAP_GRID */
void lt_globe(lt_canvas *c, lt_rect r, double lon_deg, double tilt_deg, double sun_deg, int flags);
bool lt_globe_project(lt_rect r, double lon_deg, double tilt_deg, double lon, double lat, int *col, int *row);

/* countries (Natural Earth 1:110m, public domain): a raster of country borders behind every map and globe. */
typedef struct { const char *iso, *name_en, *name_pl, *continent; double population_m, gdp_bn, label_lon, label_lat; } lt_country;
int lt_country_count(void);
int lt_country_by_iso(const char *iso3);                      /* -1 if unknown */
bool lt_country_info(int i, lt_country *out);
/* options of the extended calls: flags, wireframe colour, a choropleth (heat map: one value per ISO code, coloured along a ramp from heat_lo
 * to heat_hi; countries without a value stay grey), a highlighted country, and for the globe its orientation. */
typedef struct { const char *iso; double value; } lt_heat_point;
typedef struct {
	int flags;                              /* LT_MAP_* */
	double sun_lon;                         /* LT_MAP_NIGHT: the sun over this longitude */
	lt_color line;                          /* wireframe colour (LT_DEFAULT = green) */
	const lt_heat_point *heat; int nheat;   /* choropleth data */
	double heat_lo, heat_hi;
	const lt_color *ramp; int nramp;        /* NULL = blue - teal - yellow - red */
	const char *selected;                   /* ISO code of a country to highlight */
	double lon0, tilt, sun_deg;             /* globe only: central longitude, pole tilt, sun position */
	double zoom, clon, clat;                /* flat map only: zoom >= 1 (1 = the whole map fits) around the centre lon/lat, like a slippy map */
} lt_map_opts;
void lt_worldmap_ex(lt_canvas *c, lt_rect r, const lt_map_opts *o);
void lt_globe_ex(lt_canvas *c, lt_rect r, const lt_map_opts *o);
int lt_worldmap_country_at(lt_rect r, int col, int row);
/* the same on a zoomed / panned map (o->zoom, clon, clat), and the inverse used for dragging: the lon/lat under a cell */
int lt_worldmap_country_at_ex(lt_rect r, const lt_map_opts *o, int col, int row);
bool lt_worldmap_lonlat_at(lt_rect r, const lt_map_opts *o, int col, int row, double *lon, double *lat);
bool lt_worldmap_project_ex(lt_rect r, const lt_map_opts *o, double lon, double lat, int *col, int *row);      /* the country under a cell of the flat map, -1 for sea */
int lt_globe_country_at(lt_rect r, const lt_map_opts *o, int col, int row);

/* the same planet as thin lines (braille): graticule every 30 degrees, the coastlines, the outline; line = the colour of the coasts, the grid is a
 * dimmer version. lt_worldmap is the flat equirectangular map (2:1, fitted into r and centred), filled like the globe or as wireframe
 * (LT_MAP_WIRE); LT_MAP_GRID adds the 30-degree graticule, LT_MAP_NIGHT shades the side away from the sun (over the equator at sun_lon_deg,
 * so it moves with time). line is the colour of the wireframe only. lt_worldmap_project: lon/lat -> the cell on that map. */
#define LT_MAP_WIRE 1
#define LT_MAP_GRID 2
#define LT_MAP_NIGHT 4
#define LT_MAP_LABELS 8      /* continents, oceans, countries (and with LT_MAP_CITIES, cities): more of them the larger the map; no overlaps */
#define LT_MAP_CITIES 16
#define LT_MAP_BORDERS 32    /* country borders (thin lines on the filled map, all outlines on the wireframe) */
#define LT_MAP_GREY 128      /* no hue: the same map in greys */
#define LT_MAP_POLITICAL 64  /* colour every country differently instead of by climate */
void lt_map_set_language(const char *lang);      /* "pl" or "en"; default: from LC_ALL / LANG */
void lt_globe_wire(lt_canvas *c, lt_rect r, double lon_deg, double tilt_deg, lt_color line);
void lt_worldmap(lt_canvas *c, lt_rect r, int flags, double sun_lon_deg, lt_color line);
bool lt_worldmap_project(lt_rect r, double lon, double lat, int *col, int *row);

/* ---- display widgets (display.c): stateless, fed numbers (and the time for anything that moves) ----
 * spinners: LINE (one rotating stroke), DOTS (braille), ARC, WAVE (five breathing bars), BOUNCE (a ball ping-ponging with a trail),
 *   PARALLAX (layers of dots at different speeds: near = big + fast; uses up to 4 rows of r). label goes after the spinner.
 * piebar: one segmented bar + legend rows below; piechart: circle (or donut) of sectors, clockwise from the top, legend to the right when wide.
 *   A slice colour of LT_DEFAULT takes the categorical palette lt_theme_cat(t, i).
 * gauge: 270-degree arc (or a full ring) filled to value 0..1 along the theme gradient, the percentage in the middle.
 * linechart / barchart / histogram: axes with "nice" tick labels, optional grid, x labels, legend; lines are braille wireframe.
 * heatgrid: GitHub-style grid of cells coloured by value; tagcloud: words centred on wrapped lines, weight -> emphasis;
 * minimap: the page as braille density (one byte per source line) with the viewport highlighted and marks (search hits) on the right edge. */
typedef enum { LT_SPIN_LINE, LT_SPIN_DOTS, LT_SPIN_ARC, LT_SPIN_WAVE, LT_SPIN_BOUNCE, LT_SPIN_PARALLAX } lt_spin_kind;
void lt_spinner(lt_canvas *c, lt_rect r, const lt_theme *t, lt_spin_kind kind, double time, const char *label);
typedef struct { const char *label; double value; lt_color color; } lt_slice;
lt_color lt_theme_cat(const lt_theme *t, int i);
void lt_piebar(lt_canvas *c, lt_rect r, const lt_theme *t, const lt_slice *s, int n, bool with_legend);
void lt_piechart(lt_canvas *c, lt_rect r, const lt_theme *t, const lt_slice *s, int n, bool donut, bool with_legend);
void lt_gauge(lt_canvas *c, lt_rect r, const lt_theme *t, double value, const char *label, bool full_ring);
typedef struct { const char *name; const double *v; int n; lt_color color; } lt_series;
typedef struct { const char *title; double ymin, ymax; bool autoscale, grid, legend, values; const char *const *xlabels; int nxlabels; } lt_chart_opts;
void lt_linechart(lt_canvas *c, lt_rect r, const lt_theme *t, const lt_series *s, int ns, const lt_chart_opts *o);   /* o may be NULL */
void lt_barchart(lt_canvas *c, lt_rect r, const lt_theme *t, const char *const *labels, const double *values, int n, const lt_chart_opts *o);
void lt_histogram(lt_canvas *c, lt_rect r, const lt_theme *t, const double *values, int n, int bins, double lo, double hi, const lt_chart_opts *o);   /* lo >= hi: from the data */
void lt_heatgrid(lt_canvas *c, lt_rect r, const lt_theme *t, const double *v, int cols, int rows, double lo, double hi, int cell_w, const char *const *row_labels);
void lt_tagcloud(lt_canvas *c, lt_rect r, const lt_theme *t, const char *const *words, const double *weights, int n);
void lt_minimap(lt_canvas *c, lt_rect r, const lt_theme *t, const uint8_t *density, int n, int offset, int page, const int *marks, int nmarks);

/* web (spider) chart: axes evenly around the centre, the first at the top, values 0..max. `rings` concentric wireframe polygons and
 * spokes as the grid, the data as a filled polygon with an outline, labels outside the vertices. color LT_DEFAULT = the theme's
 * accent. Grid lines are braille wireframe, the data polygon uses the block-glyph fit, so it stays smooth. */
typedef struct { const char *label; double value; } lt_axis;
void lt_webchart(lt_canvas *c, lt_rect r, const lt_theme *t, const lt_axis *axes, int n, double max, int rings, lt_color color);

/* ship radar: a round scope with range rings, bearing ticks (north up, clockwise), a rotating sweep beam (sweep_deg) with a fading
 * trail, and blips that light up as the beam passes them and then decay. bearing in degrees, range 0..1 of the radius. */
typedef struct { double bearing, range; const char *label; } lt_blip;
void lt_radar(lt_canvas *c, lt_rect r, const lt_theme *t, double sweep_deg, const lt_blip *blips, int n, lt_color color);

/* one line of status text, and an input line with a cursor */
void lt_textbar(lt_canvas *c, lt_rect r, const lt_theme *t, const char *text, lt_slot slot);
void lt_searchbar(lt_canvas *c, lt_rect r, const lt_theme *t, const char *prompt, const char *query, int cursor);   /* cursor in characters */

/* effects driven by time (seconds), stateless: the same time gives the same picture.
 * lt_rain: falling glyph columns with fading trails (katakana and digits; 0/1 and symbols without unicode), drawn only
 *   where a glyph is, so it can sit on top of a filled background. lt_scramble: text that decodes from random glyphs
 *   as progress goes 0..1, a cursor cell at the frontier. */
void lt_rain(lt_canvas *c, lt_rect r, const lt_theme *t, double time, uint32_t seed);
void lt_scramble(lt_canvas *c, lt_rect r, const lt_theme *t, const char *text, double progress, double time);

/* filled shapes with a colour inside. Every cell is sampled 8x16 and drawn with the block glyph (eighths, halves, quadrants) and
 * the two colours that reproduce it best - a true 2-colour fit per cell, so edges are as smooth as a terminal can show;
 * with a known background the colours blend at the edge, with the default background the reverse-video trick supplies the
 * missing orientations. Cells are taken as twice as tall as wide. Without unicode a
 * shape is made of background-coloured blanks (or '#' on 8 colours). fill/outline: LT_RGB(...), LT_PAL(...);
 * outline LT_DEFAULT = none. The shape is centred in r and as large as fits. */
typedef enum { LT_SHAPE_CIRCLE, LT_SHAPE_HEXAGON, LT_SHAPE_OCTAGON } lt_shape;
void lt_shape_draw(lt_canvas *c, lt_rect r, lt_shape shape, lt_color fill, lt_color outline);
void lt_circle(lt_canvas *c, lt_rect r, lt_color fill, lt_color outline);
void lt_hexagon(lt_canvas *c, lt_rect r, lt_color fill, lt_color outline);
void lt_octagon(lt_canvas *c, lt_rect r, lt_color fill, lt_color outline);
/* any polygon (also star-shaped): xy = x0,y0,x1,y1,... in any units, x right, y down; scaled to fit r, aspect kept.
 * lt_ngon: regular polygon, rotation in degrees (0 = a vertex on the right: a hexagon with flat top; 22.5 for a flat-top octagon). */
void lt_polygon(lt_canvas *c, lt_rect r, const double *xy, int n, lt_color fill, lt_color outline);
void lt_ngon(lt_canvas *c, lt_rect r, int sides, double rotation_deg, lt_color fill, lt_color outline);
/* the pixel shader engine behind every shape: fn(x, y, rgb) returns the coverage 0..1 (and the colour, 0..255) at a physical point
 * (x in cell widths, y in half cells, from r's corner); each cell is fitted with the block glyph and two colours that reproduce its
 * 8x16 samples best. Use it for anything that is a picture: gradients, plasma, a planet. */
typedef double (*lt_shader_fn)(double x, double y, double rgb[3], void *ctx);
void lt_shade(lt_canvas *c, lt_rect r, lt_shader_fn fn, void *ctx);
/* the same with fixed coordinates: xy in physical units from r's corner (x in cells, y in half cells), no scaling. Used by charts. */
void lt_polygon_phys(lt_canvas *c, lt_rect r, const double *xy, int n, lt_color fill, lt_color outline);
/* wireframe: thin lines on braille dots (2x4 per cell, square on cells twice as tall as wide); glyphs already drawn are kept.
 * lt_wire_line takes cell coordinates (fractions allowed) of the canvas. */
void lt_wire_line(lt_canvas *c, double x0, double y0, double x1, double y1, lt_color col);
void lt_wire_polygon(lt_canvas *c, lt_rect r, const double *xy, int n, lt_color col);
void lt_wire_ngon(lt_canvas *c, lt_rect r, int sides, double rotation_deg, lt_color col);
void lt_wire_circle(lt_canvas *c, lt_rect r, lt_color col);
bool lt_color_rgb(lt_color c, int *r, int *g, int *b);           /* false for LT_DEFAULT */
lt_color lt_color_mix(lt_color a, lt_color b, double t);         /* t 0..1 from a to b, truecolour result */

/* the state behind a searchbar: UTF-8 text and a cursor with fzf's editing keys (arrows, home/end, backspace, delete,
 * ctrl-a/e/b/f/h/d/k/u/w/y, alt-b/f/d/backspace). lt_input_key returns true when the text or cursor changed. */
typedef struct { char *text; size_t len, cap; int cursor; char *yank; } lt_input;
void lt_input_init(lt_input *in);
void lt_input_free(lt_input *in);
void lt_input_set(lt_input *in, const char *text);
bool lt_input_key(lt_input *in, int key);
bool lt_input_paste(lt_input *in, const char *utf8);
/* a box with a title */
typedef enum { LT_PANEL_ROUND, LT_PANEL_SHARP, LT_PANEL_DOUBLE, LT_PANEL_HUD } lt_panel_style;   /* HUD: heavy corners, [ TITLE ], status pips */
void lt_panel(lt_canvas *c, lt_rect r, const lt_theme *t, const char *title);
void lt_panel_ex(lt_canvas *c, lt_rect r, const lt_theme *t, const char *title, lt_panel_style style);
lt_rect lt_panel_inner(lt_rect r);

/* the key hint bar: caps for Ctrl/Alt/Shift and the key, then the label; with held modifiers known only the
 * hints for those modifiers are shown, like nnn. Hit-testing gives the id of the hint under a click. */
typedef struct { const char *key; const char *label; int id; } lt_hint;      /* key: "ctrl-r", "enter", "up", "space", "f5", "a" ... */
typedef struct lt_keybar lt_keybar;
lt_keybar *lt_keybar_new(const lt_hint *hints, int n);
void lt_keybar_free(lt_keybar *k);
int lt_keybar_rows_needed(const lt_keybar *k, int width, int held_mods, bool know_mods);
void lt_keybar_draw(lt_keybar *k, lt_canvas *c, lt_rect r, const lt_theme *t, int held_mods, bool know_mods);
int lt_keybar_hit(const lt_keybar *k, int x, int y);                        /* hint id, or -1 */

/* ================================================================== interactive widgets: state + draw + event */
/* Each keeps its own state. Draw it, then feed it events (coordinates are canvas cells, as lt_event carries them) with the
 * rectangle it was drawn in. The result tells what happened; LT_W_IGNORED means the event was not for the widget. */
typedef enum { LT_W_IGNORED = 0, LT_W_HANDLED, LT_W_CHANGED, LT_W_ACTIVATE, LT_W_TOGGLE, LT_W_CLOSED } lt_wres;
typedef enum { LT_SIDE_TOP, LT_SIDE_BOTTOM, LT_SIDE_LEFT, LT_SIDE_RIGHT } lt_side;      /* which edge something docks to or reads from */

/* scrollbar in a 1-column rectangle: total rows, first visible row, rows shown. Returns the offset a click/drag at row y means. */
void lt_scrollbar(lt_canvas *c, lt_rect r, const lt_theme *t, int total, int offset, int page);
int lt_scrollbar_offset_at(lt_rect r, int total, int page, int y);

/* list: scrolling, cursor, optional multi-select (space), substring filter, mouse (click, double click, wheel, scrollbar drag) */
typedef struct lt_list lt_list;
lt_list *lt_list_new(void);
void lt_list_free(lt_list *l);
int lt_list_add(lt_list *l, const char *label, int id);               /* returns the item index */
void lt_list_clear(lt_list *l);
void lt_list_set_multi(lt_list *l, bool multi);
int lt_list_count(const lt_list *l);
int lt_list_shown(const lt_list *l);                                 /* items passing the filter */
int lt_list_cursor(const lt_list *l);                                /* item index under the cursor, -1 if none */
void lt_list_set_cursor(lt_list *l, int item);
int lt_list_id(const lt_list *l, int item);
const char *lt_list_label(const lt_list *l, int item);
bool lt_list_marked(const lt_list *l, int item);
void lt_list_filter(lt_list *l, const char *text);                   /* case-insensitive substring; NULL or "" shows all */
void lt_list_draw(lt_list *l, lt_canvas *c, lt_rect r, const lt_theme *t, bool focused);
lt_wres lt_list_event(lt_list *l, lt_rect r, const lt_event *ev);    /* CHANGED: cursor or marks moved; ACTIVATE: enter / double click */
int lt_list_item_at(const lt_list *l, lt_rect r, int x, int y);      /* item index shown at a canvas cell, -1 if none */

/* tree: nodes with children, expand/collapse, guides (|- and `-), same scrolling and mouse as the list */
typedef struct lt_tree lt_tree;
lt_tree *lt_tree_new(void);
void lt_tree_free(lt_tree *t);
int lt_tree_add(lt_tree *t, int parent, const char *label, int id);  /* parent -1 = top level; returns the node index */
void lt_tree_set_expanded(lt_tree *t, int node, bool expanded);
void lt_tree_expand_all(lt_tree *t, bool expanded);
int lt_tree_selected(const lt_tree *t);                              /* node index, -1 if none */
void lt_tree_select(lt_tree *t, int node);                           /* expands the ancestors */
int lt_tree_id(const lt_tree *t, int node);
const char *lt_tree_label(const lt_tree *t, int node);
bool lt_tree_is_expanded(const lt_tree *t, int node);
int lt_tree_count(const lt_tree *t);
void lt_tree_draw(lt_tree *t, lt_canvas *c, lt_rect r, const lt_theme *th, bool focused);
/* keys: arrows, PgUp/PgDn, Home/End, Right expands (or enters), Left collapses (or goes to the parent), Enter/Space toggles a parent
 * (ACTIVATE on a leaf). Mouse: the arrow toggles, the label selects, a double click toggles/activates, wheel scrolls. */
lt_wres lt_tree_event(lt_tree *t, lt_rect r, const lt_event *ev);
int lt_tree_visible(const lt_tree *t);                              /* rows the tree has right now (expanded nodes only) */
int lt_tree_node_at(const lt_tree *t, lt_rect r, int x, int y);     /* the node drawn at a canvas cell, -1 if none */
bool lt_tree_has_children(const lt_tree *t, int node);

/* dropdown: a one-row box; opened it shows a popup list below (or above when there is no room), drawn over everything */
typedef struct lt_dropdown lt_dropdown;
lt_dropdown *lt_dropdown_new(const char *const *items, int n);
/* a dropdown whose popup is a TREE (the tree is not owned): the box shows the label of the chosen node, a click on a leaf (or Enter on it)
 * chooses and closes, arrows / Right / Left expand and move inside the popup, clicking an arrow only toggles. */
lt_dropdown *lt_dropdown_from_tree(lt_tree *tree);
int lt_dropdown_selected_node(const lt_dropdown *d);
void lt_dropdown_free(lt_dropdown *d);
int lt_dropdown_selected(const lt_dropdown *d);
void lt_dropdown_set_selected(lt_dropdown *d, int i);
bool lt_dropdown_is_open(const lt_dropdown *d);
void lt_dropdown_close(lt_dropdown *d);
void lt_dropdown_draw(lt_dropdown *d, lt_canvas *c, lt_rect box, const lt_theme *t, bool focused);
void lt_dropdown_draw_popup(lt_dropdown *d, lt_canvas *c, lt_rect box, const lt_theme *t);   /* call last, after all other widgets */
/* box = where the closed box was drawn. While open every mouse event is consumed (a click outside closes it). */
lt_wres lt_dropdown_event(lt_dropdown *d, lt_rect box, const lt_event *ev, int canvas_h);

/* scroll view: a window onto content taller than itself. begin() clips drawing to the window and returns where to draw the content
 * (its top is above the window by the offset); end() lifts the clip and draws the scrollbar. Wheel, scrollbar click/drag and (optionally)
 * the arrow / PgUp / PgDn / Home / End keys scroll it. show() scrolls so that a part of the content is visible (e.g. the focused widget). */
typedef struct { int offset, content_h; lt_rect view; bool dragging; } lt_scrollview;
void lt_scrollview_init(lt_scrollview *sv);
lt_rect lt_scrollview_begin(lt_scrollview *sv, lt_canvas *c, lt_rect view, int content_h);
void lt_scrollview_end(lt_scrollview *sv, lt_canvas *c, const lt_theme *t);
lt_wres lt_scrollview_event(lt_scrollview *sv, const lt_event *ev, bool keys);
void lt_scrollview_show(lt_scrollview *sv, int content_y, int h);

/* searching: five ways to rank documents by a query. FILTER: every word is a substring. FUZZY: every word matches as a subsequence, scored (runs,
 * word starts, gaps) and ranked. TAG: `tag:x` / `#x` needs the tag, `-tag:x` forbids it, the rest is a substring filter. REGEX: POSIX extended,
 * case-insensitive. MCTS: Monte-Carlo tree search - a tree of tag groups over the documents, UCB1 selection, rollouts scored with the fuzzy
 * score, ranked by visits (an anytime, exploratory ranking; deterministic). Results are document indexes, best first. */
typedef enum { LT_SEARCH_FILTER, LT_SEARCH_FUZZY, LT_SEARCH_TAG, LT_SEARCH_REGEX, LT_SEARCH_MCTS, LT_SEARCH_COUNT } lt_search_mode;
typedef struct { const char *text; const char *tags; } lt_doc;             /* tags: separated by blanks or commas */
const char *lt_search_mode_name(lt_search_mode m);
int lt_search(lt_search_mode m, const char *query, const lt_doc *docs, int n, int *out, int cap);
/* one directive-driven query, meant to replace a mode-selector UI entirely: an optional leading "filter:"/"fuzzy:"/"regex:"/"mcts:"
 * (case-insensitive) picks the mode - no directive defaults to fuzzy - and independently of that, any "tag:x"/"-tag:x"/"#x"/"-#x" word
 * anywhere in the query is pulled out as a tag filter and applied on top of whatever the chosen mode matches, so "tag:net kernel" fuzzy-
 * matches "kernel" and keeps only tag:net docs with nothing to select. *mode_out (may be NULL) reports which mode was actually used. */
int lt_search_auto(const char *query, const lt_doc *docs, int n, int *out, int cap, lt_search_mode *mode_out);
void lt_list_set_order(lt_list *l, const int *items, int n);               /* show exactly these items in this order (ranked results) */
void lt_list_reset_order(lt_list *l);

/* ---- simple controls ---------------------------------------------------------------------------------------------------------------
 * button: "[ label ]" (primary and danger styles); Enter / Space / click = ACTIVATE.
 * check: a checkbox "[x] label", a radio dot "(o) label" or a switch "[ o==] label"; Space / Enter / click on the box or the label toggles (CHANGED).
 * radiogroup: a vertical group, one chosen; arrows move, Space / click chooses (CHANGED).
 * slider: "label [=====o-----] 42"; click / drag on the track sets it, Left/Right step, Home/End jump, the wheel steps (CHANGED). */
typedef enum { LT_BTN_NORMAL, LT_BTN_PRIMARY, LT_BTN_DANGER } lt_btn_style;
typedef struct lt_button lt_button;
lt_button *lt_button_new(const char *label, lt_btn_style style);
void lt_button_free(lt_button *b);
void lt_button_set_label(lt_button *b, const char *label);
int lt_button_width(const lt_button *b);
void lt_button_draw(lt_button *b, lt_canvas *c, lt_rect r, const lt_theme *t, bool focused);
lt_wres lt_button_event(lt_button *b, lt_rect r, const lt_event *ev);

typedef enum { LT_CHECKBOX, LT_RADIO, LT_SWITCH } lt_check_kind;
typedef struct lt_check lt_check;
lt_check *lt_check_new(const char *label, lt_check_kind kind, bool checked);
void lt_check_free(lt_check *c);
bool lt_check_checked(const lt_check *c);
void lt_check_set(lt_check *c, bool on);
void lt_check_draw(lt_check *ck, lt_canvas *c, lt_rect r, const lt_theme *t, bool focused);
lt_wres lt_check_event(lt_check *ck, lt_rect r, const lt_event *ev);

typedef struct lt_radiogroup lt_radiogroup;
lt_radiogroup *lt_radiogroup_new(const char *const *labels, int n);
void lt_radiogroup_free(lt_radiogroup *g);
int lt_radiogroup_selected(const lt_radiogroup *g);
void lt_radiogroup_set(lt_radiogroup *g, int i);
void lt_radiogroup_draw(lt_radiogroup *g, lt_canvas *c, lt_rect r, const lt_theme *t, bool focused);
lt_wres lt_radiogroup_event(lt_radiogroup *g, lt_rect r, const lt_event *ev);

typedef struct lt_slider lt_slider;
lt_slider *lt_slider_new(double min, double max, double step, double value);
void lt_slider_free(lt_slider *s);
double lt_slider_value(const lt_slider *s);
void lt_slider_set(lt_slider *s, double v);
void lt_slider_draw(lt_slider *s, lt_canvas *c, lt_rect r, const lt_theme *t, bool focused, const char *label);
lt_wres lt_slider_event(lt_slider *s, lt_rect r, const lt_event *ev);

/* ---- pixel editor: a small sprite/icon canvas, one cell per pixel (pixeleditor.c) ------------------------------------------------------
 * pen paints the current colour, eraser clears to transparent (LT_DEFAULT, drawn as a dim dot so empty cells are visible), picker copies a
 * pixel's colour into the pen (picking an empty cell leaves the pen alone, rather than turning it transparent and quietly making pen act
 * like another eraser), fill floods the 4-connected region under the cursor. Mouse: pen/eraser click+drag paints a stroke; fill/picker are
 * one-shot (a click that drifts into a drag, as a real click always does a little, does not re-fire them). Keyboard: arrows move a cursor,
 * Enter/Space applies the tool - so the whole thing is testable and usable without a mouse. 'r' resets to a blank sprite. */
typedef enum { LT_PIXEL_PEN, LT_PIXEL_ERASER, LT_PIXEL_PICKER, LT_PIXEL_FILL } lt_pixel_tool;
typedef struct lt_pixeleditor lt_pixeleditor;
lt_pixeleditor *lt_pixeleditor_new(int cols, int rows);
void lt_pixeleditor_free(lt_pixeleditor *p);
void lt_pixeleditor_reset(lt_pixeleditor *p);                             /* every pixel transparent, pen white, tool pen; also bound to 'r' */
void lt_pixeleditor_clear(lt_pixeleditor *p, lt_color fill);              /* LT_DEFAULT = transparent */
void lt_pixeleditor_set(lt_pixeleditor *p, int x, int y, lt_color c);     /* also how a caller seeds/loads an image */
lt_color lt_pixeleditor_get(const lt_pixeleditor *p, int x, int y);
void lt_pixeleditor_set_color(lt_pixeleditor *p, lt_color c);             /* the pen colour */
lt_color lt_pixeleditor_color(const lt_pixeleditor *p);
void lt_pixeleditor_set_tool(lt_pixeleditor *p, lt_pixel_tool tool);
lt_pixel_tool lt_pixeleditor_tool(const lt_pixeleditor *p);
/* off by default (existing callers keep their exact rect); on, draw/event reserve the top row for clickable "[ Pen ][ Eraser ][ Fill ][ Pick ]"
 * buttons and shift the canvas (and its own coordinate system for the caller) down by one row. */
void lt_pixeleditor_set_toolbar(lt_pixeleditor *p, bool on);
bool lt_pixeleditor_toolbar(const lt_pixeleditor *p);
/* LT_SIDE_TOP (default): the row above. BOTTOM: a row below instead. LEFT/RIGHT: a column beside the canvas, like a real paint-app
 * toolbox docked to that edge, one button per row (falls back to just the bracket/label text if there isn't room for both). */
void lt_pixeleditor_set_toolbar_side(lt_pixeleditor *p, lt_side side);
lt_side lt_pixeleditor_toolbar_side(const lt_pixeleditor *p);
int lt_pixeleditor_cols(const lt_pixeleditor *p);
int lt_pixeleditor_rows(const lt_pixeleditor *p);
int lt_pixeleditor_cursor_x(const lt_pixeleditor *p);                     /* the keyboard cursor, for a status line */

/* ---- step sequencer: tracks x steps, one boolean per cell (sequencer.c) ----------------------------------------------------------------
 * click a step, or arrows+Enter/Space, to toggle it; 'r' clears the whole pattern. lt_sequencer_set_playhead highlights a column
 * independently of the on/off state - drive it from your own clock/BPM each frame, this widget only holds the pattern and draws it
 * (lazytui has no audio I/O, so it plays nothing itself). lt_sequencer_set_group adds a one-column gap every N steps (4, typically),
 * matching how a real drum machine groups its beats. */
typedef struct lt_sequencer lt_sequencer;
lt_sequencer *lt_sequencer_new(int tracks, int steps);
void lt_sequencer_free(lt_sequencer *s);
bool lt_sequencer_get(const lt_sequencer *s, int track, int step);
void lt_sequencer_set(lt_sequencer *s, int track, int step, bool on);
void lt_sequencer_toggle(lt_sequencer *s, int track, int step);
void lt_sequencer_clear(lt_sequencer *s);                                 /* every step off; also bound to 'r' */
int lt_sequencer_tracks(const lt_sequencer *s);
int lt_sequencer_steps(const lt_sequencer *s);
void lt_sequencer_set_track_name(lt_sequencer *s, int track, const char *name);
const char *lt_sequencer_track_name(const lt_sequencer *s, int track);
void lt_sequencer_set_playhead(lt_sequencer *s, int step);                /* -1 = none */
int lt_sequencer_playhead(const lt_sequencer *s);
void lt_sequencer_set_group(lt_sequencer *s, int group);                  /* 0 = no grouping */
int lt_sequencer_cursor_step(const lt_sequencer *s);
int lt_sequencer_cursor_track(const lt_sequencer *s);
void lt_sequencer_draw(lt_sequencer *s, lt_canvas *c, lt_rect r, const lt_theme *t, bool focused);
lt_wres lt_sequencer_event(lt_sequencer *s, lt_rect r, const lt_event *ev);
int lt_pixeleditor_cursor_y(const lt_pixeleditor *p);
void lt_pixeleditor_draw(lt_pixeleditor *p, lt_canvas *c, lt_rect r, const lt_theme *t, bool focused);
lt_wres lt_pixeleditor_event(lt_pixeleditor *p, lt_rect r, const lt_event *ev);

/* ---- node editor: a drawio-like canvas of boxes and arrows (nodeeditor.c) --------------------------------------------------------------
 * nodes are small titled panels at world coordinates; edges are wire-line arrows between their borders. Drag a node's body to move it,
 * drag empty canvas to pan; 'c' arms connect mode (click a source node, then a target - ACTIVATE when the edge is made, Esc cancels);
 * 'd' deletes the selected node and every edge touching it. Node and edge ids stay valid across other removals (a free-list). */
typedef struct lt_nodeeditor lt_nodeeditor;
lt_nodeeditor *lt_nodeeditor_new(void);
void lt_nodeeditor_free(lt_nodeeditor *ne);
int lt_nodeeditor_add(lt_nodeeditor *ne, const char *label, int x, int y);            /* world coordinates (cells); returns the node id */
void lt_nodeeditor_remove(lt_nodeeditor *ne, int node);
void lt_nodeeditor_move(lt_nodeeditor *ne, int node, int x, int y);
void lt_nodeeditor_get_pos(const lt_nodeeditor *ne, int node, int *x, int *y);
void lt_nodeeditor_set_label(lt_nodeeditor *ne, int node, const char *label);
const char *lt_nodeeditor_label(const lt_nodeeditor *ne, int node);
int lt_nodeeditor_selected(const lt_nodeeditor *ne);                                   /* node id, -1 none */
void lt_nodeeditor_select(lt_nodeeditor *ne, int node);
int lt_nodeeditor_node_count(const lt_nodeeditor *ne);
int lt_nodeeditor_node_id(const lt_nodeeditor *ne, int i);                             /* the i-th live node's id, for iterating 0..count */
int lt_nodeeditor_node_at(const lt_nodeeditor *ne, lt_rect r, int x, int y);           /* the node drawn at a screen cell, -1 none */
int lt_nodeeditor_connect(lt_nodeeditor *ne, int from, int to);                        /* returns the edge id, or -1 */
void lt_nodeeditor_disconnect(lt_nodeeditor *ne, int edge);
int lt_nodeeditor_edge_count(const lt_nodeeditor *ne);
bool lt_nodeeditor_edge_at(const lt_nodeeditor *ne, int i, int *from, int *to);        /* the i-th live edge, for iterating 0..count */
void lt_nodeeditor_pan(lt_nodeeditor *ne, int ox, int oy);
void lt_nodeeditor_get_pan(const lt_nodeeditor *ne, int *ox, int *oy);
/* zoom scales the SPACING between nodes (>1 spreads them out, <1 draws them closer); box glyphs keep their own size, since text cannot
 * sub-scale. Wheel over the canvas, or +/-/0 (0 also resets the pan) work out of the box via lt_nodeeditor_event. */
double lt_nodeeditor_zoom(const lt_nodeeditor *ne);
void lt_nodeeditor_set_zoom(lt_nodeeditor *ne, double zoom);                            /* clamped to [0.25, 4] */
void lt_nodeeditor_zoom_at(lt_nodeeditor *ne, lt_rect r, double factor, int x, int y);  /* keeps the world point under screen cell (x,y) still */
bool lt_nodeeditor_connecting(const lt_nodeeditor *ne);                                /* true between pressing 'c' and completing/cancelling */
void lt_nodeeditor_draw(lt_nodeeditor *ne, lt_canvas *c, lt_rect r, const lt_theme *t, bool focused);
lt_wres lt_nodeeditor_event(lt_nodeeditor *ne, lt_rect r, const lt_event *ev);

/* ---- gradient picker: stops on a bar (picker.c) ----------------------------------------------------------------------------------------
 * the top row is the gradient itself, the row below one marker per stop. Click near a marker to select (and start dragging) it; click
 * elsewhere on the bar adds a new stop there, seeded with the colour already at that point. Left/Right nudge the selected stop's position,
 * Tab cycles the selection, Delete/Backspace removes it (never below two stops, so the gradient is always well-defined). This widget owns
 * positions only - lt_gradientpicker_set_color assigns the selected stop's colour (pair it with your own colour entry, or lt_theme_cat to
 * cycle through the categorical palette). lt_gradientpicker_to_theme_grad samples it straight into an lt_theme.grad[] for a live preview. */
typedef struct { double pos; lt_color color; } lt_gradstop;
typedef struct lt_gradientpicker lt_gradientpicker;
lt_gradientpicker *lt_gradientpicker_new(void);                                        /* starts as black -> white, two stops */
void lt_gradientpicker_reset(lt_gradientpicker *g);                                    /* back to that black -> white default; also bound to 'r' */
void lt_gradientpicker_free(lt_gradientpicker *g);
void lt_gradientpicker_set_stops(lt_gradientpicker *g, const lt_gradstop *stops, int n);   /* n >= 2 */
int lt_gradientpicker_count(const lt_gradientpicker *g);
lt_gradstop lt_gradientpicker_stop(const lt_gradientpicker *g, int i);
int lt_gradientpicker_selected(const lt_gradientpicker *g);
void lt_gradientpicker_select(lt_gradientpicker *g, int i);
void lt_gradientpicker_set_color(lt_gradientpicker *g, lt_color c);                    /* recolours the selected stop */
lt_color lt_gradientpicker_sample(const lt_gradientpicker *g, double f);               /* the gradient at 0..1 */
int lt_gradientpicker_add_stop(lt_gradientpicker *g, double pos, lt_color c);          /* returns its id (current index) */
void lt_gradientpicker_remove_stop(lt_gradientpicker *g, int i);
void lt_gradientpicker_to_theme_grad(const lt_gradientpicker *g, lt_color *out, int n);   /* samples n colours 0..1, ready for lt_theme.grad */
void lt_gradientpicker_draw(lt_gradientpicker *g, lt_canvas *c, lt_rect r, const lt_theme *t, bool focused);
lt_wres lt_gradientpicker_event(lt_gradientpicker *g, lt_rect r, const lt_event *ev);

/* ---- colour picker: HSV plane + hue bar + hex readout (colorpicker.c) ------------------------------------------------------------------
 * the plane is a real smooth saturation/value gradient (lt_shade) at the current hue; a hue bar runs down the right edge; the bottom row
 * shows a swatch and the hex/rgb value. Click/drag either area; Left/Right/Up/Down nudge saturation/value, '['/']' nudge hue. Feed its
 * result to lt_gradientpicker_set_color, a theme override, or anywhere else an lt_color is wanted. */
typedef struct lt_colorpicker lt_colorpicker;
lt_colorpicker *lt_colorpicker_new(lt_color initial);
void lt_colorpicker_free(lt_colorpicker *p);
void lt_colorpicker_reset(lt_colorpicker *p);                                          /* back to the colour given at construction; also bound to 'r' */
lt_color lt_colorpicker_color(const lt_colorpicker *p);
void lt_colorpicker_set_color(lt_colorpicker *p, lt_color c);
void lt_colorpicker_get_hsv(const lt_colorpicker *p, double *h, double *s, double *v);   /* h: 0..360, s/v: 0..1 */
void lt_colorpicker_set_hsv(lt_colorpicker *p, double h, double s, double v);
void lt_colorpicker_draw(lt_colorpicker *p, lt_canvas *c, lt_rect r, const lt_theme *t, bool focused);
lt_wres lt_colorpicker_event(lt_colorpicker *p, lt_rect r, const lt_event *ev);

/* ---- clock, timer, date/time pickers (datetime.c) --------------------------------------------------------------------------------------
 * lt_clock_draw is stateless like the display widgets: it just draws whatever struct tm it is given (pass a real localtime/gmtime reading,
 * or a fixed one for tests) in one of three forms - a plain "HH:MM[:SS]" line, big block digits (falls back to the plain line if the
 * rectangle is too short), or an analog face with hour/minute/second hands (built on the same polar-coordinate trick as lt_radar).
 * lt_timer is a stopwatch (countdown = 0) or countdown (countdown > 0); like every other time-aware call in this library it takes the
 * current time as a `now` parameter (an lt_now() reading) instead of reaching for the clock itself, which keeps it perfectly testable.
 * Space toggles start/pause, 'r' resets, Up/Down/PgUp/PgDn/wheel dial the target duration up or down without disturbing a run already in
 * progress. lt_datepicker is a one-month calendar grid by default (own civil-calendar math, no libc TZ involved); Left/Right/Up/Down move
 * a day/week, PgUp/PgDn a month, click a day directly, and Tab cycles into quick month- and year-pick views (Left/Right/Up/Down or click
 * a month, Up/Down/PgUp/PgDn a year, Enter/Tab back to the day grid) - the same mode-cycling idiom as treeview. lt_datepicker_set_kind
 * switches to LT_DATE_COMPACT instead: one "YYYY-MM-DD" row of field-spinner segments (own Left/Right/Tab/Up/Down/wheel, no grid or quick-
 * pick views - moving to the year or month segment and nudging it already picks it directly). lt_timepicker is an HH:MM[:SS] field spinner
 * by default (LT_TIME_FIELDS); Left/Right/Tab pick the field, Up/Down/wheel change it, click a field to jump to it.
 * lt_timepicker_set_kind switches its look: LT_TIME_SPINBOX draws each field as its own up-arrow/value/down-arrow column (falls back to
 * FIELDS if the rectangle is too short); LT_TIME_H12 is FIELDS with 12-hour display and a trailing AM/PM segment (still stored as 24h). */
typedef enum { LT_CLOCK_DIGITAL, LT_CLOCK_BIG, LT_CLOCK_ANALOG } lt_clock_kind;
void lt_clock_draw(lt_canvas *c, lt_rect r, const lt_theme *t, lt_clock_kind kind, struct tm when, bool show_seconds);

typedef struct lt_timer lt_timer;
lt_timer *lt_timer_new(void);
void lt_timer_free(lt_timer *tm);
void lt_timer_set_countdown(lt_timer *tm, double seconds);                            /* 0 = stopwatch (counts up); >0 = counts down from this */
void lt_timer_start(lt_timer *tm, double now);
void lt_timer_pause(lt_timer *tm, double now);
void lt_timer_toggle(lt_timer *tm, double now);
void lt_timer_reset(lt_timer *tm, double now);                                        /* back to 0 elapsed / the full countdown; keeps running if it was */
bool lt_timer_running(const lt_timer *tm);
double lt_timer_elapsed(const lt_timer *tm, double now);                              /* seconds counted up, or remaining if counting down (>= 0) */
bool lt_timer_expired(const lt_timer *tm, double now);                                /* a countdown that reached zero; never true for a stopwatch */
void lt_timer_add_duration(lt_timer *tm, double delta_seconds);                       /* dial the target up/down without disturbing a run in progress; 0 = back to a stopwatch */
void lt_timer_draw(const lt_timer *tm, lt_canvas *c, lt_rect r, const lt_theme *t, bool focused, double now);
lt_wres lt_timer_event(lt_timer *tm, lt_rect r, const lt_event *ev, double now);       /* space/click: start/pause; 'r': reset; Up/Down/PgUp/PgDn/wheel: dial the duration */

typedef enum { LT_DATE_GRID, LT_DATE_COMPACT } lt_datepicker_kind;
typedef struct lt_datepicker lt_datepicker;
lt_datepicker *lt_datepicker_new(int year, int month, int day);                       /* month 1..12; day clamped to that month */
void lt_datepicker_free(lt_datepicker *d);
void lt_datepicker_get(const lt_datepicker *d, int *year, int *month, int *day);
void lt_datepicker_set(lt_datepicker *d, int year, int month, int day);
void lt_datepicker_set_kind(lt_datepicker *d, lt_datepicker_kind k);                   /* also resets back to the day grid / first field */
void lt_datepicker_draw(lt_datepicker *d, lt_canvas *c, lt_rect r, const lt_theme *t, bool focused);
lt_wres lt_datepicker_event(lt_datepicker *d, lt_rect r, const lt_event *ev);

typedef enum { LT_TIME_FIELDS, LT_TIME_SPINBOX, LT_TIME_H12 } lt_timepicker_kind;
typedef struct lt_timepicker lt_timepicker;
lt_timepicker *lt_timepicker_new(int h, int m, int s);                                /* clamped to 0..23 / 0..59 / 0..59 */
void lt_timepicker_free(lt_timepicker *tp);
void lt_timepicker_get(const lt_timepicker *tp, int *h, int *m, int *s);
void lt_timepicker_set(lt_timepicker *tp, int h, int m, int s);
void lt_timepicker_set_seconds(lt_timepicker *tp, bool show);                         /* false (default): HH:MM, the seconds field is skipped */
void lt_timepicker_set_kind(lt_timepicker *tp, lt_timepicker_kind k);
void lt_timepicker_draw(const lt_timepicker *tp, lt_canvas *c, lt_rect r, const lt_theme *t, bool focused);
lt_wres lt_timepicker_event(lt_timepicker *tp, lt_rect r, const lt_event *ev);

/* ---- navigation: tabs, breadcrumbs, menus, tree bar -------------------------------------------------------------------------------------
 * tabs: a strip of tabs on any side of a pane (lt_tabs_pane says where the content goes); click / arrows / Home / End / wheel switch (CHANGED).
 * breadcrumbs: a path as clickable segments "home > user > dev", the middle collapsed to "..." when it does not fit; click = ACTIVATE that segment.
 * menu: a menu bar (File Edit View ...) whose menus pull down; items have a shortcut label, may be checkable, separators, disabled. Left/Right change
 *   menu, Up/Down move, Enter / click on an item = ACTIVATE (lt_menu_id says which); Esc / click outside closes. lt_menu_open_at makes a context menu.
 * treebar: a lt_tree navigated as a bar: the path above the current node as breadcrumbs, its children as buttons; Enter / Down go in, Up / Backspace out. */
typedef struct lt_tabs lt_tabs;
lt_tabs *lt_tabs_new(const char *const *labels, int n, lt_side side);
void lt_tabs_free(lt_tabs *t);
int lt_tabs_selected(const lt_tabs *t);
void lt_tabs_set(lt_tabs *t, int i);
lt_rect lt_tabs_pane(const lt_tabs *t, lt_rect whole);              /* what is left of `whole` for the content */
void lt_tabs_draw(lt_tabs *t, lt_canvas *c, lt_rect whole, const lt_theme *th, bool focused);
lt_wres lt_tabs_event(lt_tabs *t, lt_rect whole, const lt_event *ev);

typedef struct lt_breadcrumbs lt_breadcrumbs;
lt_breadcrumbs *lt_breadcrumbs_new(void);
void lt_breadcrumbs_free(lt_breadcrumbs *b);
void lt_breadcrumbs_set_path(lt_breadcrumbs *b, const char *path, char sep);      /* "/home/me/dev" -> home, me, dev (a leading "/" becomes a "/" segment) */
int lt_breadcrumbs_count(const lt_breadcrumbs *b);
int lt_breadcrumbs_cursor(const lt_breadcrumbs *b);
void lt_breadcrumbs_path_upto(const lt_breadcrumbs *b, int index, char sep, char *out, size_t n);   /* the path made of segments 0..index */
void lt_breadcrumbs_draw(lt_breadcrumbs *b, lt_canvas *c, lt_rect r, const lt_theme *t, bool focused);
lt_wres lt_breadcrumbs_event(lt_breadcrumbs *b, lt_rect r, const lt_event *ev);

typedef struct lt_menu lt_menu;
lt_menu *lt_menu_new(void);
void lt_menu_free(lt_menu *m);
int lt_menu_add(lt_menu *m, const char *title);                                   /* returns the menu index */
void lt_menu_item(lt_menu *m, int menu, const char *label, const char *shortcut, int id);
void lt_menu_check(lt_menu *m, int menu, const char *label, const char *shortcut, int id, bool checked);
void lt_menu_sep(lt_menu *m, int menu);
void lt_menu_set_checked(lt_menu *m, int id, bool checked);
void lt_menu_set_enabled(lt_menu *m, int id, bool enabled);
bool lt_menu_checked(const lt_menu *m, int id);
int lt_menu_id(const lt_menu *m);                                                  /* the item activated last */
bool lt_menu_is_open(const lt_menu *m);
void lt_menu_open_at(lt_menu *m, int menu, int x, int y);                         /* as a popup / context menu at a cell */
void lt_menu_close(lt_menu *m);
void lt_menu_draw_bar(lt_menu *m, lt_canvas *c, lt_rect r, const lt_theme *t);
void lt_menu_draw_popup(lt_menu *m, lt_canvas *c, const lt_theme *t);              /* call last, over everything */
lt_wres lt_menu_event(lt_menu *m, lt_rect bar, const lt_event *ev, int canvas_w, int canvas_h);

typedef struct lt_treebar lt_treebar;
lt_treebar *lt_treebar_new(lt_tree *tree);                                         /* the tree is not owned */
void lt_treebar_free(lt_treebar *b);
int lt_treebar_node(const lt_treebar *b);                                          /* the node under the cursor, -1 = none */
int lt_treebar_level(const lt_treebar *b);                                         /* the node whose children are shown, -1 = the top */
void lt_treebar_draw(lt_treebar *b, lt_canvas *c, lt_rect r, const lt_theme *t, bool focused);
lt_wres lt_treebar_event(lt_treebar *b, lt_rect r, const lt_event *ev);
int lt_tree_parent(const lt_tree *t, int node);
int lt_tree_child_count(const lt_tree *t, int node);                                /* node -1 = the top level */
int lt_tree_child(const lt_tree *t, int node, int i);

/* interactive view of the flat map: wheel zooms around the pointer, dragging grabs the map (like a slippy map), double click zooms in,
 * + - 0 and arrows work from the keyboard; a click that did not drag sets `country` and returns ACTIVATE. Draw with lt_mapview_apply(v, &opts)
 * then lt_worldmap_ex; pass the same rectangle and base options (flags do not matter) to the event call. */
typedef struct { double zoom, clon, clat; bool dragging, moved; int gx, gy; double glon, glat; int country; } lt_mapview;
void lt_mapview_init(lt_mapview *v);
void lt_mapview_apply(const lt_mapview *v, lt_map_opts *o);
void lt_mapview_zoom_at(lt_mapview *v, lt_rect r, const lt_map_opts *base, double factor, int col, int row);
lt_wres lt_mapview_event(lt_mapview *v, lt_rect r, const lt_map_opts *base, const lt_event *ev);

/* button bar: the classic dmenu strip - an optional prompt, then the labels side by side, the cursor one inverted, "<" / ">" when it
 * does not all fit. Click = ACTIVATE that button, Left/Right/Tab/Shift-Tab/Home/End move the cursor, Enter activates it, the wheel moves
 * it. An "active" button (the current tab, say) is drawn in the accent colour, independent of the cursor. */
typedef struct lt_buttonbar lt_buttonbar;
lt_buttonbar *lt_buttonbar_new(const char *const *labels, int n);
void lt_buttonbar_free(lt_buttonbar *b);
void lt_buttonbar_set_prompt(lt_buttonbar *b, const char *prompt);
int lt_buttonbar_cursor(const lt_buttonbar *b);                     /* the button under the cursor / last activated */
void lt_buttonbar_set_cursor(lt_buttonbar *b, int i);
void lt_buttonbar_set_active(lt_buttonbar *b, int i);               /* -1 = none */
int lt_buttonbar_count(const lt_buttonbar *b);
void lt_buttonbar_draw(lt_buttonbar *b, lt_canvas *c, lt_rect r, const lt_theme *t, bool focused);
lt_wres lt_buttonbar_event(lt_buttonbar *b, lt_rect r, const lt_event *ev);

/* searchbar events: click moves the cursor, keys edit (lt_input_key), paste inserts. Returns true when the text changed. */
bool lt_searchbar_event(lt_input *in, const char *prompt, lt_rect r, const lt_event *ev, lt_wres *res);

/* ================================================================== app: data-driven and event-driven UI */
/* The whole interface is DATA: a small indented text (a "spec") says what is on screen, where, and which model key each widget shows; the
 * program changes the model and reacts to named EVENTS. Nothing in a spec is code.
 *
 *     col
 *       buttonbar id=top items="main,extras" active=tab size=1
 *       pages bind=tab
 *         scroll
 *           panel title="bars" size=8
 *             progressbar name="progressbar" bind=progress
 *             searchbar id=q prompt="> " bind=query
 *
 * Kinds: row col stack (containers), panel (title, border), scroll (a tall column with a scrollbar; children need fixed sizes), pages (shows
 * child number <bind>), spacer, text, textbar, progressbar, ringbar, zonebar, pacmanbar, searchbar, buttonbar, dropdown, list, tree, keybar,
 * rain, custom. Attributes: id, size (N | fill | fill:W | pct:P | min:N | max:N), gap, pad, title, name (a dim label to the left), bind (the model
 * key), text, prompt, items ("a,b,c"), active (buttonbar: the model key of the current button), max (progressbar: the value that is 100%),
 * zones ("used=k1:err,cache=k2:warn"), hints ("f1:page;tab:focus"), multi, type (custom). Widget content that is not a scalar (list rows, tree nodes)
 * is filled through lt_app_list / lt_app_tree. Events are (id, kind): "change" (a value changed: searchbar text, dropdown, selection),
 * "activate" (enter / double click / button pressed), "toggle" (tree node), "click", "key" (a key nobody used), "tick" (every frame). */
typedef struct lt_ui lt_ui;
typedef struct lt_app lt_app;
typedef struct { const char *id, *kind; const char *text; int index; double number; int key; } lt_appevent;
typedef void (*lt_app_fn)(lt_app *app, const lt_appevent *ev, void *ctx);
typedef void (*lt_custom_draw)(lt_app *app, const char *id, lt_canvas *c, lt_rect r, const lt_theme *t, bool focused, void *ctx);
typedef lt_wres (*lt_custom_event)(lt_app *app, const char *id, lt_rect r, const lt_event *ev, void *ctx);

lt_app *lt_app_new(void);
void lt_app_free(lt_app *a);
bool lt_app_load(lt_app *a, const char *spec, char *err, size_t errn);          /* false + a message with the line number */
/* the model */
void lt_app_set(lt_app *a, const char *key, double value);
void lt_app_set_text(lt_app *a, const char *key, const char *text);
double lt_app_number(const lt_app *a, const char *key);
const char *lt_app_text(const lt_app *a, const char *key);                      /* "" when unset */
/* events and keys */
void lt_app_on(lt_app *a, const char *id, const char *kind, lt_app_fn fn, void *ctx);   /* id or kind "*" matches any */
void lt_app_key(lt_app *a, const char *key, const char *event);                 /* a global hotkey: emits ("app", event) */
void lt_app_emit(lt_app *a, const char *id, const char *kind, const char *text, double number);
void lt_app_every(lt_app *a, double seconds, const char *event);                 /* emits ("app", event) periodically */
/* widgets that hold content */
lt_list *lt_app_list(lt_app *a, const char *id);
lt_tree *lt_app_tree(lt_app *a, const char *id);
lt_dropdown *lt_app_dropdown(lt_app *a, const char *id);
void lt_app_custom(lt_app *a, const char *type, lt_custom_draw draw, lt_custom_event event, void *ctx);   /* the widget behind `custom type=...` */
bool lt_app_rect(const lt_app *a, const char *id, lt_rect *out);                 /* where a widget was drawn last (screen cells) */
void lt_app_focus(lt_app *a, const char *id);
const char *lt_app_focused(const lt_app *a);
/* running it */
void lt_app_render(lt_app *a, lt_canvas *c, const lt_theme *t);                 /* draw one frame (also usable without a terminal) */
bool lt_app_dispatch(lt_app *a, const lt_event *ev, const lt_caps *caps, int held_mods);   /* feed one terminal event; true if used */
void lt_app_tick(lt_app *a, double dt);
void lt_app_invalidate(lt_app *a);                                           /* something the widgets draw changed outside the model: redraw on the next tick */
void lt_app_attach(lt_app *a, lt_ui *ui);                                       /* draw, events and ticks from a lt_ui loop */
lt_ui *lt_app_ui(const lt_app *a);
void lt_app_quit(lt_app *a);

/* ================================================================== ui: loop, hooks, anchors */
typedef void (*lt_draw_fn)(lt_ui *ui, lt_canvas *c, void *ctx);
typedef bool (*lt_event_fn)(lt_ui *ui, const lt_event *ev, void *ctx);      /* return true = handled */
typedef void (*lt_tick_fn)(lt_ui *ui, double dt, void *ctx);
lt_ui *lt_ui_new(lt_tty *t);
void lt_ui_free(lt_ui *u);
lt_anim *lt_ui_anim(lt_ui *u);
lt_layout *lt_ui_layout(lt_ui *u);
const lt_theme *lt_ui_theme(const lt_ui *u);
void lt_ui_set_theme(lt_ui *u, lt_theme t);
lt_tty *lt_ui_term(lt_ui *u);
void lt_ui_on_draw(lt_ui *u, lt_draw_fn fn, void *ctx);
void lt_ui_on_event(lt_ui *u, lt_event_fn fn, void *ctx);                   /* hooks run in the order they were added */
void lt_ui_on_tick(lt_ui *u, lt_tick_fn fn, void *ctx);
void lt_ui_redraw(lt_ui *u);                                                 /* ask for one more frame */
void lt_ui_quit(lt_ui *u);
/* timers live in the loop's priority queue: the loop sleeps until the earliest of them. after = once, every = repeating; both return an id
 * (0 = refused) for lt_ui_cancel. A firing timer requests a redraw. lt_ui_set_fps: how often the tick hooks run (default 30; the
 * spring animations always run at 60 while something moves). lt_ui_arena is reset at the start of every frame: scratch memory for draw code. */
typedef void (*lt_timer_fn)(lt_ui *ui, void *ctx);
int lt_ui_after(lt_ui *u, double seconds, lt_timer_fn fn, void *ctx);
int lt_ui_every(lt_ui *u, double seconds, lt_timer_fn fn, void *ctx);
void lt_ui_cancel(lt_ui *u, int id);
void lt_ui_set_fps(lt_ui *u, double fps);
void lt_ui_set_tick_redraw(lt_ui *u, bool on);          /* default true: every tick redraws; a hook that decides itself turns it off and calls lt_ui_redraw */
lt_arena *lt_ui_arena(lt_ui *u);
double lt_ui_frame_ms(const lt_ui *u);                                       /* how long the last frame took to draw + diff + write */
unsigned long lt_ui_frames(const lt_ui *u);
double lt_ui_frame_avg_ms(const lt_ui *u);
double lt_ui_frame_max_ms(const lt_ui *u);
void lt_ui_run(lt_ui *u);                                                    /* returns after lt_ui_quit */
double lt_now(void);

#ifdef __cplusplus
}
#endif
#endif
