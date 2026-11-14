# holyc

A collection of C tools and libraries, plus the Wayland desktop pieces I run them with. The name
is a nod, not a claim: this has nothing to do with TempleOS's HolyC language, it is plain C
(C89 to C11, see the table for which) and nothing else.

| Directory | What it is | Language, exactly | Origin |
|---|---|---|---|
| [`core`](core) | Single-header C library: 107 modules (types, allocators, queues, data structures, math, codecs, crypto, net) | C89 style, one `long long` beyond C89; tests pass with `-std=c89`, `gnu89`, `c99`, `gnu99`, `c11`, `gnu11`, `gnu17`; default `-std=gnu11` | own |
| [`notcoreutils`](notcoreutils) | `cb cloc colorado dv fzf gtop man netcat sudo tgates tree`, one file each | C11 GNU: `-std=gnu11 -D_GNU_SOURCE` | own |
| [`desktop`](desktop) | Own Wayland desktop: compositor, launcher/menu, bar, settings panel, volume panel in one multi-call binary | C, wlroots 0.20 | built on dwl, dmenu and lemonbar (their licences in `desktop/src/LICENSES`) |
| [`nnn`](nnn) | File manager | C11: `-std=c11` | fork of [nnn](https://github.com/jarun/nnn) (`debloat` branch) |

The "language" column is what each directory's own Makefile or `run` script passes to the
compiler, not a claim about what else would compile.

Every directory builds on its own; there is no top-level build. Each has its own README with
the details, this page is the map. `tools/` holds three repository scripts: `runall` runs every project's tests and
`lean4/run` and prints one table (`--full` adds the benchmarks; logs in `.runall/`), `dirprefix` names the files in a folder's subfolders after their folder and rewrites
what names them (core's headers went through it), `leaninit` starts a project's `lean4/` folder.

## Quick start

```sh
git clone https://github.com/Gucixdev/holyc && cd holyc

# libraries: tests run from a plain `cc`, no dependencies beyond libc, libm and pthreads
(cd core && make test)            # 130 test programs
(cd core && make bench)           # 5 benchmarks against malloc / naive loops

# tools
(cd notcoreutils && make && make test)   # its tests, fzf's (differential against /usr/bin/fzf when present)
                                         # and gtop's (against the live system); binaries land in bin/

```

## What each piece is

### core
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

The module-by-module table is in [`core/README.md`](core/README.md). Ported code is credited in
[`core/THIRD_PARTY.md`](core/THIRD_PARTY.md) with the upstream licence texts.

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

### The Wayland desktop: desktop, nnn
The former dwl, dmenu and lemonbar forks live on inside [`desktop`](desktop) (see its docs/hacking.md).
- **nnn**: an arena allocator to fix an O(n²) `dentfill()`, plus opener scripts that open a GUI
  Emacs without blocking nnn. Release engineering of upstream was removed on purpose.

## Proofs

Proofs about the C code are written in Lean 4. Every project has a `lean4/` folder at its root whose `run` builds
the proofs, checks that no theorem rests on an axiom beyond Lean's `propext`, `Classical.choice` and `Quot.sound`,
and scans the sources for `sorry` and `native_decide`; `tools/leaninit PROJECT` starts one.
Each folder builds on core Lean in seconds; `core/lean4` is the example of the method: a Lean model of each C
function, theorems about the model, and a differential test that holds the C code to the model.

## Requirements

| Piece | Needs |
|---|---|
| core, notcoreutils | a C compiler (gcc or clang), make, Linux |
| gtop (in notcoreutils) | optional `nvidia-smi`, `intel_gpu_top`, `lsusb` for GPU and TPU info |
| nnn | ncurses; readline support is off by default (`O_NORL := 1` in its Makefile) |
| every `lean4/` | elan with `leanprover/lean4:v4.26.0` |

The code is developed and tested on Linux (x86-64). Other platforms are not tested.

## Repository layout and history

This repository is where the pieces are published together. The forked subtrees (`nnn` and the desktop's dwl, dmenu
and lemonbar parts) keep their upstream history; `core` and `notcoreutils` start here. Issues and patches welcome; run the
directory's tests before sending one.

## Licence

My own code (`core`, `notcoreutils` with fzf and gtop) is MIT, see
[`LICENSE`](LICENSE). The forks keep the licences of their upstreams, in each directory:
`dwl` (GPLv3 with dwm/sway/tinywl notices), `dmenu` (MIT/X), `lemonbar` (MIT), `nnn` (BSD
2-Clause). Third-party code ported into `core` is listed with its licence in
[`core/THIRD_PARTY.md`](core/THIRD_PARTY.md).
