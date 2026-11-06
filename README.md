# holyc

A collection of C tools and libraries, plus the Wayland desktop pieces I run them with. The name
is a nod, not a claim: this has nothing to do with TempleOS's HolyC language, it is plain C
(C89 to C11, see the table for which) and nothing else.

| Directory | What it is | Language, exactly | Origin |
|---|---|---|---|
| [`base`](base) | Single-header C library: 107 modules (types, allocators, queues, data structures, math, codecs, crypto, net) | C89 style, one `long long` beyond C89; tests pass with `-std=c89`, `gnu89`, `c99`, `gnu99`, `c11`, `gnu11`, `gnu17`; default `-std=gnu11` | own |
| [`lazytui`](lazytui) | Terminal UI library on plain ANSI escapes | C99: `-std=c99 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE` | own |
| [`notcoreutils`](notcoreutils) | `cb cloc colorado dv fzf gtop man netcat sudo tgates tree`, one file each | C11 GNU: `-std=gnu11 -D_GNU_SOURCE` | own |
| [`vecindex`](vecindex) | Vector database: rotated 4/2/1-bit fast-scan with exact re-scoring, IVF, bit-packed quantizers, BM25 hybrid search; a C API in one library and `vserv`, an epoll server with TLS | C89: `-std=c89 -pedantic`; the sanitizer, crash and server tests `-std=gnu99` | own |
| [`dwl`](dwl) | Wayland compositor | C99 features; no `-std` flag, built with the compiler default (wlroots' headers need C11) | fork of [dwl](https://codeberg.org/dwl/dwl) |
| [`dmenu`](dmenu) | Launcher, native Wayland | C99: `-std=c99 -pedantic`; `dmenu_menu.c` is `-std=gnu11` | fork of [dmenu](https://git.suckless.org/dmenu) (`wayland` branch) |
| [`lemonbar`](lemonbar) | Status bar, native Wayland | C99: `-std=c99 -D_GNU_SOURCE` | fork of [lemonbar](https://github.com/LemonBoy/bar) (`wayland` branch) |
| [`nnn`](nnn) | File manager | C11: `-std=c11` | fork of [nnn](https://github.com/jarun/nnn) (`debloat` branch) |

The "language" column is what each directory's own Makefile or `run` script passes to the
compiler, not a claim about what else would compile.

The vector database that lived here as `ragdb/` was its own repository,
[vecindex](https://github.com/Gucixdev/vecindex), from 2026-10-17 to 2026-11-01 and is back as `vecindex/`, history
included. The agent layer that grew on top of it is `vecindex/examples/agent_launcher/`, which uses `vecindex/` as a library.

Every directory builds on its own; there is no top-level build. Each has its own README with
the details, this page is the map. `tools/` holds three repository scripts: `runall` runs every project's tests and
`lean4/run` and prints one table (`--full` adds the benchmarks and vecindex's whole `./run all`, fuzzing and
coverage; logs in `.runall/`), `dirprefix` names the files in a folder's subfolders after their folder and rewrites
what names them (base's headers went through it), `leaninit` starts a project's `lean4/` folder.

## Quick start

```sh
git clone https://github.com/Gucixdev/holyc && cd holyc

# libraries: tests run from a plain `cc`, no dependencies beyond libc, libm and pthreads
(cd base && make test)            # 130 test programs
(cd base && make bench)           # 5 benchmarks against malloc / naive loops
(cd lazytui && make test)         # widgets, layout, solver, fuzzed input

# tools
(cd notcoreutils && make && make test)   # its tests, fzf's (differential against /usr/bin/fzf when present)
                                         # and gtop's (against the live system); binaries land in bin/

# vector database, and the agent layer built on it
(cd vecindex && ./run)             # self-tests, sanitizers, kill -9, the server end to end
(cd vecindex && ./run all DATA)    # all of that, stress, the cross-built tests and every benchmark
(cd vecindex/examples/agent_launcher && ./run)
```

## What each piece is

### base
The single-file library. Include what you need and define `<NAME>_IMPLEMENTATION` in one
translation unit (stb style). Seven areas, 107 headers, every one with a test:

- **core**: `core_base.h` (constants, fixed-width and fixed-point types, `Option`/`Result`/`TRY`/
  `Future`/`DEFER`, own `str`/`String_View`/`Arena_String`/`print`), the arena, a
  fixed-chunk pool and file-backed arena and pool, reference counting, a job pool with a task
  graph, MPMC and SPSC queues, priority queue, coroutines, state machine, streams, `io`, `log`,
  `args`, `flags`, `utf8`.
- **data**: array, list, tree, slot map, trie, suffix array, string interner, lookup table, and
  hashing: hash table/map/set, MurmurHash3, XXH32, rolling hash, perfect hash.
- **math**: vectors, matrices, quaternions, curves, easing, noise (simplex, Perlin, Voronoi,
  blue), fractals, PCG32 and xorshift, distributions, statistics, and **balanced ternary logic**
  (`math_tenary_gates.h`: min/max/neg/diff over -1, 0, +1).
- **codec**: deflate, PNG, JPEG, QOI, WAV, MP3, BDF, JSON, CSV, INI, tar, base64/base63, and
  **small-radix packing** (`codec_tenary.h`: 4:1 and 5:1 packing of 2- and 3-level symbols, delta +
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

### fzf (notcoreutils/fzf.c)
A from-scratch fzf in C (libc and pthreads only): the FuzzyMatchV2 scoring, extended search
syntax, `--bind` grammar and actions, preview windows, `--listen`, mouse, borders. Not done
(listed in its README): `--gap`, `--tail`, sixel/kitty previews, `--tmux`, shell key-binding
scripts.

### notcoreutils
Eleven small tools, one source file each (a man page for the first nine), fzf and gtop among them: `cb` (X11 clipboard manager), `cloc`
(lines of code per language), `colorado` (log colorizer), `dv` (disk usage, biggest first), `man`
(manual-page viewer), `netcat` (TCP pipe), `sudo` (doas-style, not installed setuid), `tgates`
(ternary logic gates on the command line) and `tree` (`tree(1)`-compatible). Things GNU
coreutils already does well are not reimplemented. See [`notcoreutils/README.md`](notcoreutils/README.md).

### gtop (notcoreutils/gtop.c)
A system monitor for the terminal in C99, libc and libm only, reading `/proc` and `/sys`
directly: CPU with per-core frequency and temperature, RAM split into used/zram/cache/swap, GPU
(NVIDIA, amdgpu, Intel), disks with a partition tree, network, and a process table with two
views (clean and raw), an always-open process tree per program split into user and system, an info panel that explains the selected process or service, multi-select, kill and a command palette. `gtop --snapshot` prints a frame as plain text,
which the 46 checks in `notcoreutils/tests/gtop` use. It started as a 1:1 port of an earlier Rust/ratatui version; the process tree and the new PROC columns came after.

### vecindex
A vector database in C89: a TurboQuant-style rotated 4-bit (2-bit, or 1-bit in front of the 4-bit) fast-scan with
exact re-scoring, IVF with self-tuning and live indexing, rows past RAM in column chunks, BM25 hybrid search, write
batches, tenants, a journal with background snapshots. It runs inside a program through its C API or behind `vserv`.
See [`vecindex/README.md`](vecindex/README.md).

### vecindex/examples/agent_launcher
Notes and memory for LLM agents, on vecindex's store: `vecdb.c` (notes, `[[wikilinks]]`, tags, BM25 + vector hybrid
search, MCTS over the link graph, embedding servers, a command line), `mem.c` (agent memory: lifetime, tiers, a rule
engine) and `rerank.c` (a cross-encoder second pass). It includes `../vecindex/src/store.c` directly.
See [`agent_launcher/README.md`](vecindex/examples/agent_launcher/README.md).

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

## Proofs

Proofs about the C code are written in Lean 4. Every project has a `lean4/` folder at its root whose `run` builds
the proofs, checks that no theorem rests on an axiom beyond Lean's `propext`, `Classical.choice` and `Quot.sound`,
and scans the sources for `sorry` and `native_decide`; `tools/leaninit PROJECT` starts one.
[`vecindex/lean4`](vecindex/lean4/README.md) holds the proofs so far and shows the method: a Lean model of each C
function, theorems about the model, and a differential test that holds the C code to the model. The other folders
are empty so far and build on core Lean in seconds; vecindex's needs Mathlib.

## Requirements

| Piece | Needs |
|---|---|
| base, lazytui, notcoreutils | a C compiler (gcc or clang), make, Linux |
| vecindex | a C compiler and bash; python3 for the server's end-to-end tests; numpy and the compared libraries for `./run compare` |
| agent_launcher | a C compiler, bash and `vecindex/` (`VECINDEX` points at another copy); python3 with torch only for the optional model servers |
| gtop (in notcoreutils) | optional `nvidia-smi`, `intel_gpu_top`, `lsusb` for GPU and TPU info |
| dwl | wlroots 0.20, wayland, xkbcommon, pkg-config |
| dmenu, lemonbar | wayland-client, xkbcommon, fontconfig, freetype2 |
| nnn | ncurses; readline support is off by default (`O_NORL := 1` in its Makefile) |
| every `lean4/` | elan with `leanprover/lean4:v4.26.0`; vecindex's also Mathlib, TrustLean and AMO-Lean, which lake fetches |

The code is developed and tested on Linux (x86-64). Other platforms are not tested.

## Repository layout and history

This repository is where the pieces are published together. The five forked or standalone
subtrees (`dwl`, `dmenu`, `lemonbar`, `nnn`, and `gtop` until it moved) keep their own upstream history; `base`,
`lazytui`, `fzf` and `notcoreutils` start here. fzf and gtop moved into `notcoreutils` as one file each on
2026-11-06; their old folders are in `trash/`. `vecindex` brought its history back with `git subtree add`;
`git subtree split --prefix=vecindex` gives the standalone repository's commits again. Issues and patches welcome; run the
directory's tests before sending one.

## Licence

My own code (`base`, `lazytui`, `notcoreutils` with fzf and gtop) is MIT, see
[`LICENSE`](LICENSE). The forks keep the licences of their upstreams, in each directory:
`dwl` (GPLv3 with dwm/sway/tinywl notices), `dmenu` (MIT/X), `lemonbar` (MIT), `nnn` (BSD
2-Clause). Third-party code ported into `base` is listed with its licence in
[`base/THIRD_PARTY.md`](base/THIRD_PARTY.md).
