# base

A library of single-file C headers: 107 modules, 130 test programs, 5 benchmarks. Types, memory
allocators, queues and job pools, data structures, hashing, math and noise, binary and text
formats, crypto, networking, and the small utilities that keep coming up.

- **Single file per module.** Include the header you need. The code of a module is compiled in by
  defining its `<NAME>_IMPLEMENTATION` macro in exactly one translation unit (the stb convention).
- **No build system needed.** Compile with `-Isrc`; link `-lm -lpthread` when a module uses them.
- **Every module has a test**, and what a header does not do is stated at its top.

```c
#define ARENA_ALLOC_IMPLEMENTATION    // once, in one .c file
#include "core/arena_alloc.h"

Arena a = {0};
void *p = arena_alloc(&a, 256);       // a pointer add; freed all at once
Arena_Mark m = arena_snapshot(&a);    // ... temporaries ...
arena_rewind(&a, m);
arena_free(&a);
```

## Which C

The code is written in C89 style (declarations at the top of blocks, no `//` comments in the
headers, no VLAs, no designated initialisers) with one exception that is beyond C89: 64-bit
integers are `long long`. Measured with GCC 16.2, all 130 tests compile and pass with
`-std=c89`, `-std=gnu89`, `-std=c99`, `-std=gnu99`, `-std=c11`, `-std=gnu11` and `-std=gnu17`
(`CSTD=-std=c89 sh tests/run`). `-pedantic` flags only the `long long` constants. The default for
`make test` is `-std=gnu11 -D_GNU_SOURCE`. Linux only: several modules use `mmap`, `pthreads`,
`flock`, `write(2)`.

## Layout

```
src/core/     base.h, allocators, job pool and queues, coroutines, streams, io, log, args, flags, utf8
src/data/     array, list, tree, slot map, trie, suffix array, interner, lookup table; data/hash/
src/math/     vectors/matrices/quaternions, trig, curves, easing, noise, RNGs, statistics, ternary logic
src/codec/    deflate, png, jpeg, qoi, wav, mp3, bdf, json, csv, ini, tar, base64, base63, ternary packing
src/crypto/   aes256, sha256, merkle tree
src/net/      sockets, http client and server, ipv4 helpers, stream pipe
src/util/     paths, vfs, config, dates, units, uuid, fuzzy match, expression tree + formula + vm, profiler...
tests/        one program per module: tests/test_NAME.c
bench/        benchmarks
THIRD_PARTY/  licence texts of the code some headers are ported from, see THIRD_PARTY.md
```

`make test` (or `sh tests/run [filter]`) builds and runs every test as its own program, in
parallel. `make bench` runs `bench/`: the arena against glibc malloc, the pool against
malloc/free, the job pool against a thread per job, the hash table and Morton encoding against
naive versions.

## What is in it, module by module

### core/

