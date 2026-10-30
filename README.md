# vecindex

A vector database in C89 for approximate nearest-neighbour search: a TurboQuant-style rotated 4-bit (or 2-bit)
fast-scan with exact re-scoring, IVF clustering, bit-packed 1-8 bit quantizers, BM25 hybrid search and an optional
agent-memory layer. It runs inside a program in any language through its C API (`include/vecindex.h`, one shared
or static library), or as `vserv`, a small epoll server (Unix and TCP sockets, a binary protocol and HTTP/1.1, TLS
1.2 through the vendored BearSSL). Nothing beyond libc, libm and pthreads (and BearSSL for TLS).

It used to live in [holyc](https://github.com/Gucixdev/holyc) as `ragdb/`; its history came along.

## Layout

| path | what is there |
|---|---|
| `include/vecindex.h` | the C API: one opaque handle, plain C types; `libvecindex.so` / `.a` export nothing else |
| `src/` | the library and the server: headers, quantizers, indexes, the store, vecdb, mem.c, `vserv.c`, `vecdb_cli.c` |
| `tests/` | one self-test per module (`NAME_test.c`), plus the crash, fuzz, thread and stress tests |
| `bench/` | benchmark programs in C and `run_all.sh`, which runs every measurement in [docs/benchmarks.md](docs/benchmarks.md) |
| `py/` | all the Python: the harness that times the other libraries, dataset preparation, the embedding and rerank servers, the server's end-to-end tests |
| `docs/` | the long-form documentation (list below) |
| `tools/gen` | regenerates `src/q2.c`, `q4.c`, `q5.c` and their tests from the q1 templates |
| `third_party/bearssl/` | BearSSL 0.6, MIT, unmodified |
| `emb/` | generated retrieval benchmark data, not committed |

Everything is one translation unit: a program includes the `.c` file of the layer it needs (`store.c`, `vecdb.c`,
`mem.c`) and compiles once with `-Isrc`. `./run` builds each test into `.build/`. A file named `LAYER_part.c` is a
part of `LAYER.c`, which includes it; it is not compiled on its own.

| `src/` files | what they hold |
|---|---|
| `base.h`, `arena_alloc.h`, `contracts.h`, `print.h`, `fine_grained.h`, `vsimd.h` | fixed-size types, the arena allocator, contracts, output without stdio, the job system, SIMD helpers |
| `q1.c` - `q5.c`, `qmix.c`, `q4fs.c` | bit-packed 1-8 bit scalar quantizers, mixed depth per dimension, the 4-bit fast-scan |
| `ivf.c`, `hnsw.c`, `engine.c`, `engine_disk.c` | the k-means coarse router, the HNSW graph router, the adaptive engine and its file format |
| `tq.c`, `tq_scan.c`, `tq_add.c` | the rotated 4-bit / 2-bit fast-scan index: build, scan, rows added after the build |
| `hdict.c`, `lex.c` | hash dictionaries; the term dictionary, postings and BM25 |
| `store.c`, `store_tune.c`, `store_live.c`, `store_search.c`, `store_text.c`, `store_disk.c` | the vector store: rows, ids, build; self-tuning; live mode; search; texts, id ranges and hybrid search; the file and its journal |
| `vecdb.c`, `vecdb_embed.c`, `vecdb_search.c`, `vecdb_graph.c`, `vecdb_disk.c`, `vecdb_cli.c` | the note database: notes, links, tags; embedding servers; hybrid search; MCTS and the query language; files; the command line |
| `mem.c`, `mem_rules.c`, `mem_disk.c` | agent memory: lifetime and tiers; the rule engine; its files |
| `rerank.c` | a cross-encoder second pass over vecdb's hybrid search |
| `vecindex.c` | the C API of `include/vecindex.h` over the store |
| `vfind.c` | search a store's texts as you type, like fzf; Enter opens the whole document |
| `vserv.c`, `vserv_proto.c`, `vserv_text.c`, `vserv_log.c`, `vserv_pem.c` | the server: sockets, workers, TLS; the line, binary and HTTP protocols; texts and JSON; the log; certificates and keys |

## Build and test

```sh
git clone https://github.com/Gucixdev/vecindex && cd vecindex
./run            # self-tests, fuzzers, thread and crash-safety tests, the server end to end (needs bash and gcc)
./run stress     # plus the churn / oracle stress tests
tools/cross      # the core tests for aarch64, armv7, riscv64 and s390x under qemu (needs their gcc and qemu-user)
```

## Use it

From any language: `./run` builds `.build/libvecindex.so` and `.build/libvecindex.a`, whose only exports are the
`vi_*` functions of `include/vecindex.h`. In C:

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
1 -0.012
2 -1.779
```

For an LLM, each row can carry its text: `vi_add_text`, then `vi_search_hybrid` (the vector and BM25 over the texts,
fused by rank) and `vi_range` over a document's ids to hand the model the chunks around a hit, or the whole document.
`.build/vfind docs.vxi` searches those texts as you type, like fzf: a few lines of each hit, Enter opens the whole
document, Enter again prints the choice. The same from Python (ctypes), Go (cgo) and Rust (`extern "C"`), every
function, texts, and how durability works: [docs/api.md](docs/api.md). Inside a C program the library can also be
one more file of the build (`src/vecindex.c`, or `store.c` itself for the internal API). Over a socket, `vserv` serves one store file to anything that can open a
socket, in one-line commands (`search 10 0.1 0.2 ...` -> `ok 3 42 -0.01 ...`), binary frames or HTTP
([docs/server.md](docs/server.md)).

## Results

GloVe-100 (1,183,514 vectors, 10,000 held-out queries, recall@10 against the exact top 10), 2 vCPUs of an Intel
Xeon (Cascade Lake). Queries per second at the fastest setting that reaches each recall, as one batch on 2 threads /
one query per call on 1 thread:

| method | build | bytes/vec | best recall@10 | >= 0.90 | >= 0.95 | >= 0.97 |
|---|---:|---:|---:|---:|---:|---:|
| vecindex 4-bit + f32 rows | 8.0 s | 475 | 0.9884 | 7,996 / 2,277 | 4,897 / 1,433 | 2,606 / 862 |
| vecindex 2-bit + int8 rows | 8.3 s | 153 | 0.9712 | 7,235 / 2,654 | 2,420 / 949 | 1,896 / 715 |
| FAISS IVF4096,PQ50x4fs,RFlat | 124.5 s | 436 | 0.9785 | 4,456 / 1,081 | 2,822 / 871 | 1,846 / 526 |
| turbovec 4-bit | 2.2 s | 56 | 0.8465 | - | - | - |

The method, every row, the scale runs (100M vectors in 5.84 GiB), what each optimisation bought and the bottlenecks
left are in [docs/benchmarks.md](docs/benchmarks.md).

## Production readiness

**Embedded first, one machine.** The library runs inside the application, like SQLite; `vserv` puts the same store
on a socket for other processes and languages. Neither needs a cluster or a cloud: a tenant is a store file, a batch
of writes goes in whole or not at all, the journal is flushed every second and folded into the file by background
snapshots, the log records who did what (there are no users and roles), mutual TLS can tie a client to its tenant,
one setting caps the CPU it takes and another what stays in RAM. A standby is a copy of the file and its journal;
there is no replication. Every number below is measured, except in the rows marked not yet run: the older rows on a
GTX 1050 Ti box (7.8 GB RAM, 12 cores), the scale, server and benchmark rows on the 2-core box of the Results
section; `./run`, `./run stress`, `./run bench` and `./run real_val` reproduce them.

| area | status | evidence |
|---|---|---|
| Crash safety | atomic per-file + cross-file (generation-suffix) writes; the store's journal and background snapshots | `tests/vcrash.c`/`tests/vcrash2.c`, 25-90 rounds, always consistent; `tests/scrash.c` kills a store writer (and in every other round its snapshot child) at random moments: every synced row there exactly once |
| CPUs | x86-64 (AVX-512, AVX2 or neither), ARM 64- and 32-bit (NEON), RISC-V 64, s390x (big-endian); 32-bit builds keep 64-bit file offsets | `tests/simd_test.c` checks every SIMD kernel against a plain reference: in `./run` for this CPU and, on x86-64, for an AVX2-only and a no-SIMD build. `tools/cross` builds the core tests for aarch64, armv7, riscv64 and s390x and runs them under qemu; it found a 32-bit bug that broke snapshots, fixed. Speed on real ARM hardware: not measured |
| C API | `include/vecindex.h`, only `vi_*` exported | `tests/api_test.c` against the shared and the static library; Python, Go and Rust bindings run by hand ([docs/api.md](docs/api.md)) |
| Concurrency | one rwlock per layer, TSan-clean, the store's live thread too | `tests/vthreads.c`/`tests/mthreads.c` and the store self-test under TSan in `./run` (it found two real races in live mode, fixed) |
| Fuzzing | 3 fuzzers (parser, rule engine, malformed input) | `tests/vfuzz.c`/`tests/mrfuzz.c` - the rule-parser one found and fixed a real stack-overflow crash on its first run |
| Adversarial stress | invariant checks, not just "doesn't crash" | `tests/stress.c`/`tests/mstress.c` - found and fixed 5 bugs this way |
| Static analysis | clang analyzer + clang-tidy across the core, findings triaged by hand | 2 real (unreachable) issues fixed, the rest traced and confirmed false positives |
| Scale | measured to the breaking point | store: 100M vectors (d=96, 2-bit + int8) ingested, built and searched in a 5.84 GiB cgroup on 2 cores; the old store was OOM-killed between 7M and 8M. vecdb: 8M notes end to end where the old code died at ~4M |
| Memory tiers | the scanned codes hot in RAM, the rows cold on the disk and read only for re-scored candidates; `vi_set_memory` / `vserv -m` lock the hot part (or the rows too) with mlock | tests written, not yet run: `tests/store_test.c` checks the sizes and, through VmLck, what gets locked and released; `tests/vserv_test.c` the stats fields and `-m` |
| Server | `src/vserv.c`: epoll, Unix + TCP, a line protocol + binary + HTTP/1.1, TLS 1.2 (BearSSL), mutual TLS; the journal flushed every second, background snapshots; a log of every write with its peer (uid/pid, address, certificate name) instead of users and roles | `py/vserv_test.py` (29 checks), `py/vserv_tls_test.py` (22 checks) and `tests/vserv_test.c` (kill -9 against the journal, timed snapshots, the log, mTLS names) in `./run` |
| Retrieval quality | a SQuAD-based benchmark, not hand-labeled questions | `bench/real_val.c`: gold@10 up to 85.6%, against an 87.1% ceiling for this embedder |
| Reranking | a cross-encoder second pass with a measured effect | `src/rerank.c`: MRR 0.751 -> 0.887, gold@1 62.1% -> 81.5% on the subset where it has room to help |
| Independent review | **not done** | every test here was written and run by the author of the code |
| Tenants | a store file per tenant, opened on first use: in the library (`vi_dir_*`) or the server (`vserv DIR`, where `-U` binds a client certificate to its tenant) | tests written, not yet run: `tests/api_test.c` (`vi_dir_*`), `tests/vserv_test.c` (isolation, the three protocols, the log, a restart, `-U` with a tenant's certificate) |
| Transactions | write batches: searches see all or none of one, a crash leaves all or none (one CRC-checked journal record), commit with fsync on request (`vi_batch_*`, `begin`/`commit` in `vserv`) | tests written, not yet run: `tests/store_test.c` (refused and aborted batches, a torn and a damaged batch in the journal, a reader that must never see half of one, durable commits from 4 threads), `tests/api_test.c`, `tests/vserv_test.c` (kill -9 right after `commit sync`) |
| Users and roles / replication | **not present, on purpose** | access is the socket's file mode, the bound address and client certificates; accountability is the log; a backup or a standby is a copy of the file and its journal ([docs/server.md](docs/server.md)) |

## Documentation

- [docs/api.md](docs/api.md) - the C API, bindings from Python, Go and Rust, durability (journal and snapshots).
- [docs/benchmarks.md](docs/benchmarks.md) - GloVe-100 against FAISS and turbovec (the Qdrant runs never finished); scale; the priority table
  of optimisations; bottlenecks; how to reproduce.
- [docs/server.md](docs/server.md) - `vserv`: sockets, protocols, TLS, live mode.
- [docs/vecdb.md](docs/vecdb.md) - the note database on top of the store: hybrid search, wikilinks, MCTS
  exploration, embeddings, reranking, durability, self-tuning, live ingest, scale.
- [docs/mem.md](docs/mem.md) - mem.c: lifetime, metamemory, event -> fact -> concept, a rule engine.
- [docs/comparison.md](docs/comparison.md) - how this compares to mainstream RAG and vector-DB stacks.
- [docs/quantizers.txt](docs/quantizers.txt) - the bit-packed quantizer files q1-q5, qmix, q4fs, ivf, hnsw, engine.

## License

MIT, see `LICENSE`. BearSSL is MIT too (`third_party/bearssl/LICENSE.txt`).
