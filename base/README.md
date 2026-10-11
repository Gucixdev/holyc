# base

A library of single-file C headers: types, allocators, queues and jobs, data structures, math,
codecs, a few crypto primitives, and building blocks for games and terminal UIs. Everything is a
header you include; the implementation of a module is pulled in by defining its
`<NAME>_IMPLEMENTATION` macro in exactly one translation unit (the stb convention). A handful of
modules also ship a `.c` file in `src/`.

```c
#include "arena_alloc.h"          // include/ is on the include path

#define TYPES_IMPLEMENTATION      // once, for Async_Signal
#include "types.h"
```

## Layout

```
include/      the headers (include/hash/ holds the hash tables and hash functions)
src/          the few modules that are a .c file (pool_alloc.c, flags.c, ...)
tests/        one program per module: tests/test_NAME.c
bench/        benchmarks
THIRD_PARTY/  licence texts of the code some headers are ported from, see THIRD_PARTY.md
```

`make test` (or `sh tests/run [filter]`) builds and runs every test on its own, in parallel.
`make bench` runs `bench/`: the arena against glibc malloc, the pool against malloc/free, the job
pool against spawning a thread per job, the hash table and Morton encoding against naive versions.
On the author's machine the arena is about 2.7x faster than malloc, the pool about 10x and the job
pool about 8x; run it yourself, the numbers depend on the machine.
Add `-Iinclude -Iinclude/hash -Isrc` to compile your own code against it.

## What is in it

**Core** (`abstract`): `types` (Option, Result, TRY, Future, DEFER, Async_Signal, fixed-width ints),
`base`, `arena_alloc`, `persistant_arena`, `persistant_pool`, `src/pool_alloc.c`, `ref_count`,
`ring_buffer` (lock-free single producer/consumer), `fine_grained` (a worker pool that claims disjoint
chunks of an index range, no locks around shared state), `action_queue` (bounded multi-producer
multi-consumer queue of actions), `event_queue` (ticked events for deterministic replay),
`priority_queue`, `corountines` (stackless coroutines), `fsm`, `fabric`, `architect`, `args`, `flags`,
`log`, `print`, `io`, `socket`, `stream`, `serialize`, `contracts`, `verify`, `xmacro`.

**Data structures**: `array`, `list`, `tree`, `hashmap`, `hashset`, `hashtable`, `slot_map`,
`spatialhash`, `perfecthash`, `rollhash`, `merkletree`, `pubsub`, `prefix`, `suffix`, `token_table`,
`ast`, `formula`, `hex`, hash functions (`marmur` = MurmurHash3, `tinyxxhash` = XXH32).

**Math**: `linear_algebra`, `trigonometry`, `mathx`, `fp_math` (fixed point), `curve`, `easing`,
`gradient`, `noise`, `intersect`, `distribution`, `mean`, `variance`, `rng`, `pcg`, `xorshift`,
`fractal_math`, `vat`.

**Codecs and formats**: `deflate` (zlib), `png_dec`, `jpeg_dec`, `qoi_codec`, `wav`, `mp3_dec`,
`webp` (header reader), `bdf_dec`, `ini_dec`, `json`, `csv`, `svg`, `obj`, `stl`, `fbx`, `tinytar`,
`base64`, `img`.

**Crypto**: `aes256` (with CBC), `sha256_hash`.

**Utilities**: `utf8`, `str` (String_View), `text_ops`, `path`, `exe_path`, `vfs`, `config`,
`date_util`, `time_util`, `units`, `fuzzy_match` (fzf's FuzzyMatchV2 scoring), `uuid4`, `lut`,
`merge_ops`, `single_instance`, `vm`.

**Game building blocks**: `ecs`, `aabb_collision`, `obb`, `gjk`, `broadphase`, `raycast`, `rigidbody`,
`verlet`, `particle`, `camera`, `geometry`, `blackboard`, `btree`, `nn` (a small neural network).

**Terminal UI**: `tui`, `ansi_escape`, `color`, `font`, `layout`, `node`, `piemenu`, `sequencer`,
`cassowara` (constraint solver), `immediate` (immediate-mode GUI), `reactive`.

**Network**: `http` (client), `http_server`, `net`.

## Design rules

These are the rules the headers themselves state, and they hold across the library:

- **No hidden control flow.** No RAII: a release is a function you call. `Option`/`Result` are
  tagged unions you take apart with a plain `switch`.
- **Recursion and virtual dispatch are avoided**, not just discouraged (`profiler.h`,
  `corountines.h` say why).
- **Own printing**: `print.h` writes with `write` and its own integer conversion instead of `printf`.
- **Math is written out** rather than pulled from libm; five headers (`fractal_math`, `cassowara`,
  `easing`, `piemenu`, `ansii_piemenu`) still include `<math.h>`.
- **Data-oriented** where it matters: structure of arrays rather than array of structures (`ecs.h`).
- **Honest scope**: each header states what it does not do.

## Tests

`tests/` holds 191 programs, all of which compile and pass. A module without a test is not in this
directory: modules that could not be compiled and verified yet were left out rather than shipped
unchecked.

## Credits and licences

The library is MIT licensed (see the repository `LICENSE`). Some headers are ports of other
people's code; each is credited at the top of its header and listed with its licence in
[`THIRD_PARTY.md`](THIRD_PARTY.md).
