# The C API

`include/vecindex.h` is the whole interface: one opaque handle per store, plain C types, a status or a count from
every call. `./run` builds `.build/libvecindex.so` and `.build/libvecindex.a` from `src/vecindex.c`; the shared
library exports the `vi_*` functions and nothing else (it is built with `-fvisibility=hidden`, and `./run` checks
the exported symbols). Any language with a C foreign-function interface binds it without a compiler step of its
own. The examples below were run against the library; each prints

```
1 -0.012
2 -1.779
```

Scores are `-|q - v|^2` of the unit vectors: 0 is the same direction, -4 the opposite one. Every call may be made
from any thread on the same handle at once.

## Functions

| function | what it does |
|---|---|
| `vi_open(path, dim, flags)` | opens the store in `path`, or creates it with `dim` dimensions; `path` NULL: in memory; `flags` `VI_LIVE` indexes added rows as they come |
| `vi_close(db)` | flushes the journal to the disk and frees the handle |
| `vi_add(db, id, vec)`, `vi_add_batch(db, n, ids, vecs)` | adds rows, or replaces the row with that id (one change: a crash leaves the old row or the new one); a vector with a NaN or an infinity is refused |
| `vi_delete(db, id)` | removes the row with this id |
| `vi_delete_range(db, lo, hi)` | removes every row with an id in `[lo, hi]`: all the chunks of a document when ids are `doc << 20 \| chunk` |
| `vi_get(db, id, out)` | the stored, unit-normalised vector |
| `vi_add_text(db, id, vec, text, len)` | `vi_add` with a text kept beside the row (any bytes, up to 16 MB - 1; `len` -1: a C string) |
| `vi_get_text(db, id, buf, cap)` | the row's text: its length, the first `cap` bytes copied into `buf` |
| `vi_range(db, lo, hi, ids, max)` | the ids in `[lo, hi]` that have a row, ascending: the chunks of a document |
| `vi_search_hybrid(db, q, text, tlen, k, ids, scores)` | the vector and the words (BM25 over the texts), fused by rank; either side may be left out |
| `vi_set_tag`, `vi_get_tag` | a 64-bit tag per row |
| `vi_search(db, q, k, ids, scores)` | the k nearest rows, best first |
| `vi_search_tagged(db, q, k, all, any, ids, scores)` | the same over rows whose tag has every bit of `all` and one of `any` |
| `vi_search_batch(db, qs, nq, k, ids, scores, threads)` | many queries on several threads |
| `vi_build(db)` | builds the index (searches before it scan the new rows exactly) |
| `vi_set_recall(db, target)` | keeps recall@10 at `target`: every build picks the fewest probes that reach it |
| `vi_set_layout(db, code_bits, row_bytes)` | 4- or 2-bit scan codes, f32 or int8 rows, before the first row |
| `vi_save(db, path)` | rewrites the file and starts a fresh journal |
| `vi_sync(db)` | flushes the journal to the disk |
| `vi_snapshot(db, path)`, `vi_snapshot_wait(db, block)` | writes the file from a child process while this one keeps serving |

## C

```c
#include "vecindex.h"
#include <stdio.h>

int main(void)
{
    vi_db *db = vi_open("demo.vxi", 4, 0);
    float a[4] = { 1, 0, 0, 0 }, b[4] = { 0, 1, 0, 0 }, q[4] = { 0.9f, 0.1f, 0, 0 }, scores[2];
    vi_i64 ids[2];
    int n, i;
    vi_add(db, 1, a);
    vi_add(db, 2, b);
    n = vi_search(db, q, 2, ids, scores);
    for (i = 0; i < n; ++i) printf("%d %.3f\n", (int)ids[i], scores[i]);
    vi_close(db);
    return 0;
}
```

```sh
cc -Iinclude demo.c -L.build -lvecindex -lm -lpthread -o demo && LD_LIBRARY_PATH=.build ./demo
```

Linking `.build/libvecindex.a` instead needs no `LD_LIBRARY_PATH`; compiling `src/vecindex.c` into the program
(`-Isrc -Iinclude`) needs no library at all.