| header | what it is |
|---|---|
| `base.h` | **the base header**: constants (`HOLYC_PI`, `TAU`, `E`, `SQRT2`, golden ratio, deg/rad); fixed-width types `i8..i64 u8..u64 f32 f64`, fixed-point `fp16/fp32/fp64` (Q formats), byte/word array views `a8..a64`, bit tricks, FNV-1a, `memset_explicit`, x86 SSE/AVX sums, dots, min and max; `Option`/`Result`/`TRY`/`Future`/`DEFER`/`Async_Signal` (tagged unions you take apart with a `switch`, no hidden control flow); `str_*`/`mem_*` (own replacements for the `<string.h>` functions the library needs) and `String_View` (chop, trim, split, compare, parse numbers; from tsoding's sv); `Arena_String` (growable text on an arena); `print_*` (output with `write(2)` and own integer conversion, no `printf`) |
| `contracts.h` | `HOLYC_REQUIRES`/`ENSURES`/`INVARIANT` design-by-contract macros: ordinary trap-based assertions in a normal build, and under CBMC (a bounded model checker) the same macros become assumptions and proof obligations |
| `arena_alloc.h` | the arena (port of tsoding's arena.h): linked regions, mmap/malloc/flat backend, aligned alloc, `realloc`, `strdup`/`memdup`/`sprintf`, snapshot/rewind/reset/trim, growable-array and string-builder macros; plus `arena_calloc`, `arena_used`, `arena_reserved` and `Slab` (growable pool of same-sized blocks, zeroed, rejects a double release) |
| `pool_alloc.h` | fixed-chunk free-list pool over a buffer you give it: O(1) alloc and free, no malloc, no fragmentation |
| `persistant_arena.h` | bump arena in an `mmap(MAP_SHARED)` file: the content and the "how much is used" both survive a restart |
| `persistant_pool.h` | the same pool, file-backed: chunk content and which chunks are allocated survive a restart |
| `ref_count.h` | manual reference counting: `rc_init` with an on-zero callback, `rc_retain`, `rc_release`, `rc_count` |
| `fine_grained.h` | worker thread pool and task graph: workers claim disjoint chunks of an index range with one atomic, no locks around shared state; DAG levels; benchmarked against a thread per job |
| `action_queue.h` | bounded multi-producer multi-consumer queue of `{fn, ctx}` actions (mutex + condvar), blocking push/pop |
| `ring_buffer.h` | lock-free single-producer single-consumer ring buffer (from c89atomic) |
| `priority_queue.h` | binary-heap priority queue for jobs that must run in order of importance |
| `event_queue.h` | tick-stamped events for deterministic recording and replay |
| `corountines.h` | stackless coroutines via protothreads (a switch/case jump table) |
| `fsm.h` | finite state machine: integer states, transitions, enter/exit callbacks |
| `chunk.h`, `stream.h`, `static.h`, `dynamic.h` | the data-flow pieces: a bounded `Chunk`; an unbounded `Stream` built from a chain of chunks; `static` is a fixed buffer where a write is all-or-nothing, `dynamic` the growable flow |
| `io.h` | read a whole file into memory, write a buffer to a file |
| `log.h` | levelled logger (from rxi's log.c), no printf-style varargs: you build the message first |
| `args.h` | declarative command-line parser: a table of flags and options, fixed-capacity result, no allocation |
| `flags.h` | the four flags every CLI wants (verbose, quiet, no-color, dry-run) from environment variables with a prefix and from argv |
| `utf8.h` | decode/encode one codepoint, count codepoints, validate a buffer |
| `xmacro.h` | X-macro helpers: one list generates an enum, a name table and a dispatcher |

### data/

| header | what it is |
|---|---|
| `array.h` | type-erased growable array, or a static one over your own buffer; swap-remove |
| `list.h` | intrusive doubly linked list with arena-allocated nodes |
| `tree.h` | generic N-ary tree, arena nodes, `void*` payload |
| `slot_map.h` | fixed-capacity container with stable indices (a handle stays valid after a removal) |
| `prefix.h` | trie over byte strings (256-way, arena nodes) |
| `suffix.h` | suffix array over a text buffer with substring search (O(n² log n) construction) |
| `token_table.h` | string interner: the same string always gives the same dense integer id |
| `lut.h` | lookup table built once from a generator function (sin table, gamma curve, CRC table) |
| `tag.h` | string-key to typed-value metadata bag on top of the hash table |
| `scaffold.h` | `SCAFFOLD_ARRAY_DEFINE(T, Name)`: one macro line for a typed growable array with push/get/clear |
| `mapops.h` | filter / fold / foreach over the type-erased `Array` |
| `merge_ops.h` | the merge step of merge sort for two sorted `Array`s, and merging one hash map into another (overwrite or keep existing) |
| `hex.h` | bytes to lowercase hex and back |
| `hash/hashtable.h` | generic hash table: open addressing, triangular probing, FNV-1a, tombstones, resize at 70% load |
| `hash/hashmap.h` | insertion-ordered map on the hash table |
| `hash/hashset.h` | set of byte keys on the hash table |
| `hash/marmur.h` | MurmurHash3, x86 32-bit |
| `hash/tinyxxhash.h` | XXH32 |
| `hash/rollhash.h` | rolling polynomial hash (Rabin-Karp): O(1) update as a window slides |
| `hash/perfecthash.h` | minimal perfect hash for a fixed key set (hash, displace and compress) |

### math/

| header | what it is |
|---|---|
| `mathx.h` | `vec2/vec3/vec4`, quaternion, 4x4 matrix |
| `linear_algebra.h` | transparent re-export of the above |
| `trigonometry.h` | degrees, radians and turns |
| `fp_math.h` | `sqrt` for the fixed-point types in `base.h` |
| `curve.h` | Bezier, Catmull-Rom, Hermite, B-spline, Morton (Z-order) codes |
| `easing.h` | lerp, smoothstep, quaternion slerp/nlerp, Penner easing curves |
| `noise.h` | blue noise, fBm, OpenSimplex2 simplex, Voronoi, from FastNoiseLite |
| `gradient.h` | classic grid Perlin noise (a different family from the simplex noise) |
| `fractal_math.h` | Mandelbrot and Julia escape time, iterated function systems, box counting |
| `rng.h` | PCG32 (XSH-RR), the default generator |
| `pcg.h` | PCG32 `pcg32_random_r` reference variant |
| `xorshift.h` | xorshift32 |
| `distribution.h` | non-uniform sampling from uniform samples you pass in: Gaussian (Box-Muller) and exponential, decoupled from any generator |
| `mixture.h` | weighted blend of named components (a weighted average, not a selection) |
| `mean.h`, `variance.h` | mean; population variance and standard deviation, independent of each other |
| `extras.h` | ratios: simplify to lowest terms, from width and height (1920x1080 gives 16:9), to float |
| `tenary_gates.h` | **balanced ternary logic** over `Trit` in {-1, 0, +1}: `tmin`/`tmax` are Kleene AND/OR, `tneg` is NOT, `tdiff` is the three-way compare; array forms that may work in place. Used by `notcoreutils/tgates` |

### codec/

| header | what it is |
|---|---|
| `tenary.h` | **small-radix packing and prediction**: pack N symbols of radix r into as few bytes as possible (`r^N <= 256`: 4 levels give 4:1, 3 levels / ternary give 5:1, the BitNet b1.58 trick), plus delta, zigzag and Golomb-Rice coding for smooth sequences |
| `base63.h` | base63: 10 digits, 26 upper, 26 lower and `-` as the 63rd symbol |
| `base64.h` | RFC 4648 base64, encode and decode |
| `deflate.h` | DEFLATE and zlib decompressor (from stb_image) |
| `png_dec.h` | PNG decoder: 8-bit, non-interlaced (gaps are documented in the header) |
| `jpeg_dec.h` | baseline JPEG decoder, grayscale and YCbCr (progressive is a documented gap) |
| `qoi_codec.h` | QOI encoder and decoder, from the spec |
| `img.h` | image loader that sniffs the magic bytes and dispatches to PNG, JPEG or BMP |
| `wav.h` | WAV (RIFF/PCM) reader and writer, from scratch |
| `mp3_dec.h` | MP3 decoder (from dr_mp3) |
| `bdf_dec.h` | BDF bitmap font parser |
| `json.h` | RFC 8259 JSON parser (objects, arrays, all escapes, numbers as f64), parser only |
| `csv.h` | RFC 4180 CSV parser; unquoted fields are views into the input |
| `ini_dec.h` | INI parser that works in place (from rxi/ini) |
| `tinytar.h` | POSIX ustar reader and writer with a pluggable stream backend (from rxi/microtar) |
| `serialize.h` | binary reader and writer over a buffer: little-endian integers, floats, byte runs |

### crypto/

| header | what it is |
|---|---|
| `aes256.h` | AES-256 (FIPS-197) from scratch: key expansion, block encrypt/decrypt, CBC with PKCS#7 |
| `sha256_hash.h` | SHA-256 (FIPS 180-4), one shot |
| `merkletree.h` | Merkle tree over SHA-256 with proofs: verify a block against the root without the whole data set |

### net/

| header | what it is |
|---|---|
| `socket.h` | thin POSIX TCP wrapper: connect, listen, accept, full read and write loops |
| `http.h` | HTTP/1.1 client over `socket.h` (Content-Length framing; chunked is documented) |
| `http_server.h` | HTTP/1.1 server: single-threaded, one request per connection, handler callback |
| `net.h` | IPv4 string to 4 bytes and back, 16/32-bit host/network byte order |
| `pipe.h` | producer/consumer close protocol on a `Stream`: "empty for now" versus "finished" |

### util/

| header | what it is |
|---|---|
| `path.h` | basename, dirname, extension, join, normalize, is_absolute (API shape of cwalk) |
| `exe_path.h` | path of the running executable (from whereami, Linux and FreeBSD) |
| `vfs.h` | virtual filesystem behind function pointers (read, write, exists): a real directory or an in-memory one |
| `config.h` | typed in-memory key-value config with defaults |
| `text.h` | split into pieces and whole-buffer transforms on top of `String_View` |
| `text_ops.h` | line, word and occurrence counting, split into fields, matching lines |
| `fuzzy_match.h` | fuzzy subsequence scoring (the FuzzyMatchV2 recurrence of fzf, written from scratch) |
| `uuid4.h` | random UUIDv4 (from rxi/uuid4), seeded from the OS |
| `date_util.h` | calendar arithmetic through Julian Day Numbers (weekday, add days, differences) |
| `time_util.h` | monotonic timing and duration formatting |
| `units.h` | length (mm..km) and weight (mg..t) conversion through one base unit |
| `cli.h` | getopt without global state (from xgetopt, public domain) |
| `ast.h` | generic expression-tree node: literal, identifier, unary, binary, call |
| `formula.h` | evaluates an `ast.h` tree against a variable context |
| `vm.h` | small stack-based bytecode VM: i64/f64 arithmetic, stack ops, compare, jump, call |
| `verify.h` | checked `add/sub/mul/div` for i32 and i64 and finite-float checks, returning a `Proof_Error` instead of overflowing |
| `profiler.h` | named-scope timing: begin/end per name, call counts and a summary |
| `pubsub.h` | observer: a fixed list of `(callback, ctx)` subscribers, emit in order |
| `raii.h` | scoped cleanup with `__attribute__((cleanup))` (GCC/Clang) |
| `single_instance.h` | one running copy of a program via `flock` on a lock file (atomic, no PID race) |
| `hardscan.h` | probes core count, cache sizes and measured memory bandwidth, recommends an arena chunk size |

## Not in it

Game-engine pieces (collision shapes, GJK, rigid bodies, particles, camera, ECS), terminal UI
layers (the terminal UI library is [`../lazytui`](../lazytui)), mesh and model formats, and a
neural-network header. They were part of an earlier, larger version of this library and were cut
because they did not have a general use outside a game or a TUI. The ECS lives in the
`gdtoolbox` repository.

## Design rules

These are what the headers themselves state, and they hold across the library:

- **No hidden control flow.** No RAII by default (`raii.h` is opt-in): a release is a function you
  call. `Option`/`Result` are tagged unions you take apart with a plain `switch`.
- **Recursion and virtual dispatch are avoided** (`corountines.h` and `ring_buffer.h` say why).
- **Own printing and string functions** where the library can do without libc: `print_*` writes
  with `write(2)`, `str_*`/`mem_*` replace the handful of `<string.h>` functions it needs.
- **Honest scope.** Every header lists what it does not do (PNG without Adam7, JPEG without
  progressive, MP3 ported rather than written, HTTP without chunked).

## Tests

`tests/` holds 130 programs. All compile and pass under every `-std` listed above. A module that
could not be compiled and verified is not here: it was left out rather than shipped unchecked.

## Credits and licences

The library is MIT licensed (see the repository root `LICENSE`). Some headers are ports of other
people's code; each is credited at the top of its header and listed with its licence in
[`THIRD_PARTY.md`](THIRD_PARTY.md).
