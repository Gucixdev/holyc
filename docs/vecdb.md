# vecdb: notes on top of vecindex

`src/hdict.c` (open-addressing hash dictionaries, i64 and string keys), `src/lex.c` (BM25
term dictionary + postings) and `src/vecdb.c` sit on `src/store.c`.

* **Hash dictionary inside the index** - `src/store.c` keeps an id -> row map, so
  `store_has/get_vec/set_meta/get_meta`, delete and replace are O(1) instead of a scan.
* **Precision** - `vecdb_search` fuses vector kNN and BM25 with reciprocal rank
  fusion, so rare exact words ("Delft") win even when the embedding blurs them.
* **Graph** - `[[wikilinks]]` (alias, `#heading`, `![[embeds]]`), backlinks,
  `#hashtags` (nested `#a/b` also tag `#a`, ignored in code/URLs/headings),
  ghost nodes for links to notes that do not exist yet.
* **MCTS** - `vecdb_explore` runs UCB1 tree search over links, backlinks and tags
  from the hybrid hits and surfaces bridge notes that plain kNN misses.
* CLI: `gcc ... src/vecdb_cli.c -lm -lpthread -o vecdb_cli`, then
  `vecdb_cli DB.vdb index DIR | search Q | explore Q | links T | backlinks T | tag T | tags | ghosts`.
* Tests: `./run` (includes hdict, lex, vecdb), `./run stress`.

## vecdb benchmarks

`bench/vbench.c` (synthetic, any N), `bench/vbench_wiki.c` (real Simple English Wikipedia:
`bzcat simplewiki-latest-pages-articles.xml.bz2 | python3 py/wikiprep.py > wiki.rec`, then
`.build/vbench_wiki wiki.rec`) and `bench/vcmp.c` + `py/vcmp_turbovec.py` (vecindex vs turbovec on identical vectors).

201k articles, 12 cores, hit rates over 300 queries (hashed 256-d embedder):

| | vecindex (vector only) | vecdb hybrid | vecdb MCTS |
|---|---|---|---|
| body's first 12 words -> article in top 10 | 72% | 97% | 96% |
| title -> exact article first | 23% | 100% | 100% |
| title -> its real link neighbours (recall@10) | 5% | 21% | 32% |
| ms / query | 1.6 | 2.3 | 3.7 |

## TurboQuant lesson (fast vector index)

`vcmp` on the same 201k x 256 vectors, recall@10 against exact search:

| index | build | recall@10 | ms/query |
|---|---|---|---|
| vecindex auto-tuned (IVF+q4) | 70 s | 76% | 7.9 |
| vecindex IVF fast build | 8.6 s | 65% | 2.1 |
| vecindex flat q4 | 2.9 s | 56% | 24.8 |
| turbovec 4-bit (TurboQuant) | 0.35 s | 86% | 1.5 |
| turbovec 2-bit | 0.23 s | 59% | 0.5 |
| **`src/tq.c` (rotation + 4-bit fast-scan + exact re-score)** | **1.1 s** | **100%** | **1.2-1.4** |

`src/tq.c` copies what makes TurboQuant work: a fixed random rotation (signs + Walsh-Hadamard) turns every
coordinate into ~N(0,1/d) for any data, so one fixed Lloyd-Max 4-bit codebook is near-optimal with no
training; codes sit in a fast-scan layout scored with PSHUFB (AVX2 or AVX-512, threaded above 150k rows). The scan only
proposes 256+ candidates; the f32 rows the store keeps re-score them exactly, which is what lifts 86% to
100%. `store_set_fast` selects it (vecdb does); the IVF auto-tuner remains for the plain store API.

## Persistence

`vecdb_save` writes PATH.vdb (node table), PATH.vdt (bodies, stay on disk), PATH.vxi (vector rows) and
PATH.img.vxi. `vecdb_sync` afterwards only appends to journals (`store_sync`, PATH.vxi.j, a torn tail is
ignored on load), so a save costs the change, not the store. Loading rebuilds links, tags and BM25 from the
bodies (~10 us per note); the vector rows and the TurboQuant index are mapped from PATH.vxi as they are (no
rebuild, pages come in when first touched).