## Python (ctypes, standard library)

```python
import ctypes

vi = ctypes.CDLL("libvecindex.so")
F = ctypes.POINTER(ctypes.c_float)
vi.vi_open.restype = ctypes.c_void_p
vi.vi_open.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_int]
vi.vi_add.argtypes = [ctypes.c_void_p, ctypes.c_int64, F]
vi.vi_search.argtypes = [ctypes.c_void_p, F, ctypes.c_int, ctypes.POINTER(ctypes.c_int64), F]
vi.vi_close.argtypes = [ctypes.c_void_p]

Vec = ctypes.c_float * 4
db = vi.vi_open(b"demo.vxi", 4, 0)
vi.vi_add(db, 1, Vec(1, 0, 0, 0))
vi.vi_add(db, 2, Vec(0, 1, 0, 0))
ids, scores = (ctypes.c_int64 * 2)(), (ctypes.c_float * 2)()
n = vi.vi_search(db, Vec(0.9, 0.1, 0, 0), 2, ids, scores)
for i in range(n):
    print(ids[i], round(scores[i], 3))
vi.vi_close(db)
```

```sh
LD_LIBRARY_PATH=.build python3 demo.py
```

## Go (cgo)

```go
package main

// #cgo LDFLAGS: -lvecindex -lm -lpthread
// #include <stdlib.h>
// #include "vecindex.h"
import "C"

import (
	"fmt"
	"unsafe"
)

func main() {
	path := C.CString("demo.vxi")
	defer C.free(unsafe.Pointer(path))
	db := C.vi_open(path, 4, 0)
	defer C.vi_close(db)
	C.vi_add(db, 1, &[]C.float{1, 0, 0, 0}[0])
	C.vi_add(db, 2, &[]C.float{0, 1, 0, 0}[0])
	ids, scores := make([]C.vi_i64, 2), make([]C.float, 2)
	n := C.vi_search(db, &[]C.float{0.9, 0.1, 0, 0}[0], 2, &ids[0], &scores[0])
	for i := 0; i < int(n); i++ {
		fmt.Printf("%d %.3f\n", ids[i], scores[i])
	}
}
```

```sh
CGO_CFLAGS=-I/path/to/vecindex/include CGO_LDFLAGS=-L/path/to/vecindex/.build go build -o demo .
LD_LIBRARY_PATH=/path/to/vecindex/.build ./demo
```

## Rust (extern "C", no crates)

```rust
use std::ffi::CString;
use std::os::raw::{c_char, c_float, c_int, c_void};

#[link(name = "vecindex")]
extern "C" {
    fn vi_open(path: *const c_char, dim: c_int, flags: c_int) -> *mut c_void;
    fn vi_add(db: *mut c_void, id: i64, vec: *const c_float) -> c_int;
    fn vi_search(db: *mut c_void, q: *const c_float, k: c_int, ids: *mut i64, scores: *mut c_float) -> c_int;
    fn vi_close(db: *mut c_void);
}

fn main() {
    let path = CString::new("demo.vxi").unwrap();
    unsafe {
        let db = vi_open(path.as_ptr(), 4, 0);
        vi_add(db, 1, [1.0f32, 0.0, 0.0, 0.0].as_ptr());
        vi_add(db, 2, [0.0f32, 1.0, 0.0, 0.0].as_ptr());
        let (mut ids, mut scores) = ([0i64; 2], [0f32; 2]);
        let n = vi_search(db, [0.9f32, 0.1, 0.0, 0.0].as_ptr(), 2, ids.as_mut_ptr(), scores.as_mut_ptr());
        for i in 0..n as usize {
            println!("{} {:.3}", ids[i], scores[i]);
        }
        vi_close(db);
    }
}
```

```sh
rustc -O demo.rs -L .build -o demo && LD_LIBRARY_PATH=.build ./demo
```

