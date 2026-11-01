# lazytui (C)

Terminal UI library on plain ANSI escapes: Cassowary layout, ODE (spring) animation, canvas with diff rendering, widgets
(progressbar, pacmanbar = pacman's own line, ringbar, zonebar, sparkline/graph, radar, circle/hexagon/octagon/polygon + wireframe,
list, tree, dropdown, searchbar, keybar, panels, rain/scramble effects), themes (incl. matrix), event loop with mouse.
Also: five search modes, a country-accurate world map and globe (borders, labels, heat maps, click a country), and `lt_app` -
the whole UI as a text spec + a model + named events. See DESIGN.md. Works on the Linux console too (ASCII, 16 colours, no
alternate screen).

## Layout, and how to use it like an stb file

```
src/            the core: canvas, tty, input, layout, solver, theme, arena, app (the spec language), anim, controls
src/draw/       helpers that draw things for the widgets: shapes.c (circle/polygon/wire), util.h, chartaxes.h, legend.h
src/map/        the world-map data and rasteriser the map widgets sit on (world, labels, view)
src/widgets/basic/    the everyday widgets, ONE FILE PER WIDGET: progressbar, list, tree, dropdown, searchbar, clock, timer, datepicker,
                      timepicker, piechart, gauge, linechart, barchart, histogram, spinner, keybar, panel, ...
src/widgets/extras/   the fancy ones, one file each: worldmap, globe, webchart, radar, heatgrid, heatgrid_dither, tagcloud, minimap,
                      rain, scramble, pixeleditor, nodeeditor, gradientpicker, colorpicker, sequencer
```

For quick dev link the whole library (`liblazytui.a`, or build a shared one from `src/**/*.c`). For the final program copy only the widget
files you use, plus the core: every widget file starts with a comment saying what it needs, and the only private headers a widget may
include are `internal.h` (small shared inline helpers), `draw/util.h`, `draw/chartaxes.h`, `draw/legend.h` and, for the maps, `src/map/*.h`.
The demo has the same two tabs; `examples/TUItemplate.c` is the smallest app to copy (tabs on top, key hints at the bottom, a scrolling
page in between).

```sh
./run                                  # build, test, start the demo
make test                              # test only
make asan                              # test under sanitizers
examples/demo --snapshot 100 30 [page] # one frame as text, no terminal needed
```