## Real embeddings, documents, pictures

`db->embed` / `vecdb_use_embproc` plug in any model through a child process; `py/embedsrv.py` serves
multilingual-e5-small (Polish ok) and `--clip` serves CLIP for pictures. The CLI (`VECDB_EMBED_SERVER`,
`VECDB_CLIP_SERVER` env vars, `repl` keeps the models loaded) indexes .md/.txt, and through `py/extract.py`
pdf/docx/odt/pptx/epub/html: long documents become a document note linked to overlapping-free chunks
(each chunk links back and to its neighbours), pictures go to a second vector space and are linked by
`![[pic.png]]` / `![](pic.png)`. Search: `search`, `images` (text -> pictures), `similar` (picture -> pictures).

## Cross-encoder reranking (rerank.c) - a real second-stage pass, with a real measured effect

Hybrid (vector+BM25 RRF) search is a real substitute for a lot of what reranking buys, but not the same as a
cross-encoder that actually reads query and candidate together - a stage most production RAG stacks add on
top of ANN retrieval that this repo didn't have. `src/rerank.c` (`#include "vecdb.c"`, same layering as mem.c -
vecdb.c itself untouched) adds it: `vecdb_rerank_open` spawns `py/rerank_srv.py` (any cross-encoder model, same
child-process/pipe pattern as `vecdb_embproc_open`, just a different protocol since a reranker scores
(query, candidate) PAIRS, not single texts), `vecdb_rerank_hits` re-scores and re-sorts an existing
`VHit[]` (from `vecdb_search`/`vecdb_query`) in place, falling back to the original hybrid order untouched if
the rerank server is unavailable or fails - always an optional refinement, never the only ranking path.
Self-test: `./run` (`RERANK_DEMO`), degrades gracefully (prints a skip message, does not fail the run) when
no torch-capable Python is reachable.

**Measured effect**, not just "we added an API": against `bench/real_val.c`'s own SQuAD benchmark data (see below),
on the 871/1000 questions where the gold passage is already somewhere in the raw top-10 cosine neighbours (a
reranker can only reorder what retrieval already found - the other 129 are retrieval misses, unaffected by
any reranking and reported separately), `cross-encoder/ms-marco-MiniLM-L-6-v2` reranking those 10 candidates:

| | MRR | gold@1 |
|---|---|---|
| before (raw cosine order) | 0.751 | 62.1% |
| after (cross-encoder rerank) | 0.887 | 81.5% |
| delta | **+0.136** | **+19.4 points** |

(`py/eval_rerank.py`, reusing `bench/real_val.c`'s own ground truth - `py/real_texts.py` reconstructs the
corpus/query text alongside it, since `py/real_gen.py` itself only ever saved vectors.) A +19.4 point jump in
"is the right answer literally first" is a substantial, real quality gain for the ~330/871 questions where
the gold passage was in the top-10 but not already first - exactly the case a reranker exists for.

## Threads

Searches on a store take a read lock and run concurrently (stress test: 8 readers + a writer);
the TurboQuant scan itself is multi-threaded on big stores, and `store_search_batch` spreads a batch of queries
over threads. The index build is threaded (k-means, assignment, encoding). Lexical (BM25) searches on one index
take turns behind a mutex. Embedding and ingest are single-threaded.

## Query language

`vecdb_query(db, "words #tag -#draft [[Note]] to:[[Note]] from:[[Note]]", k, mcts, out)` (CLI `search` and
`explore` use it): `#tag` / `-#tag` / `tag:x` (nested: `#a` also matches `#a/b`), `[[X]]` notes linked to or from X,
`to:[[X]]` X's backlinks, `from:[[X]]` X's links. Conditions are ANDed with the words; without words the
matches are listed most-linked first; an unknown note or tag matches nothing.

## Embedding: CPU only, batched, threaded

