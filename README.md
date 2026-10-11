# holyc

A collection of C tools and libraries, plus the Wayland desktop pieces I run them with. The name
is a nod, not a claim: this has nothing to do with TempleOS's HolyC language, it is plain C
(C89 to gnu11) and a little Rust.

| Directory | What it is | Language | Origin |
|---|---|---|---|
| [`base`](base) | Single-header C library: types, allocators, queues, data structures, math, codecs, crypto | C | own |
| [`lazytui`](lazytui) | Terminal UI library on plain ANSI escapes | C | own |
| [`ragdb`](ragdb) | Embedded note/vector database, IVF + HNSW, hybrid search | C89 | own |
| [`fzf`](fzf) | Fuzzy finder, compatible with fzf 0.74's command line | C | own, not a fork of the Go fzf |
| [`notcoreutils`](notcoreutils) | `cb cloc colorado dv man netcat sudo tgates tmux tree` | C | own |
| [`gtop`](gtop) | System monitor TUI (CPU/GPU, RAM, disk, net, processes) | Rust | own |
| [`dwl`](dwl) | Wayland compositor | C | fork of [dwl](https://codeberg.org/dwl/dwl) |
| [`dmenu`](dmenu) | Launcher, native Wayland | C | fork of [dmenu](https://git.suckless.org/dmenu) (`wayland` branch) |
| [`lemonbar`](lemonbar) | Status bar, native Wayland | C | fork of [lemonbar](https://github.com/LemonBoy/bar) (`wayland` branch) |
| [`nnn`](nnn) | File manager | C | fork of [nnn](https://github.com/jarun/nnn) (`debloat` branch) |

Every directory builds on its own; there is no top-level build. Each has its own README with
the details, this page is the map.

## Quick start

```sh
git clone https://github.com/Gucixdev/holyc && cd holyc

# libraries: tests run from a plain `cc`, no dependencies beyond libc, libm and pthreads
(cd base && make test)            # 97 test programs
(cd base && make bench)           # 5 benchmarks against malloc / naive loops
(cd lazytui && make test)         # widgets, layout, solver, fuzzed input
(cd ragdb && ./run)               # self-tests, fuzzers, thread and crash-safety tests

# tools
(cd fzf && make && make test)     # 413 tests, differential against /usr/bin/fzf when present
(cd notcoreutils && make && make test)   # 59 tests; binaries land in bin/
(cd gtop && cargo build --release)       # binary is called `bsc`
```

## What each piece is

### base
The single-file library everything else shares ideas with. Include what you need, define
`<NAME>_IMPLEMENTATION` in one translation unit (stb style). `core/base.h` carries the base
types, `Option`/`Result`/`TRY`, `str`/`String_View`, `Arena_String` and `print`;
`core/arena_alloc.h` is the arena (plus a growable slab). Then data structures, math, codecs
(png, jpeg, qoi, wav, mp3, deflate, json, csv), `aes256`/`sha256`, `http`, `vfs` and more.
Every module has a test; modules that could not be verified were left out. Ported code is
credited in [`base/THIRD_PARTY.md`](base/THIRD_PARTY.md) with the upstream licence texts.

### lazytui
Cassowary constraint layout, spring animation, a diff-rendered canvas, widgets (bars, charts,
lists, trees, pickers, a world map and globe), themes and mouse input. Works on the Linux
console too (ASCII, 16 colours, no alternate screen). `examples/demo --snapshot W H` prints a
frame without a terminal. Design notes: [`lazytui/DESIGN.md`](lazytui/DESIGN.md).

### ragdb
An embedded vector and note database: bit-packed quantizers, IVF and HNSW routers, BM25 lexical
search fused with vector search, a crash-safe on-disk format and an optional agent-memory layer.
Single node, library only (no server, no auth, no replication). Its README has the measured
scale and recall numbers and what was and was not independently reviewed. Benchmark data
(94 MB) is not in the repository; the README says how to generate it. Some optional helpers
(`embedsrv.py`, `rerank_srv.py`, `wikiprep.py`) are Python and only needed for benchmarking and
reranking; the core has no Python dependency.

### fzf
A from-scratch fzf in C (libc and pthreads only): the FuzzyMatchV2 scoring, extended search
syntax, `--bind` grammar and actions, preview windows, `--listen`, mouse, borders. Not done
(listed in its README): `--gap`, `--tail`, sixel/kitty previews, `--tmux`, shell key-binding
scripts.

### notcoreutils
Small tools without a GNU counterpart, one source file and one man page each: `tree`, `cb`
(clipboard), `cloc`, `dv`, `man`, `netcat`, `sudo` (not installed setuid), `tgates`, `tmux`
helpers, `colorado`. Things GNU already does well are deliberately not reimplemented.

### gtop
A ratatui system monitor reading `/proc` and `/sys` directly (no `sysinfo` crate): CPU with
GPU, RAM, disk, network and a process table with app-grouped, raw and user/system views,
multi-select kill and a command palette. Rust 2021, built with `cargo`.

### The Wayland desktop: dwl, dmenu, lemonbar, nnn
Forks kept as `git subtree`s with the full upstream history, so `git log <dir>/` shows who
wrote what. Local changes on top of upstream:

- **dwl**: i3-style layouts, workspaces and directional focus, the own dmenu wired in as the
  launcher, window-geometry fixes. Builds against wlroots 0.20.
- **dmenu**: ported to native Wayland (`wayland-client`, `xkbcommon`, `fontconfig`, `freetype2`)
  with a layer-shell surface; adds `dmenu_menu`.
- **lemonbar**: native Wayland port sharing dmenu's drawing code.
- **nnn**: an arena allocator to fix an O(n²) `dentfill()`, plus opener scripts that open a GUI
  Emacs without blocking nnn. Release engineering of upstream was removed on purpose.

## Requirements

| Piece | Needs |
|---|---|
| base, lazytui, ragdb, fzf, notcoreutils | a C compiler (gcc or clang), make, Linux. ragdb's `./run` wants bash and gcc |
| gtop | Rust toolchain; crates `ratatui`, `crossterm`, `libc` |
| dwl | wlroots 0.20, wayland, xkbcommon, pkg-config |
| dmenu, lemonbar | wayland-client, xkbcommon, fontconfig, freetype2 |
| nnn | ncurses; readline support is off by default (`O_NORL := 1` in its Makefile) |

The code is developed and tested on Linux (x86-64). Other platforms are not tested.

## Repository layout and history

This repository is where the pieces are published together. The five forked or standalone
subtrees (`dwl`, `dmenu`, `lemonbar`, `nnn`, `gtop`) keep their own upstream history; `base`,
`lazytui`, `ragdb`, `fzf` and `notcoreutils` start here. Issues and patches welcome; run the
directory's tests before sending one.

## Licence

My own code (`base`, `lazytui`, `ragdb`, `fzf`, `notcoreutils`, `gtop`) is MIT, see
[`LICENSE`](LICENSE). The forks keep the licences of their upstreams, in each directory:
`dwl` (GPLv3 with dwm/sway/tinywl notices), `dmenu` (MIT/X), `lemonbar` (MIT), `nnn` (BSD
2-Clause). Third-party code ported into `base` is listed with its licence in
[`base/THIRD_PARTY.md`](base/THIRD_PARTY.md).
