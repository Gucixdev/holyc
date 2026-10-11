# lazytui (C)

Terminal UI library on plain ANSI escapes: Cassowary layout, ODE (spring) animation, canvas with diff rendering, widgets
(progressbar, pacmanbar = pacman's own line, ringbar, zonebar, sparkline/graph, radar, circle/hexagon/octagon/polygon + wireframe,
list, tree, dropdown, searchbar, keybar, panels, rain/scramble effects), themes (incl. matrix), event loop with mouse.
Also: five search modes, a country-accurate world map and globe (borders, labels, heat maps, click a country), and `lt_app` - the whole UI as a text spec + a model + named events. See DESIGN.md. `./run` builds, tests and starts the demo; `make test` only tests; `make asan` tests under sanitizers;
`examples/demo --snapshot 100 30 [page]` prints a frame without a terminal. Works on the Linux console (ASCII, 16 colours,
no alternate screen).
