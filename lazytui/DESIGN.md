# lazytui (C) - design

A small C library for terminal UIs on plain ANSI escapes. It replaces `setup/bash/lib/lazytui.sh` (fzf based)
step by step; the shell version stays until the C one covers `lt_select`, `lt_wizard`, `lt_progress`.

## Goals (from the notes in setup/bash/lib/readme)

- pure C99 + POSIX + libm, no other dependency, one static library and one header (`include/lazytui.h`)
- scaling that is right at any size and works on the Linux console (`TERM=linux`) as well as kitty/foot
- widgets: pacmanbar (1:1 with pacman ILoveCandy), progressbar, ringbar (text flowing through the bar), zonebar (segments), keybar,
  textbar, searchbar, panel; later tree / graph / list
- Cassowary layout; ODE based animation; theme slots; hooks/anchors/events
- a keybar always at the bottom, like nnn: caps for Ctrl / Alt / Shift, filtered by the modifiers currently held

## Parts

| file | what | why it is separate |
| --- | --- | --- |
| `src/solver.c` | Cassowary: incremental simplex with strengths and edit variables | testable on its own; no terminal knowledge |
| `src/layout.c` | tree of boxes -> constraints -> integer rectangles | the only user of the solver; rebuilds only when the tree changes |
| `src/anim.c` | springs (RK4), general `lt_ode_rk4` | pure math, no time source |
| `src/canvas.c` | cell grid, colour quantising, ASCII fallback, diff renderer | never touches a tty: everything is unit-tested as text |
| `src/tty.c` | raw mode, capability detection, key/mouse/paste/kitty decoding | the only file that talks to the terminal |
| `src/theme.c` | colour slots and presets | one place to re-colour a whole UI |
| `src/widgets.c` | bars, panel, keybar | stateless: a widget is `draw(canvas, rect, theme, numbers)` |
| `src/ui.c` | event loop, hooks, animation ticks, frame diff | glue only |

## Layout (Cassowary)

Every node has four variables (x y w h). Constraints, strongest first:

1. root size = terminal size (edit variables; changing it is two `suggest` calls, a few dual pivots)
2. FIXED / PCT sizes
3. MIN / MAX bounds
4. the children fill their box
5. FILL weight ratios

Positions inside a box (each child starts where the previous one ends) and `w,h >= 0` are REQUIRED - they always
hold, so the solver never reports a conflict however small the terminal is. Anything a terminal could make
impossible (a minimum width) is a strong wish, not a required constraint: on a tiny screen the box degrades in
that order instead of failing.

Rounding: results are real numbers; both edges of a rectangle are rounded, never its size, so neighbours meet
exactly and sizes sum to the whole at every terminal width (tested for 1:2:1 at 4..250 columns).

Not supported: removing a constraint. A layout whose structure changes is rebuilt (cheap: tens of variables).

## Animation (ODE)

