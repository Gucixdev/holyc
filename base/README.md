# base

A library of single-file C headers: types, allocators, queues and jobs, data structures, math,
codecs, a few crypto primitives and the utilities that keep coming up. It holds the
80 modules that proved useful on their own, each with a test. `lazytui/` (a terminal UI
library) lives here too.

Everything is a header you include. The implementation of a module is pulled in by defining its
`<NAME>_IMPLEMENTATION` macro in exactly one translation unit (the stb convention); a few
modules also ship a `.c` file in `src/`.

```c
#define ARENA_SLAB_IMPLEMENTATION     // once
#define MEM_HOOK_IMPLEMENTATION       // once
#include "arena_slab.h"

hc_arena *a = hc_arena_new(1 << 20);
void *p = hc_arena_alloc(a, 256);     // a pointer add; freed all at once
```

## Layout

```
include/      the headers (include/hash/ holds the hash tables and hash functions)
src/          the few modules that are a .c file (pool_alloc.c, flags.c, ...)
tests/        one program per module: tests/test_NAME.c
bench/        benchmarks
lazytui/      terminal UI library (compiled, own Makefile and tests)
THIRD_PARTY/  licence texts of the code some headers are ported from, see THIRD_PARTY.md
```

`make test` (or `sh tests/run [filter]`) builds and runs every test on its own, in parallel.
`make bench` runs `bench/`: the arena against glibc malloc, the pool against malloc/free, the job
pool against spawning a thread per job, the hash table and Morton encoding against naive versions.
Add `-Iinclude -Iinclude/hash -Isrc` to compile your own code against it.

## What is in it

**Types and allocators**: `types`, `base`, `contracts`. `types` is Option, Result, TRY,
Future, DEFER and Async_Signal in one header, taken apart with a plain `switch`.
Allocators: `arena_alloc`, `arena_slab`, `mem_hook`, `persistant_arena`, `persistant_pool`, `pool_alloc`, `ref_count`.
`arena_slab` is a bump arena and a fixed-size slab on a replaceable allocator hook (`mem_hook`);
`arena_alloc` is a C89 arena; `pool_alloc` a fixed-size pool.

**Concurrency and queues**: `fine_grained`, `action_queue`, `event_queue`, `ring_buffer`, `priority_queue`, `corountines`, `fsm`, `pipe`, `socket`.
`fine_grained` is a worker pool that claims disjoint chunks of an index range with no locks around
shared state; `action_queue` is a bounded multi-producer multi-consumer queue; `ring_buffer` is
lock-free single producer/consumer; `event_queue` carries ticked events for deterministic replay.

**Data structures**: `array`, `list`, `tree`, `hashmap`, `hashset`, `hashtable`, `slot_map`, `marmur`, `tinyxxhash`, `hex`.

**Math**: `linear_algebra`, `trigonometry`, `mathx`, `fp_math`, `curve`, `easing`, `noise`, `rng`, `pcg`, `xorshift`, `mean`, `variance`, `constant`.

**Codecs and formats**: `deflate`, `png_dec`, `jpeg_dec`, `qoi_codec`, `wav`, `mp3_dec`, `bdf_dec`, `img`, `ini_dec`, `json`, `csv`, `tinytar`, `base64`.

**Crypto**: `aes256`, `sha256_hash`.

**I/O, text and utilities**: `io`, `stream`, `serialize`, `print`, `log`, `args`, `flags`, `str`, `utf8`, `text_ops`, `path`, `exe_path`, `vfs`, `config`, `date_util`, `time_util`, `units`, `fuzzy_match`, `uuid4`, `xmacro`.

**Network**: `http`, `http_server`, `net`.

## lazytui

`lazytui/` is a terminal UI library on plain ANSI escapes: Cassowary layout, spring animation,
a diff-rendered canvas, widgets (progress and pacman bars, charts, lists, trees, pickers, a world
map), themes and mouse input. Build and test it from its own directory (`make`, `make test`).

## Design rules

These are the rules the headers themselves state, and they hold across the library:

- **No hidden control flow.** No RAII: a release is a function you call. `Option`/`Result` are
  tagged unions you take apart with a plain `switch`.
- **Recursion and virtual dispatch are avoided** (`profiler.h`, `corountines.h` say why).
- **Own printing**: `print.h` writes with `write` and its own integer conversion instead of `printf`.
- **Math is written out** rather than pulled from libm where it can be.
- **Honest scope**: each header states what it does not do.

## Tests

`tests/` holds 97 programs, all of which compile and pass. A module that could not be
compiled and verified is not here: it was left out rather than shipped unchecked.

## Credits and licences

The library is MIT licensed (see the repository `LICENSE`). Some headers are ports of other
people's code; each is credited at the top of its header and listed with its licence in
[`THIRD_PARTY.md`](THIRD_PARTY.md).