Other languages follow the same pattern through their own FFI (Node with koffi or N-API, Java with the FFM API, C#
with P/Invoke, Zig with `@cImport`, Lua with LuaJIT's FFI); those were not run here.

## Texts: from a small chunk to the whole document

A model answers better from a whole passage than from the sentence that matched, and an embedding matches a short
chunk better than a long one. So keep both: one row per small chunk, its text beside it, and an id that names the
document - `doc << 20 | chunk` - so the chunks around a hit, or all of them, come back with one `vi_range`.

```c
#include "vecindex.h"
#include <stdio.h>

int main(void)
{
    vi_db *db = vi_open("rag.vxi", 4, 0);
    const char *chunk[3] = { "Tea came to Europe in the 1600s.", "Green and black tea come from one plant.",
                             "Oolong is partly oxidised." };
    float v[3][4] = { { 1, 0, 0, 0 }, { 0.9f, 0.4f, 0, 0 }, { 0.8f, 0, 0.6f, 0 } }, q[4] = { 0.95f, 0.3f, 0, 0 }, sc[3];
    vi_i64 doc = 42, ids[8], hit;
    char text[256];
    int i, n;
    for (i = 0; i < 3; ++i) vi_add_text(db, doc << 20 | i, v[i], chunk[i], -1);   /* id = document << 20 | chunk */
    n = vi_search_hybrid(db, q, "oxidised", -1, 1, ids, sc);                      /* the best small chunk... */
    hit = ids[0];
    printf("hit: doc %d chunk %d\n", (int)(hit >> 20), (int)(hit & 0xfffff));
    n = vi_range(db, hit >> 20 << 20, (hit >> 20 << 20) | 0xfffff, ids, 8);       /* ...then its whole document */
    for (i = 0; i < n; ++i) {
        int len = vi_get_text(db, ids[i], text, (int)sizeof(text));
        printf("%d: %.*s\n", (int)(ids[i] & 0xfffff), len, text);
    }
    vi_close(db);
    return 0;
}
```

```
hit: doc 42 chunk 2
0: Tea came to Europe in the 1600s.
1: Green and black tea come from one plant.
2: Oolong is partly oxidised.
```

The query vector is closest to chunk 1; the word "oxidised" picks chunk 2. `vi_search_hybrid` runs both sides and
fuses them by rank (reciprocal rank fusion, the weights `vecdb` measured: words 1.5, vector 0.6, plus up to 2 for a
side whose best hit stands well clear of its runner-up), so an exact name, a number or a rare word is found where the
embedding blurs it. Pass `NULL` for the words to search by the vector alone, or for the vector to search by words.

Texts are journaled with their row (one record, so a crash keeps both or neither), saved in the file and mapped from
it on open, so nothing is read until it is asked for; texts added since the last save live in memory (spilled to a
file with the columns past 256 MB). Replaced and deleted texts stay until the next `vi_save`, which writes only the
live ones and, when at least half the heap is garbage, compacts the copy in memory too. The word index is built by the
first hybrid search after an open (one pass over the texts) and kept current by every write after it.

## Durability

A store opened from a file keeps a journal of every add and delete next to it (`PATH.j`). `vi_sync` flushes it to
the disk; `vi_open` replays it. `vi_save` rewrites the whole file and starts a fresh journal. `vi_snapshot` does the
same from a child process (`fork`: it writes a copy-on-write image of the store at that moment) while the parent keeps
answering searches and taking writes; the writes made meanwhile go to the old journal and to the new file's
(`PATH.jn`), which takes over once the file is complete. At any moment of a crash, the file on disk and one of the
two journals belong together: both carry the same 64-bit stamp. `tests/scrash.c` kills a writer with snapshots running
at random moments, in every other round together with its snapshot child, and checks that every synced row is there
exactly once.

A snapshot costs memory for the pages the parent changes while the child writes, up to a second copy of whatever is
rewritten meanwhile. Under strict overcommit (`vm.overcommit_memory=2`) `fork` can fail for a store larger than the
free commit limit; `vi_snapshot` then returns -1 and nothing changes.
