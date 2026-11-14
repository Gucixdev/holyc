# lazytui (C)

Terminal UI library on plain ANSI escapes: Cassowary layout, ODE (spring) animation, a canvas with diff rendering, ~60 widgets,
themes (incl. matrix), an event loop with mouse, five search modes, a country-accurate world map and globe, and `lt_app`:
the whole UI as a text spec + a model + named events. Works on the Linux console too (ASCII, 16 colours, no alternate screen).
Design notes: DESIGN.md. gtop comparison: docs/gtop-crosspollination.md.

## Layout

```
include/lazytui.h        the whole public API (one header)
src/core/fx/             animation and maths: anim (springs), cassowary (+ per), deltatime, perlin, voronoi, theme, ui, fx
src/core/io/             canvas, tty, input, layout, solver, arena, app (the spec language), kinds, controls, nav, pq,
                         shapes + private headers (internal.h, appint.h, util.h, chartaxes.h, legend.h, places.h)
src/widgets/bar/         one-row bars: progress, ring, pacman, zone, pie, bar chart, button, key, search, text, volume
src/widgets/basic/       everyday: list, tree, dropdown, table (layout), panel, frame, gauge, graph, linechart, histogram, piechart, search
src/widgets/datetime/    clock, timer
src/widgets/picker/      colorpicker, datepicker, gradientpicker, timepicker
src/widgets/fancy/       heatgrid(+dither), nodeeditor, pixeleditor, radar, rain, scramble, sequencer, tagcloud, webchart
src/widgets/map/         worldmap, globe, minimap (+ world data and rasteriser: world, maplabels, mapview)
src/widgets/monitor/     system-monitor widgets ported from gtop: histgraph, segbar, corebars, ratepanel, proctable
src/widgets/spinner/     spinner
examples/                demo.c (every widget on two tabs), TUItemplate.c (smallest app to copy)
tests/                   test_*.c, one per area; `make test` runs all of them
```

Every widget file starts with a comment saying what it needs. The Makefile finds sources recursively under `src/` and puts
every directory that has a private header on the include path, so a widget just says `#include "internal.h"`.
To use it like an stb file, copy only the widget files you use plus `src/core/`.

```sh
make                                   # liblazytui.a, examples/demo, examples/TUItemplate
make test                              # every tests/test_*.c under ASAN + UBSAN (leak check for test_robust)
make install PREFIX=~/.local           # include/lazytui.h + liblazytui.a
./run                                  # build, test, start the demo
examples/demo --snapshot 110 330 0     # one frame as text (W H tab [theme]); tab 0 = basic, 1 = extras
```

## Widgets

Status: **tested** = exercised by a tests/ file; **demo** = only drawn in the demo / reached through lt_app (no dedicated assertions).
Keys/mouse are for the interactive ones; "-" = display only.

| widget | file | what | keys / mouse | status |
|---|---|---|---|---|
| progressbar, ringbar, pacmanbar, zonebar | bar/ | one-row progress variants | - | tested (widgets, display) |
| piebar, textbar | bar/ | stacked slice bar; a text line in a theme slot | - | demo |
| barchart | bar/barchart.c | labelled bars | - | tested |
| buttonbar, keybar, searchbar | bar/ | button strip, key hints, input row | click, arrows, typing | tested (controls, search) |
| volumebar | bar/volumebar.c | level bar h/v, mute, backend callbacks | arrows, +/-, PgUp/Dn, Home/End, m; click, drag, wheel | tested (volumebar) |
| list, tree, dropdown | basic/ | scrolling list with marks, tree, dropdown (tree too) | arrows, Enter, click, double click, wheel | tested (interactive) |
| table | basic/table.c | layout container: x columns by y rows | Tab between cells | tested (app) |
| panel, frame | basic/ | bordered container with title | - | tested |
| gauge, graph, histogram, linechart, piechart | basic/ | gauge ring, sparkline/area graph, histogram, multi-series line chart, pie/donut | - | tested (display, widgets) |
| search | basic/search.c | filter / fuzzy / tag / regex / mcts, `lt_search_auto` | in the search bar | tested (search) |
| clock, timer | datetime/ | digital/big/analog clock; countdown/stopwatch | space, r, arrows, wheel | tested (clock) |
| datepicker, timepicker | picker/ | calendar; 24h/12h/spinner time | arrows, Enter, click, wheel | tested (editors) |
| colorpicker, gradientpicker | picker/ | colour and gradient editors | arrows, click, drag | tested (editors) |
| heatgrid, heatgrid_dither | fancy/ | GitHub-style grid, dithered | - | demo |
| radar, webchart, tagcloud, rain, scramble | fancy/ | radar sweep, n-axis web chart, tag cloud, matrix rain, text decode | - | demo |
| pixeleditor, nodeeditor | fancy/ | sprite editor, node graph editor | tools/keys, click, drag | tested (editors) |
| sequencer | fancy/sequencer.c | step sequencer grid | arrows, Enter, click | tested (sequencer) |
| worldmap, globe, minimap | map/ | country map with labels/heat, globe, document minimap | wheel zoom, drag, click a country | tested (display), demo |
| spinner | spinner/spinner.c | busy indicators | - | demo |
| **histgraph** | monitor/histgraph.c | history graph stretched to the width, fixed or auto ceiling; `lt_human_bytes/rate` | - | tested (monitor) |
| **segbar** | monitor/segbar.c | one-row stacked segment bar + suffix | - | tested (monitor) |
| **corebars** | monitor/corebars.c | per-core rows with load-tier colour and history | - | tested (monitor) |
| **ratepanel** | monitor/ratepanel.c | disk/net rate rows: human rate, peak, auto-scaled history | - | tested (monitor) |
| **proctable** | monitor/proctable.c | sortable process table with tree rows | arrows, PgUp/Dn, s/r sort, Right/Left/Enter expand; header click sorts, click, wheel | tested (monitor) |

Core pieces with their own tests: canvas, layout (Cassowary solver), anim, app (spec language), kinds, controls, robust (fuzz).
