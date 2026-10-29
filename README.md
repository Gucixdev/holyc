# vecindex

A vector database in C89 for approximate nearest-neighbour search: a TurboQuant-style rotated 4-bit (or 2-bit)
fast-scan with exact re-scoring, IVF clustering, bit-packed 1-8 bit quantizers, BM25 hybrid search and an optional
agent-memory layer. It runs embedded in a program, or as `vserv`, a small epoll server (Unix and TCP sockets, a
binary protocol and HTTP/1.1, TLS 1.2 through the vendored BearSSL). Nothing beyond libc, libm and pthreads (and
BearSSL for TLS).

It used to live in [holyc](https://github.com/Gucixdev/holyc) as `ragdb/`; its history came along.

## Layout

| path | what is there |
|---|---|
| `src/` | the library and the server: headers, quantizers, indexes, the store, vecdb, mem.c, `vserv.c`, `vecdb_cli.c` |
| `tests/` | one self-test per module (`NAME_test.c`), plus the crash, fuzz, thread and stress tests |
| `bench/` | benchmark programs in C and `run_all.sh`, which runs every measurement in [docs/benchmarks.md](docs/benchmarks.md) |
| `py/` | all the Python: the harness that times the other libraries, dataset preparation, the embedding and rerank servers, the server's end-to-end tests |
| `docs/` | the long-form documentation (list below) |
| `tools/gen` | regenerates `src/q2.c`, `q4.c`, `q5.c` and their tests from the q1 templates |
| `third_party/bearssl/` | BearSSL 0.6, MIT, unmodified |
| `emb/` | generated retrieval benchmark data, not committed |

Everything is one translation unit: a program includes the `.c` file of the layer it needs (`store.c`, `vecdb.c`,
`mem.c`) and compiles once with `-Isrc`. `./run` builds each test into `.build/`.

## Build and test

```sh
git clone https://github.com/Gucixdev/vecindex && cd vecindex
./run            # self-tests, fuzzers, thread and crash-safety tests, the server end to end (needs bash and gcc)
./run stress     # plus the churn / oracle stress tests
```

## Use it

In a C program:

```c
#include "store.c"
#include <stdio.h>

int main(void)
{
    static Arena arena;
    RagStore *s = store_new(&arena, 4);
    f32 a[4] = { 1, 0, 0, 0 }, b[4] = { 0, 1, 0, 0 }, q[4] = { 0.9f, 0.1f, 0, 0 };
    i64 ids[2];
    f32 score[2];
    i32 n, i;
    store_add(s, a, 1);
    store_add(s, b, 2);
    store_build(s);
    n = store_search(s, q, 2, ids, score);           /* your ids, best first; score = -|q - v|^2 */
    for (i = 0; i < n; ++i) printf("%d %.3f\n", (int)ids[i], score[i]);
    store_save(s, "demo.vxi");                       /* store_load(&arena, "demo.vxi") maps it back */
    store_destroy(s);
    return 0;
}
```

```sh
gcc -std=c89 -O2 -march=native -D_POSIX_C_SOURCE=200112L -Isrc demo.c -lm -lpthread -o demo && ./demo
1 -0.012
2 -1.779
```

Over a socket: `vserv` serves one store file to any language that can open a socket ([docs/server.md](docs/server.md)).

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

**Embedded, single-node.** Not a hosted, multi-tenant service like Pinecone, Qdrant or Milvus. Every number below is
measured: the older rows on a GTX 1050 Ti box (7.8 GB RAM, 12 cores), the scale, server and benchmark rows on the
2-core box of the Results section; `./run`, `./run stress`, `./run bench` and `./run real_val` reproduce them.

| area | status | evidence |
|---|---|---|
| Crash safety | atomic per-file + cross-file (generation-suffix) writes | `tests/vcrash.c`/`tests/vcrash2.c`, 25-90 rounds, always consistent |
| Concurrency | one rwlock per layer, TSan-clean, the store's live thread too | `tests/vthreads.c`/`tests/mthreads.c` and the store self-test under TSan in `./run` (it found two real races in live mode, fixed) |
| Fuzzing | 3 fuzzers (parser, rule engine, malformed input) | `tests/vfuzz.c`/`tests/mrfuzz.c` - the rule-parser one found and fixed a real stack-overflow crash on its first run |
| Adversarial stress | invariant checks, not just "doesn't crash" | `tests/stress.c`/`tests/mstress.c` - found and fixed 5 bugs this way |
| Static analysis | clang analyzer + clang-tidy across the core, findings triaged by hand | 2 real (unreachable) issues fixed, the rest traced and confirmed false positives |
| Scale | measured to the breaking point | store: 100M vectors (d=96, 2-bit + int8) ingested, built and searched in a 5.84 GiB cgroup on 2 cores; the old store was OOM-killed between 7M and 8M. vecdb: 8M notes end to end where the old code died at ~4M |
| Server | `src/vserv.c`: epoll, Unix + TCP, HTTP/1.1 + binary, TLS 1.2 (BearSSL), mutual TLS | `py/vserv_test.py` (29 checks) and `py/vserv_tls_test.py` (22 checks) in `./run` |
| Retrieval quality | a SQuAD-based benchmark, not hand-labeled questions | `bench/real_val.c`: gold@10 up to 85.6%, against an 87.1% ceiling for this embedder |
| Reranking | a cross-encoder second pass with a measured effect | `src/rerank.c`: MRR 0.751 -> 0.887, gold@1 62.1% -> 81.5% on the subset where it has room to help |
| Independent review | **not done** | every test here was written and run by the author of the code |
| Multi-tenancy / auth / replication / HA | **not present** | out of scope for a library |

## Documentation

- [docs/benchmarks.md](docs/benchmarks.md) - GloVe-100 against FAISS, turbovec and Qdrant; scale; the priority table
  of optimisations; bottlenecks; how to reproduce.
- [docs/server.md](docs/server.md) - `vserv`: sockets, protocols, TLS, live mode.
- [docs/vecdb.md](docs/vecdb.md) - the note database on top of the store: hybrid search, wikilinks, MCTS
  exploration, embeddings, reranking, durability, self-tuning, live ingest, scale.
- [docs/mem.md](docs/mem.md) - mem.c: lifetime, metamemory, event -> fact -> concept, a rule engine.
- [docs/comparison.md](docs/comparison.md) - how this compares to mainstream RAG and vector-DB stacks.
- [docs/quantizers.txt](docs/quantizers.txt) - the bit-packed quantizer files q1-q5, qmix, q4fs, ivf, hnsw, engine.

## License

MIT, see `LICENSE`. BearSSL is MIT too (`third_party/bearssl/LICENSE.txt`).
