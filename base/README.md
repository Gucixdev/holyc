# base

A library of single-file C headers: types, allocators, queues and jobs, data structures, math,
codecs, a few crypto primitives and the utilities that keep coming up. It holds the
modules that proved useful on their own, each with a test.

Everything is a header you include. The implementation of a module is pulled in by defining its
`<NAME>_IMPLEMENTATION` macro in exactly one translation unit (the stb convention); a few
modules also ship a `.c` file next to the header (`core/pool_alloc.c`, `core/flags.c`).

```c
#define ARENA_ALLOC_IMPLEMENTATION    // once
#include "core/arena_alloc.h"

Arena a = {0};
void *p = arena_alloc(&a, 256);       // a pointer add; freed all at once
Arena_Mark m = arena_snapshot(&a);    // ... temporaries ...
arena_rewind(&a, m);
arena_free(&a);
```

## Layout

```
src/core/     base.h (types, str, print, constants), arena_alloc, pool, job pool, queues,
              coroutines, fsm, io, log, args, flags, utf8
src/data/     array, list, tree, slot_map, hex; data/hash/ has the hash tables and functions
src/math/     linear algebra, trig, curves, noise, rng, statistics
src/codec/    deflate, png, jpeg, qoi, wav, mp3, json, csv, tar, base64, serialize
src/crypto/   aes256, sha256
src/net/      http, http_server, net, socket, pipe
src/util/     path, exe_path, vfs, config, date/time, units, fuzzy_match, uuid4, text_ops
tests/        one program per module: tests/test_NAME.c
bench/        benchmarks
THIRD_PARTY/  licence texts of the code some headers are ported from, see THIRD_PARTY.md
```

`make test` (or `sh tests/run [filter]`) builds and runs every test on its own, in parallel.
`make bench` runs `bench/`: the arena against glibc malloc, the pool against malloc/free, the job
pool against spawning a thread per job, the hash table and Morton encoding against naive versions.
Compile with `-Isrc` and include as `"core/arena_alloc.h"`, `"core/base.h"`, ...

## What is in it

**`core/base.h`** is the one base header: constants, fixed-width types, Option/Result/TRY/Future/
DEFER/Async_Signal (taken apart with a plain `switch`), `str` and `String_View`, `Arena_String` and
`print`. Each part keeps its own `*_IMPLEMENTATION` macro (`BASE_TYPE_`, `TYPES_`, `STR_`,
`STRING_VIEW_`, `ARENA_STRING_`, `PRINT_`). `contracts` sits beside it.

**Allocators**: `arena_alloc` is the arena (regions, mark/rewind, realloc, sprintf, growable
arrays and string builders; plus a growable `Slab` of fixed-size blocks with double-release
detection). `pool_alloc` is a fixed-buffer pool, `persistant_arena`/`persistant_pool` the file-backed
variants, `ref_count` reference counting.

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