`py/embedsrv.py` never touches the GPU (`CUDA_VISIBLE_DEVICES=""`). `vecdb_add_notes` sends 32 texts per round trip
(`B`/`C` batch kinds) or, with the built-in embedder, embeds on all cores. Measured on 1024 Wikipedia leads
(e5-small, 12 threads): batching alone changes little (the model is compute-bound: ~200 GFLOP/s already),
what pays is fewer tokens and int8: `EMBED_INT8=1` ~ -20% time, `EMBED_MAXLEN=128` ~ -50%
(25 ms/article instead of 57), with no loss in hybrid retrieval (vector-only -1..2 points). Recommended for
ingest: `EMBED_INT8=1 EMBED_MAXLEN=128`. Repeated query strings are served from a 32-entry cache
(a model call is ~12 ms, the search itself <1 ms).

## Index choice

`store_build` picks automatically: with no memory budget (default) the TurboQuant-style scan (`src/tq.c`);
with `ENG_MIN_SIZE` or a `max_code_bytes` below the f32 size, the q1-q5 / IVF / HNSW tuner (`engine_build`),
which is what small-footprint goals need.

## GPU / CPU mix

`VECDB_EMBED_SERVER="cmd1|cmd2"` starts several servers; batches (1024 texts per flush in the CLI) are cut into
groups of 32 and each server pulls the next group when free; single queries go to the first one.
`EMBED_DEVICE=cuda` puts a server on the GPU (fp32: Pascal's fp16 is throttled and slower), `EMBED_THREADS`
limits its torch threads. Measured on 4094 Wikipedia leads (e5-small, 128 tokens): GPU alone 5.5 ms/article,
CPU alone 36, GPU + CPU int8 22.1 s vs 22.6 s GPU alone (no gain: the CPU server steals cores the GPU
server needs for tokenising), GPU + CPU fp32 31.7 s (worse). For bulk ingest use the GPU alone; a CPU server
is for queries (12 ms) once the card is needed elsewhere.

## Embedding cache

`vecdb_embed_cache_open(db, path, tag)` keeps every document vector the model made in a file, keyed by
hash(tag + text); `tag` names the model and its settings (another tag = separate entries, nothing is mixed).
Re-indexing an unchanged vault asks the model 0 times, a changed vault only for the changed notes (batches
send just the misses). The CLI turns it on whenever `VECDB_EMBED_SERVER` is set: `DB.emb` next to the database,
or `VECDB_EMBED_CACHE=path`. Only documents are cached (queries have their own 32-entry in-memory cache).

`EMBED_MAXLEN` now defaults to 128 tokens (25 ms/article on CPU int8, 5.5 ms on GPU fp32, no loss measured
on Wikipedia leads; raise it for long passages).

## Idea, not done: fixed-point (int8) embedding on the GPU

The GTX 1050 Ti (Pascal, sm_61) has the `dp4a` int8 dot instruction, so a hand-written int8 e5-small could in
theory run 2-3x faster than fp32. Not worth it now: PyTorch has no int8 GEMM path for this card (`_int_mm`
needs newer hardware), so it means custom CUDA kernels (or llama.cpp / onnxruntime int8), the card already
sustains ~1 TFLOPS of its ~2, and the whole 201k-article Wikipedia costs 18 minutes once (the embedding cache
and `vecdb_sync` mean only new or changed notes are ever embedded again). The CPU already uses dynamic int8
(`EMBED_INT8=1`, -20% time, no AVX-VNNI on this machine to gain more). Revisit if a corpus of tens of millions
of chunks shows up.

## Production notes

- **Failure handling.** The model server answers every request with a status byte; a batch that fails is retried
  text by text, a text that still fails yields no vector, and such a note is *not added* (message on stderr) and
  never cached. A query whose embedding failed falls back to words only. The server halves its batch on GPU
  out-of-memory (a card shared with the desktop has 0.3-3.5 GB free at any time) and only gives up on a single
  text. Before this a failed batch silently stored zero vectors.
- **`vecdb_free`** releases a database; `tests/vfuzz.c` (random markup, queries, replace/remove, save/load under
  ASan/UBSan, run by `./run`) found and fixed one buffer overflow in the query parser.
- **CLI**: `index DIR`, `update DIR` (changed files re-embedded through the cache, vanished ones removed, saved
  through the journal), `remove TITLE`, `stats`, `search`, `explore`, `images`, `similar`, `links`, `backlinks`,
  `tag`, `tags`, `ghosts`, `repl`. Chunks are sized to what the model reads (`EMBED_MAXLEN` x 3.5 characters,
  `VECDB_CHUNK` overrides). `./run` also drives the CLI end to end.
- **Choosing `EMBED_MAXLEN`** (3000 Wikipedia articles, e5-small, GPU): for queries taken from deep inside an
  article (beyond the first tokens) 128 tokens gives vector-only 47%, 512 gives 78% (hybrid 92% -> 97%), at
  4.4x the ingest time on a shared card (46 vs 10.6 ms/article). Leads and titles are the same at both. So: keep
  128 and let the CLI chunk long documents (each chunk then fits what is embedded), use 512 only for whole
  long notes that must be one vector.
- **Fusion weights** (words 1.5, vector 0.6 before the confidence bonus) were tuned on Wikipedia with both the
  hashed and the e5 embedder: deep-body hybrid 91.8% -> 95%, linked-neighbour recall 38% -> 46%, leads/titles
  unchanged. They are constants in `vdb_search_`.

## Durability, formats, threads

- **Atomic writes.** Every file that is replaced is written beside the old one, `fsync`ed, then `rename`d (and the
  directory `fsync`ed). Commit order for an incremental save: bodies (append-only, fsynced) -> vector files -> the
  node table *last*, which is the commit record. `vecdb_sync` does the same with the journals. If a crash lands
  between the vector files and the node table, the counts disagree on load and the vectors are recomputed (the
  embedding cache makes that cheap). `tests/vcrash.c` SIGKILLs a writer at random moments, 25 rounds per `./run`
  (hundreds of rounds tested): the database always reloads with everything that was synced. That proves process
  crashes; power loss depends on the disk honouring `fsync` and is not something I can test here.
- **Cross-file atomicity (the part per-file atomicity does not give you).** PATH.vdt and PATH.vxi are tightly
  coupled - a node table's byte offsets only mean anything against the *exact* bodies file they were written for.
  Two independent atomic renames are not atomic *as a pair*: a crash between them can leave an old node table
  paired with new bodies (or vice versa) - silently wrong reads, not a crash. This only happens on a **full
  rewrite of an already-existing path** (`vecdb_new` + `save`/`sync`, not `vecdb_load` first - what the CLI's
  `index` does when re-indexing on top of a database that already exists there); the normal incremental
  append-then-sync path never replaces anything, so it was never exposed to this. Fix: a full rewrite that would
  have to replace an existing PATH.vdt/.vxi never touches them - it writes the whole new file set under a fresh
  random generation suffix (PATH.vdt.g\<hex\>, PATH.vxi.g\<hex\>, `VecDB.bodygen`), and only the *last* step (the
  node table, itself atomic) starts pointing at that generation. A crash before it leaves the old generation's
  files untouched (loads as the old database, unaffected); a crash after leaves the new generation's files
  complete and fsynced (loads as the new one). Never a mismatched pairing, at the cost of a harmless orphaned
  file on an interrupted attempt (cleaned up opportunistically on the next successful save to that path). A brand
  new path (nothing there yet) skips the generation dance and uses the bare names - the common case stays cheap.
  `tests/vcrash2.c` (`./run`) forks a child that re-indexes on top of an existing database and kills it at a random
  point spanning the real completion boundary: over 30 rounds, roughly a even mix of "landed on the old copy"
  and "landed on the complete new copy", zero mixed/corrupted, zero load failures.
- **Format versions.** `.vdb` starts with magic `VDBX` and a version (now 5, which added `bodygen`). Version 3
  and 4 files still load; a file from a newer build, garbage, a missing or truncated file are refused and
  `vecdb_error()` says why. When the format changes, bump `VDB_VERSION` and add a branch to the loader.
- **Threads.** One readers-writer lock per database (`vecdb_*` are safe to call from many threads): searches,
  queries, MCTS and lookups share it; add / remove / smooth / build / save / sync are exclusive. Model calls happen
  outside the lock. The body file and the embedding cache have their own mutexes. `tests/vthreads.c` (4 readers + a
  writer doing adds, replaces, removals, rebuilds and syncs) runs under ThreadSanitizer and ASan in `./run`.

## Real notes check (77 markdown files: memory notes, project docs; Polish and English mixed)

Indexed with e5-small on the GPU (128 tokens): 2075 notes/chunks in 47 s. 20 hand-written questions in Polish and
English (vague on purpose, each with one expected file), hit@1 / hit@3: hybrid 12 / 14, vector only 10 / 11, words
only 11 / 14. So the mix beats each half, but modestly, and it is a 20-question sample with my own labels.
What the misses show: an index note (`MEMORY.md`, one line about every project) matches almost any query
lexically; documents cut into many chunks get many chances to rank; a Polish question about an English note
depends on e5-small's cross-lingual ability (`git` vs `gita`: the light stemmer leaves 4-letter words alone).
The CLI prints one line per document (best chunk, "+N more parts").

## Stemming: Polish and English together

`src/lex.c` folds Polish letters to ASCII (`ą ć ę ł ń ó ś ź ż` -> `a c e l n o s z`, so a query typed without diacritics
still matches) and strips one longest matching suffix from every token, then a final vowel. There is no language
detection: one rule set covers both (Polish case endings, `-owanie / -uje / -ować`, `-ami -ach -ów`; English
`-s -es -ed -ing`). Tokens with digits are left alone. Tests keep families together (kot/kota/kotem/kotami/kotów,
komputer/komputerze, programować/programuje/programowanie, zapytanie/zapytań, notes/note) and look-alikes apart
(kot/kos). English retrieval on Wikipedia did not move (hybrid 98.7% on lead words, 96.4% on deep-body words).
Known limits: stem-final alternations (dokumentacja/dokumentacji, kot/kocie, pies/psa) are not handled, and the
English side is a light suffix stripper, not Porter, so `data`/`date` meet.

## Concurrency model

The vector scan is memory-bound: on 201k x 256 (26 MB of 4-bit codes) one thread needs 2.3 ms, four threads 1.3 ms,
eight or twelve no better, a persistent thread pool no better (tried and removed). So the design stays: one
readers-writer lock for the API, a few data-parallel threads for the scan (4, enough to saturate the bus),
self-scheduling groups for the model servers (a shared counter, which is why a fast GPU takes more of the work),
no coroutines (nothing in-process waits on I/O that a thread would not wait on) and no fixed master/slave
partitions (uneven work makes the static split wait for its slowest half, which is what the CPU+GPU mix showed).
Less memory traffic (2-bit codes with a longer shortlist, IVF pruning) is what would make the scan faster.

## IVF for the TurboQuant scan (large stores)

From `STORE_IVF_MIN` = 600 000 rows the store groups the rows by cluster (IVF) and a query scans only the blocks of
the clusters closest to it. The cluster count follows the usual rule, 4 sqrt(rows), with no cluster planned below
`STORE_IVF_ROWS` = 256 rows: 1.2M rows get 4350 clusters of ~270, 100M rows 40000 of 2500. Training is spherical
k-means on 256 rows per centroid (the sample is capped at 512 MB); above 1024 clusters it is two-level: ~sqrt(nc) top
cells, trained on a subsample, each split into sub-clusters in proportion to its share of the sample. A row goes to the
closest sub-cluster among those of its 4 closest top cells (2 past 128 cells), and one Lloyd round over the whole
sample then lets neighbouring cells settle their shared borders. With only the closest cell, a quarter of GloVe's rows
sat in the wrong cluster - rows a query then looks for in vain; now 4.5% do, and the true top 10 lies inside the 96
closest clusters for 90.0% of queries instead of 87.3%, with 10% fewer rows to scan. Centroids are scored 16 at a time
(transposed, AVX-512 FMA, 4 rows x 4 groups in registers); encoding runs on every core, each thread owning a range of
blocks so no byte is shared.

A query picks its clusters from an int8 copy of the centroids: VNNI scores 16 centroids on 4 dimensions per
instruction, a quarter of the bytes of f32, so the copy stays in L2 (0.44 MB for GloVe). A binary search on a vector
count keeps about probe + probe/4 + 16 of them, and only those get exact f32 scores, so the clusters probed are the
ones f32 would pick. In a batch, 8 queries share each pass over the centroids. The scan keeps its candidates in a
reservoir (append while above the threshold, cut to the best R with a quickselect when full) and prefetches two
blocks ahead. The default probe is min(16%, 4 sqrt(clusters)) of the clusters (GloVe-100: 264 of 4350, recall@10
0.95); `store_set_probe` trades recall for speed, `store_set_rerank` sets how many candidates are re-scored exactly.
Below 600k rows the flat scan is used.

`./run` exercises the IVF scan directly (recall, tombstones, meta filter, all-clusters == flat, 2-bit too, the
two-level clustering on 1500 clusters, batch == one query at a time), and the store demo and stress test also pass
when forced onto IVF at 1500 rows (`-DSTORE_IVF_MIN=1500 -DSTORE_IVF_ROWS=100`).

## Self-tuning: ask for a recall, the store keeps it

`store_set_target_recall(s, 0.95)`: from then on every `store_build` measures recall@10 against exact search and
picks the fewest IVF probes that reach the target, aiming 1.5 standard errors above it so fresh queries meet it too.
It measures on the queries the store has actually been answering (a reservoir sample of the last 256, kept while a
target is set) or, before 64 have come in, on a spread of its own rows (each one's own row left out of the truth).
`store_tune(s)` measures again on demand - after the traffic or the data have drifted - and the probe count it
settles on is saved with the index. GloVe-100, `bench/tune.c`, on the Cascade Lake host of [benchmarks.md](benchmarks.md):

| target | tuned at build, on its own rows | held-out recall@10 (2000 queries) | re-tuned on those 2000 | recall@10 on the other 8000 |
|---|---|---|---|---|
| 0.90 | 118 probes | 0.9134 | 108 probes | 0.9053 |
| 0.95 | 264 probes | 0.9528 | 290 probes | 0.9584 |

A tuning costs about 2 s on GloVe, most of it the exact search for the truth; on bigger stores it uses fewer
queries (at most ~3e10 multiply-adds). The self-test checks it on held-out queries too.

## Live ingest: rows indexed as they come, the index rebuilt as it ages

`store_set_live(s, 1, 1.0f)` (or `vserv -L`) and nobody has to call `store_build` again:

- A thread of the store's own codes every added row into its closest cluster, 4096 rows at a time: coded off the
  lock, placed under the write lock. The rotation and the 4-bit codebook need no training, so an added row gets the
  same codes a build would give it (the self-test checks it: built + added rows probed over every cluster == a flat
  scan of all of them). Until a row is indexed, the exact scan of the pending tail finds it, as before.
- It rebuilds the index when it has aged: grown by `grow` since the last build (1.0 = doubled), a window of new rows
  sitting 1.25x farther from their centroids than the built rows did (the data moved), the cluster sizes 1.2x more
  uneven than at the build (new rows piling into a few clusters), or a quarter of it deleted. The rebuild runs beside
  the indexing, catches up on the rows that came in meanwhile and is swapped in under the write lock; searches use
  the old index until then. With a target recall set (`store_set_target_recall`) it re-tunes after each rebuild.
- Background work takes half the cores. `store_maintain` runs one step by hand, `store_live_stats` reports.
- Deletes no longer cost the saved index: the compaction renumbers the rows inside it, `store_save` writes added rows
  in the plain layout, and a load is still an mmap.

GloVe-100, `bench/live.c`, on the Cascade Lake host of [benchmarks.md](benchmarks.md). QPS: one batch of the 10k queries on 2
threads at the fastest probe count that reaches the recall (the same index measured twice differs by up to 10%):

| what the index holds | recall@10 >= 0.85 | >= 0.90 | >= 0.95 | >= 0.97 |
|---|---:|---:|---:|---:|
| one build over all 1,183,514 rows | 13,800 | 10,430 | 6,229 | 4,292 |
| 60% built, the other 40% added live | 13,707 (0.99x) | 8,858 (0.85x) | 5,745 (0.92x) | 3,806 (0.89x) |
| 74% built, 26% from regions the build never saw added live | 12,803 (0.93x) | 8,723 (0.84x) | 5,694 (0.91x) | 3,712 (0.86x) |
| 60% built, the 40% leaning one direction added live | 10,105 (0.73x) | 6,820 (0.65x) | 4,943 (0.79x) | 3,175 (0.74x) |
| the same after the rebuild live mode chose on its own (skew 1.51, 1.13 at the build) | 14,928 (1.08x) | 10,151 (0.97x) | 6,305 (1.01x) | 3,900 (0.91x) |

Before live mode the same 40% waited in the exact-scan tail: 96 QPS at recall@10 0.9589, against 5,393 at 0.9608
with them indexed (56x). Indexing them took 4.5 s on one core (105k rows/s).

Searching while rows stream in: one query at a time on one thread, 473k rows added by another thread at 50k rows/s,
live mode with rebuilds at +50%:

| phase | queries | p50 | p99 | max |
|---|---:|---:|---:|---:|
| idle, before | 3,000 | 0.42 ms | 0.71 ms | 4.7 ms |
| rows streaming in, indexed as they come | 10,517 | 0.61 ms | 2.63 ms | 15.6 ms |
| the rebuild live mode started at +50% (8.7 s) | 9,976 | 0.73 ms | 4.40 ms | 11.1 ms |
| after | 1,692 | 0.58 ms | 0.83 ms | 1.7 ms |

Before the rebuild ran beside the indexing (rows arriving meanwhile waited in the exact tail): p50 3.57 ms, p99 9.22
ms during it and p50 1.23 ms right after. A burst at full speed - 473k rows in 1.07 s, four times faster than one core
indexes - still fills the tail: p50 2.60 ms, p99 17.3 ms for the ~5 s until it is indexed.

Which signal fires where, measured: new rows drawn like the old ones leave the cluster-size skew where it was (1.142
-> 1.152); rows from 8 held-out Voronoi regions (26% of GloVe) raise it from 1.147 to 1.229 and sit only 3.6% farther
from their centroids - in 100 dimensions the remaining centroids still cover them, no rebuild is due, and the table
above shows they cost about what random rows do; rows leaning one direction raise it to 1.509, and live mode rebuilds.
On the self-test's 32-dimensional topics, rows from 30 new topics sit far from every centroid: two far windows, one
rebuild, recall@10 of their queries at 8 probes 0.763 -> 0.953.

Costs and limits: a rebuild holds the old and the new index at once (GloVe, 4-bit: +59 MB of codes; 100M rows, 2-bit:
+2.4 GB, too much for the 5.84 GiB box of the 100M run - keep `grow` at 0 there); rebuilding 100M rows takes minutes
(the build alone was 225 s); a burst faster than one core indexes waits in the exact tail; the growth and skew
baselines start over when a store is loaded.

## Scale: rows in chunks, past RAM

What capped the store before (measured, see Results): every f32 row sat in anonymous memory, and the arrays grew by
doubling into a second arena whose old copy was never released (RSS 1.5-3x the data), so the bash cgroup's 5.84 GiB
OOM-killed it between 7M and 8M rows of 128 dimensions - during ingest, before any index existed.

Now the rows live in column chunks of 65536 rows (vectors, doc ids, tags). A chunk never moves, so growth copies
nothing. Past 256 MB new chunks are pages of an unlinked spill file (`store_set_spill`, `$VECINDEX_SPILL`,
`/var/tmp` by default - use a disk, not a tmpfs), started on their way to disk as each fills (`sync_file_range`), so
the kernel drops them under pressure instead of swapping or OOM-killing. A loaded store points its chunks into the
.vxi mapping. What a search scans stays in RAM: the codes (d/2 bytes a row, d/4 in 2-bit mode), slot map and a
dead-row bitset. Re-scoring reads only the few hundred candidate rows a query needs; when those reads start missing
the page cache (major faults), the store asks for all of a query's rows at once (`MADV_WILLNEED`) instead of one
blocking fault after another - ~190k IOPS batched against ~21k one by one on this disk. Ids that arrive in increasing
order (the usual case) need no id map at all (binary search over the id column); only rows added out of order go
into an 8-byte-a-slot hash.

For stores past what RAM and disk allow at f32, two knobs: `store_set_codes(s, 2)` (2-bit codes: half the RAM,
twice the scan speed, re-score more candidates) and `store_set_vec_bytes(s, 1)` (int8 rows with an f32 scale: d + 4
bytes on disk instead of 4d, about one recall point on GloVe - the cost of 8 bits, a rotation first does not change
it). Measured trade-offs are in the Results tables.

## vecdb at scale (hashed embedder - no model server)

`bench/vbench1m.c` (`./.build/vbench1m N [SAVEDIR]`) adds N notes with links, tags and a body through the public API,
builds, queries, saves, and reloads. Per note vecdb used ~670 bytes of C structures on top of its vector; now 392
(500k notes, measured per structure): BM25 postings are varint-packed and kept inline while they fit in 8 bytes
(271 -> 54 B), string hashes have 12-byte slots at load 0.7 (terms 105 -> 84 B, titles 61 -> 42 B), node names
and link lists come from a slab with free lists (74 -> 57 B), and a node is 56 bytes instead of 64.

In the 5.84 GiB cgroup on 2 cores the old code died at about 4M notes; now 8M notes run end to end: ingest 146 s
(18 us/note, linear), index build 19 s, 3.6 GB anonymous memory after the build (the vectors stay file-backed),
hybrid search 7.7 ms/query with 300/300 exact-id hits, MCTS explore 53 ms/query.

On the old 12-core box (history, before the changes above): 1M notes in 1.48 GB RSS, and a swap-thrashing cliff
between 3.4M and 3.5M notes with ~3 GB of the machine taken by another process.

Real notes with real (larger) embeddings and a model server will cost more per note than this hashed-embedder
test - see the Wikipedia numbers above for that side of it.

Building this benchmark surfaced a footgun, now called out where `VecDB` is declared: a note's id is **not**
its insertion index. Tags and ghost nodes share the same id space, so `vecdb_add_note`'s return value can jump
ahead of a simple counter; always use the id it returns (or `vecdb_find`), never `id == the Nth call`.

## A real retrieval benchmark, not a hand-labeled one (real_val.c, SQuAD + AG-News)

The "Real notes check" above (20 hand-written questions, own labels) is informal - fine as a sanity check, not
a number to set next to a leaderboard. `bench/real_val.c` (`./run real_val`) is the closer-to-standard version:
**48,891 real passages** (18,891 unique SQuAD/Wikipedia contexts + 30,000 AG-News snippets as distractors,
`py/real_gen.py`) embedded with `sentence-transformers/all-MiniLM-L6-v2` (384-dim), scored against **1,000
real SQuAD questions** with their actual gold passage - a genuine asymmetric question -> passage retrieval
task (not synthetic near-duplicates), the same shape as the retrieval half of a BEIR-style eval even though
it is one dataset, not the full BEIR suite. Two numbers per config: `recall@10` (against exact-cosine ANN
ground truth - "did the index find what exhaustive search would have") and `gold@10` (against the real
answer - "was the actual right passage in the top 10", the metric that matters for RAG):

| config | recall@10 | gold@10 | latency |
|---|---|---|---|
| HNSW-vec (max accuracy) | 99.0% | 85.6% | 427 us/query |
| IVF q4fs (>=85% floor) | 92.2% | 83.1% | 2.45 ms/query |
| flat (max compression, 96 B/vec) | 80.2% | 83.6% | 4.43 ms/query |
| `store` (auto-tuned wrapper, what the CLI actually uses) | 97.1% | 83.5% | 6.11 ms/query |

The ceiling to compare against: exact cosine search with this SAME embedder only finds the gold passage in
its own top-10 **87.1%** of the time (measured directly from `py/real_gen.py`'s own output) - the embedder, not
the index, is what caps retrieval quality here. Every config above lands within 1.5-4 points of that ceiling,
and the best (HNSW) is within 1.5 points of it while beating it slightly on some runs (index construction and
the exact-cosine baseline use different tie-breaking, so being *at* the ceiling is the right way to read a
±1-2 point gap, not literally below it). Read honestly: this says the ANN index itself is close to lossless
relative to what a mid-tier 118M-param embedder can retrieve - it does **not** say retrieval quality is
state-of-the-art, since a stronger embedder would raise the 87.1% ceiling itself, which this benchmark cannot
show on its own - reranking (above) is a real, measured way to close *some* of that gap without swapping
embedders; see [comparison.md](comparison.md) for the honest full picture.