`lt_spring`: x'' = -k (x - target) - c x', integrated with RK4 in sub-steps of at most 1/240 s. Retargeting
keeps the velocity, so motion never jumps; a stalled process (dt clamped to 0.25 s) cannot blow it up; the spring
snaps to rest below 1e-3 so an idle UI stops drawing. `lt_ode_rk4` is the general integrator for anything else
(a marquee phase, a pendulum). Presets: smooth (critically damped, no overshoot), firm (stiffer, still critically damped - the demo's zonebar), snappy, bouncy, slow.
The tests compare against the closed-form solution.

Frame policy (ui.c): a UI with no moving spring and no tick hook blocks in poll(): zero CPU when idle.

## Canvas and terminals

- Cells are (rune, style). `lt_canvas_diff(cur, prev)` returns the shortest escape stream; wide characters are
  handled; runs are written after a single cursor move.
- Colours are quantised to the terminal: truecolour -> 256 -> 16 -> 8 (nearest in RGB).
- `lt_caps_detect`: unicode (UTF-8 locale and not the Linux console), alternate screen, mouse, colours
  (`COLORTERM`, `TERM`). `TERM=linux`: no alternate screen (inline mode + cleanup), 16 colours, ASCII drawing
  characters (`lt_ascii_of`), no kitty query (`CSI ? u` would restore the cursor there).
- kitty keyboard protocol (only when the terminal answers): modifier keys arrive as events, so the keybar can show
  just the hints for the modifiers held. Without it the keybar shows every hint.
- Modes: alternate screen, or inline (`inline_rows`) below the cursor.

## pacmanbar (1:1 with pacman)

`lt_pacman_line` reproduces pacman's ILoveCandy download line character for character: 60% of the width (min 50) is
the info part (` name  size  rate eta`), then ` [bar]` (needs 9+ columns of room) and ` NNN%` (5+). The bar is
`-` for done, `C`/`c` for the mouth (toggles whenever the bar advances - keep one `lt_pacman_state` per transfer),
then pellets by `i % 3` (`o`, blank, blank). Sizes use 1 decimal and auto-scale above 2048 (KiB, MiB, ...), rates
2/1/0 decimals, the name is cut to `name...`. The tests are golden lines captured from the real `pacman -Sy`
(local http server, pty) at 50, 55, 59, 62, 66, 80 and 100 columns.

## Shapes, wireframe, radar

`lt_circle`, `lt_hexagon`, `lt_octagon`, `lt_ngon`, `lt_polygon` (any polygon, also star-shaped), `lt_polygon_phys` (fixed
coordinates, for charts). Every cell is sampled 8x16 (cell = 1 wide x 2 tall in physical units) and drawn with the block glyph
(eighths, halves, quadrants) and the pair of colours that reproduce the samples best: a two-colour fit per cell, so a curve is
followed to ~1/8 cell and the edge blends into a known background (the 36x18 circle in the tests is smooth to the eye; small
ones are limited by the cell grid itself, not by the fit). With the terminal's default background only the shape's colour can
be named: the fit minimises the geometric mismatch and the missing glyph orientations come from reverse video.
No unicode: background-coloured blanks (16+ colours) or `#`/`+` (8).

Wireframe (`lt_wire_line/polygon/ngon/circle`): thin lines on braille dots, 2x4 per cell, i.e. square dots. Glyphs already drawn
(a filled shape's edge) are never overwritten, so a grid can be laid over a fill.

`lt_radar`: n axes (first at the top), `rings` wire polygons + spokes as the grid, the data as a filled polygon with outline,
labels outside the vertices. Meant to sit next to web/radar charts that must look right in a terminal.

## Shader engine, globe, radar

`lt_shade(canvas, rect, fn, ctx)`: `fn(x, y, rgb)` returns coverage 0..1 and a colour for a physical point; every cell is fitted with the block
glyph and two colours (least squares from partition sums: cheap enough for a 60x30 picture per frame). Circle, polygons and the globe are
shaders. `lt_globe`: orthographic sphere, built-in coarse world map (continent outlines rasterised once into a 1-degree land mask, bilinear
coasts), axial tilt, sun (lambert + sea glint), atmosphere rim and glow, optional graticule; `lt_globe_project` places markers (lon/lat -> cell,
false on the far side). `lt_radar`: the ship scope - round face, range rings, bearing ticks, sweep beam with a fading trail, blips that light up
when the beam passes and decay. `lt_webchart` is the spider chart (the old polygon-grid radar). The same Earth also comes as thin lines (`lt_globe_wire`: graticule, coastlines, outline, braille) and as a flat equirectangular map (`lt_worldmap`: filled with a moving day/night terminator, or wireframe, optional graticule; `lt_worldmap_project` places markers).

## Scrolling a page

`lt_canvas_set_clip` makes every drawing call respect a window; `lt_scrollview` uses it: `begin` clips and returns where the content goes,
`end` draws the scrollbar, `event` handles wheel / scrollbar drag / (optionally) keys, `show` scrolls a part into view (the focused widget).
Lists and trees give the wheel back to the page when they have nothing to scroll themselves.

## History graphs

`lt_sparkline` (one row of eighth blocks, a zero still shows the low bar, NaN = gap) and `lt_graph` (area graph over r.h rows),
like bsc's process history: samples fill from the left and scroll once full; colour grows towards the accent with the value.

## Interactive widgets

State + `draw` + `event` (events carry canvas cells; the widget is given the rectangle it was drawn in). Result: IGNORED (not
for this widget), HANDLED, CHANGED, ACTIVATE (enter / double click), TOGGLE (tree node), CLOSED (dropdown).

- `lt_list`: scrolling, cursor, multi-select (space), case-insensitive filter, mouse (click, double click, wheel scrolls the view
  and not the cursor, scrollbar click + drag), pointer/mark glyphs with ASCII forms.
- `lt_tree`: expand/collapse, `tree(1)`-style guides, keys (arrows, Right/Left expand/collapse or step in/out, Enter/Space),
  mouse (arrow toggles, label selects, double click toggles/activates, wheel, scrollbar). Row layout is a pure function of
  depth, so hit testing never depends on having drawn.
- `lt_dropdown`: box + popup opening below or above; modal while open (a click outside closes it); the popup is drawn by its
  own call so it goes over everything.
- `lt_searchbar_event` + `lt_input`: click puts the cursor, fzf's editing keys, paste.
- `lt_scrollbar`: thumb size proportional, click/drag mapping.
The tty layer reports drag (button held) and release events for this.

## Keys

The tty layer asks for the kitty protocol with flags 31 (disambiguate, event types, alternate keys, all keys, text). A key arrives as text when
the terminal sends it, else as the shifted alternate key (`code:shifted`), else via a US-layout shift map; the keypad keys map to their characters.
So `+`, `-`, `=` work whatever the terminal does.

## Countries, borders, heat maps

The maps and globes are drawn from Natural Earth 1:110m admin-0 countries (public domain; `src/worlddata.h` is generated once by a script from
the GeoJSON, 177 countries, ~10k vertices, English and Polish names, label points, population, GDP). `world.c` rasterises them (scanline, even-odd,
holes work) into a 0.5-degree grid of country indexes; everything else is built on that grid: climate colours or one colour per country
(`LT_MAP_POLITICAL`), thin borders (`LT_MAP_BORDERS`), a choropleth (`lt_map_opts.heat`: one value per ISO code along a ramp, no-data countries grey),
a highlighted country (`selected`), labels for continents, oceans, countries (by importance, full name -> ISO code -> dropped when it would overlap)
and cities, wireframes (every ring as braille lines), and lookups from a screen cell to the country under it (`lt_worldmap_country_at`,
`lt_globe_country_at`) so the mouse can pick countries. `lt_country_info` gives the record. Language: `lt_map_set_language` or `LANG`.

## Searching, button bar, tree dropdown

`lt_search` ranks documents five ways: filter, fuzzy (runs, word starts, gaps), tag (`tag:x`, `#x`, `-tag:x` + words), regex, and an MCTS ranking
(a tree of tag groups, UCB1, rollouts scored with the fuzzy score, deterministic). `lt_list_set_order` shows the ranked results in a list.
`lt_buttonbar` is the dmenu strip (prompt, labels, `<` `>` when it does not fit, an "active" button for tabs). `lt_dropdown_from_tree` is a dropdown whose
popup is a tree (choose a leaf).

## App: data-driven and event-driven

`lt_app` builds a whole interface from a SPEC (indented text, see lazytui.h): containers become Cassowary layout nodes (`scroll` starts its own layout of
fixed-size children), widgets are instantiated and drawn from a key/value MODEL (`bind=key`, `active=key`), and terminal events are routed (open dropdown
first, then the widget under the mouse, the focused widget, hotkeys, Tab focus, scrolling) and come out as named events `(id, kind)` for handlers
(`lt_app_on`). Global hotkeys emit `("app", name)`; modified keys win even over a focused text field, plain characters only when no text field has the
focus. Widgets whose content is not a scalar are filled through `lt_app_list/tree/dropdown`; anything else is a `custom` node backed by draw/event
callbacks. Errors in a spec carry the line number. `lt_app_render` and `lt_app_dispatch` work without a terminal, which is how it is tested.

## Effects and the matrix theme

`lt_theme_matrix` (phosphor green on black), `lt_rain` (falling glyph columns: constant-velocity heads, fading trails, katakana
and digits; 0/1 and symbols without unicode) and `lt_scramble` (text that decodes from random glyphs) are pure functions of
time. `lt_panel_ex(..., LT_PANEL_HUD)`: heavy corners, `[ TITLE ]`, status pips.

## Demo

`examples/demo` (now written as an app spec + handlers: two tabs main / extras, a dmenu-style button bar on top, purple frames like gtop): three pages (F1..F3, tabs are clickable), every widget in its own titled panel. Values never stand still:
every refresh interval new targets are drawn and springs carry the widgets; `+`/`-` change the interval. Page 3 has tree, list,
dropdown (it switches the theme), searchbar (filters the list). `demo --snapshot W H PAGE` prints a frame without a tty.

## Robustness

`make asan` runs every test under ASan + UBSan (+ float-cast-overflow); `test_robust` also feeds 200k random key/mouse events to
list, tree, dropdown and searchbar with degenerate rectangles, and draws every widget at every
canvas size 1..70 x 1..12 with nonsense rectangles (negative, outside), NaN/inf values, three capability sets, and
solves a nested layout for sizes -2..300. Nothing may crash, overflow or leak.

## Keybar

`lt_hint {key, label, id}`: `key` is an fzf-style name (`ctrl-r`, `alt-enter`, `f5`, `space`). Each modifier is its
own coloured cap (Ctrl blue, Alt yellow, Shift green), then the key cap, then the label; hints wrap into the rows
given. `lt_keybar_hit` maps a click to the hint id. The same look is in the C fzf (`fzf --hints`).

## Testing

`make test`: solver classics + strength ladder + graceful degradation; layout at every width; springs against the
closed form; canvas dumps, clipping, ASCII, quantising and diff output; widgets as text snapshots in unicode and
ASCII; keybar filtering by modifiers. `examples/demo --snapshot W H` prints one frame with no tty.
The interactive paths (tty.c, ui.c) were exercised with a pty + pyte harness (kitty protocol answers, resize,
TERM=linux).

## Roadmap

1. `lt_select` (raw and select modes: arrows move, space marks, tab moves, `/` search) and the yaml key map
   (`action = key`), then `lt_wizard`, on top of the existing parts.
2. list / tree / graph widgets, scrollbars, text input with editing (cursor, words, kill/yank as in fzf).
3. named anchors + event/hook system beyond `lt_layout_find` and the hook lists (bubbling, focus).
4. a small `lt` CLI so shell scripts can use the widgets (`lt progress`, `lt pacman`, `lt keybar`), replacing
   `pacmanbar` and the fzf-based `lazytui.sh`.
5. timeline / easing curves next to springs; sixel/kitty graphics for `graph`.

## Toolchain notes (this must build in ten years)

C99, `-D_POSIX_C_SOURCE=200809L`, libc + libm only, no generated files, no submodules. The only external contract
is the ANSI/xterm/kitty sequences listed in `tty.c` and `canvas.c`.
