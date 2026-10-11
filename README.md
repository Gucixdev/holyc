# holyc

A collection of suckless-tool-style forks and own-written CLI tools.

Forks are kept as their own subtree (full upstream history preserved via `git subtree`, not
squashed or copy-pasted in):

- **dwl** — fork of [dwl](https://codeberg.org/dwl/dwl) (itself built on dwm/sway/tinywl): own
  patches on top (i3-style layouts/workspaces/directional focus, own dmenu wired in as the
  launcher, window-geometry fixes). Licenses: `dwl/LICENSE`, `dwl/LICENSE.dwm`,
  `dwl/LICENSE.sway`, `dwl/LICENSE.tinywl`.
- **dmenu** — fork of [dmenu](https://git.suckless.org/dmenu) (branch `wayland`), wired in as
  dwl's launcher above.
- **lemonbar** — fork of [bar/lemonbar](https://github.com/LemonBoy/bar) (branch `wayland`).
- **nnn** — fork of [nnn](https://github.com/jarun/nnn) (branch `debloat`): an arena-allocator
  fix for an O(n²) `dentfill()`, plus personal opener/smart-edit scripts (GUI Emacs integration
  that doesn't block nnn while open). License: `nnn/LICENSE`.

Own-written tools, no upstream to track (also subtree where they had their own git history,
otherwise plain copies - see each entry):

- **gtop** — Rust/ratatui system monitor TUI (subtree, own history, no upstream).
- **fzf** — own fuzzy-finder in C, not a fork of the Go `junegunn/fzf` despite the name (plain
  copy - this one was never its own git repo).
- **notcoreutils** — small own-written CLI utilities (`tree`, `cb`, `cloc`, `dv`, `man`, `netcat`,
  `sudo`, `tgates`, `tmux` helpers) (plain copy - same reason as `fzf`).


- **base** — the most useful single-file C libraries in one place (`include/`): types (`Option`, `Result`,
  `TRY`, `Future`, `DEFER`), arena, slab and pool allocators, queues and jobs, data structures, math,
  codecs, crypto and utilities, 97 tests and benchmarks. Ported code is credited in
  `base/THIRD_PARTY.md`. **`base/lazytui`** is the terminal UI library (Cassowary layout, spring
  animation, diff-rendered canvas, widgets, mouse).
- **ragdb** — an embedded note/vector database in C89: IVF and HNSW indexes with bit-packed
  quantizers, a hybrid lexical + vector search, crash-safe on-disk format, an optional agent-memory
  layer. Self-tests, fuzzers and crash tests in `./run`; benchmark data is not included (see its README).

Each subtree keeps its own upstream remote inside its own git history (visible via
`git log <dir>/`); this top-level repo is just where they're published together.

## Updating from upstream

```sh
git subtree pull --prefix=<dir> <upstream-url> <branch> --squash
```

## License

Each subtree keeps its own upstream license where one exists (see each folder's own `LICENSE*`
files: `dwl/`, `dmenu/`, `lemonbar/`, `nnn/`). `gtop`, `fzf`, and `notcoreutils` are own-written,
unlicensed for now - nothing here is unified under a single license.
