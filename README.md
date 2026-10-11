# holyc

A collection of C tools and libraries, plus the Wayland desktop pieces I run them with. The name
is a nod, not a claim: this has nothing to do with TempleOS's HolyC language, it is plain C
(C89 to C11, see the table for which) and nothing else.

| Directory | What it is | Language, exactly | Origin |
|---|---|---|---|
| [`base`](base) | Single-header C library: 107 modules (types, allocators, queues, data structures, math, codecs, crypto, net) | C89 style, one `long long` beyond C89; tests pass with `-std=c89`, `gnu89`, `c99`, `gnu99`, `c11`, `gnu11`, `gnu17`; default `-std=gnu11` | own |
| [`lazytui`](lazytui) | Terminal UI library on plain ANSI escapes | C99: `-std=c99 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE` | own |
| [`ragdb`](ragdb) | Embedded note/vector database, IVF + HNSW, hybrid search | C89: `-std=c89 -pedantic -Wall -Wextra -Werror`; its sanitizer, fuzz and crash tools use `-std=gnu99` | own |
| [`fzf`](fzf) | Fuzzy finder compatible with fzf 0.74's command line | C11 GNU: `-std=gnu11` | own, not a fork of the Go fzf |
| [`notcoreutils`](notcoreutils) | `cb cloc colorado dv man netcat sudo tgates tree` | C11 GNU: `-std=gnu11 -D_GNU_SOURCE` | own |
| [`gtop`](gtop) | System monitor TUI (CPU/GPU, RAM, disk, net, processes) | C99: `-std=c99` plus POSIX.1-2008 (set in the source), links `-lm` | own, C port of an earlier Rust version |
| [`dwl`](dwl) | Wayland compositor | C99 features; no `-std` flag, built with the compiler default (wlroots' headers need C11) | fork of [dwl](https://codeberg.org/dwl/dwl) |
| [`dmenu`](dmenu) | Launcher, native Wayland | C99: `-std=c99 -pedantic`; `dmenu_menu.c` is `-std=gnu11` | fork of [dmenu](https://git.suckless.org/dmenu) (`wayland` branch) |
| [`lemonbar`](lemonbar) | Status bar, native Wayland | C99: `-std=c99 -D_GNU_SOURCE` | fork of [lemonbar](https://github.com/LemonBoy/bar) (`wayland` branch) |
| [`nnn`](nnn) | File manager | C11: `-std=c11` | fork of [nnn](https://github.com/jarun/nnn) (`debloat` branch) |

The "language" column is what each directory's own Makefile or `run` script passes to the
compiler, not a claim about what else would compile.

Every directory builds on its own; there is no top-level build. Each has its own README with
the details, this page is the map.

## Quick start

```sh
git clone https://github.com/Gucixdev/holyc && cd holyc

# libraries: tests run from a plain `cc`, no dependencies beyond libc, libm and pthreads
(cd base && make test)            # 130 test programs
(cd base && make bench)           # 5 benchmarks against malloc / naive loops
(cd lazytui && make test)         # widgets, layout, solver, fuzzed input
(cd ragdb && ./run)               # self-tests, fuzzers, thread and crash-safety tests

# tools
(cd fzf && make && make test)     # 413 tests, differential against /usr/bin/fzf when present
(cd notcoreutils && make && make test)   # 57 tests; binaries land in bin/
(cd gtop && make && make test)           # 37 checks against the live system
```

## What each piece is

### base
The single-file library. Include what you need and define `<NAME>_IMPLEMENTATION` in one
translation unit (stb style). Seven areas, 107 headers, every one with a test:

- **core**: `base.h` (constants, fixed-width and fixed-point types, `Option`/`Result`/`TRY`/
  `Future`/`DEFER`, own `str`/`String_View`/`Arena_String`/`print`), the arena and slab, a
  fixed-chunk pool and file-backed arena and pool, reference counting, a job pool with a task
  graph, MPMC and SPSC queues, priority queue, coroutines, state machine, streams, `io`, `log`,
  `args`, `flags`, `utf8`.
- **data**: array, list, tree, slot map, trie, suffix array, string interner, lookup table, and
  hashing: hash table/map/set, MurmurHash3, XXH32, rolling hash, perfect hash.
- **math**: vectors, matrices, quaternions, curves, easing, noise (simplex, Perlin, Voronoi,
  blue), fractals, PCG32 and xorshift, distributions, statistics, and **balanced ternary logic**
  (`tenary_gates.h`: min/max/neg/diff over -1, 0, +1).
- **codec**: deflate, PNG, JPEG, QOI, WAV, MP3, BDF, JSON, CSV, INI, tar, base64/base63, and
  **small-radix packing** (`tenary.h`: 4:1 and 5:1 packing of 2- and 3-level symbols, delta +
  zigzag + Golomb-Rice).
- **crypto**: AES-256, SHA-256, Merkle tree with proofs.
- **net**: TCP sockets, HTTP/1.1 client and server, IPv4 helpers, stream pipe.
- **util**: paths, virtual filesystem, config, dates, units, UUIDv4, fuzzy match, expression
  tree + formula evaluator + bytecode VM, checked arithmetic, profiler, single-instance lock.

The module-by-module table is in [`base/README.md`](base/README.md). Ported code is credited in
[`base/THIRD_PARTY.md`](base/THIRD_PARTY.md) with the upstream licence texts.

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
Nine small tools, one source file and one man page each: `cb` (X11 clipboard manager), `cloc`
(lines of code per language), `colorado` (log colorizer), `dv` (disk usage, biggest first), `man`
(manual-page viewer), `netcat` (TCP pipe), `sudo` (doas-style, not installed setuid), `tgates`
(ternary logic gates on the command line) and `tree` (`tree(1)`-compatible). Things GNU
coreutils already does well are not reimplemented. See [`notcoreutils/README.md`](notcoreutils/README.md).

### gtop
A system monitor for the terminal in C99, libc and libm only, reading `/proc` and `/sys`
directly: CPU with per-core frequency and temperature, RAM split into used/zram/cache/swap, GPU
(NVIDIA, amdgpu, Intel), disks with a partition tree, network, and a process table with three
views, a collapsible process tree per program, multi-select, kill and a command palette. `gtop --snapshot` prints a frame as plain text,
which the 37 checks in `tests/run` use. It is a 1:1 port of an earlier Rust/ratatui version.

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
| gtop | a C compiler and make; optional `nvidia-smi`, `intel_gpu_top`, `lsusb` for GPU and TPU info |
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
