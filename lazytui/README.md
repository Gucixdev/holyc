# lazytui (C)

Terminal UI library on plain ANSI escapes: Cassowary layout, ODE (spring) animation, canvas with diff rendering, widgets
(progressbar, pacmanbar = pacman's own line, ringbar, zonebar, sparkline/graph, radar, circle/hexagon/octagon/polygon + wireframe,
list, tree, dropdown, searchbar, keybar, panels, rain/scramble effects), themes (incl. matrix), event loop with mouse.
Also: five search modes, a country-accurate world map and globe (borders, labels, heat maps, click a country), and `lt_app` -
the whole UI as a text spec + a model + named events. See DESIGN.md. Works on the Linux console too (ASCII, 16 colours, no
alternate screen).

```sh
./run                                  # build, test, start the demo
make test                              # test only
make asan                              # test under sanitizers
examples/demo --snapshot 100 30 [page] # one frame as text, no terminal needed
```
