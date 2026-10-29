# How this compares to mainstream RAG/vector-DB stacks

Asked directly, honestly, not as a sales pitch:

**Ahead of what most RAG stacks offer:**
- **mem.c's memory architecture is not something Pinecone/Weaviate/Qdrant/Milvus have at all** -
  they are ANN indexes plus a metadata filter. Decay/half-life, metamemory (a real "I don't
  know"), event->fact->concept promotion via a deterministic rule engine is usually hand-rolled
  today with an LLM call doing the summarising (Mem0, Letta/MemGPT-style agent memory). Doing it
  with zero LLM calls (a small forward-chaining s-expression interpreter over real vector
  similarity) is a genuine, unusual design choice - cheaper, deterministic, and testable in a way
  an LLM-summarised memory store is not.
- **The graph (wikilinks/backlinks/tags/ghost nodes, MCTS bridge-note exploration) is native**,
  not bolted on as metadata filters - most vector DBs have no equivalent.
- **Engineering rigor unusual for a solo/embedded project**: atomic cross-file writes (the
  generation-suffix fix), TSan-clean concurrency at two layers, adversarial stress tests with real
  invariants that found real bugs (not just "does it crash" fuzzing), a rule-parser fuzzer that
  found a real stack-overflow crash on its first run. This is closer to how a production index
  engine gets built than a typical tutorial RAG project.
- **Embedded, zero dependencies, single C89 file per layer** - and an optional server (`src/vserv.c`,
  2.4 MB idle, TLS through vendored BearSSL) when it has to sit behind a socket. A real advantage
  where self-hosted/offline/small-footprint matters, which a hosted vector DB cannot give you.
- **Speed at the same recall, build and load.** On GloVe-100 (Results) it answers 1.4-1.8x more
  queries per second than FAISS's IVF-PQ fast-scan in a batch on 2 threads, and 1.65-2.5x one query
  at a time, at every recall@10 from 0.85 to 0.97. The index builds in 8.0 s where FAISS IVF-PQ takes 124.5 s, and a saved index is ready in 7 ms
  (mmap) where FAISS needs half a second.

**Behind, honestly, no hedging:**
- **Scale.** Single-node only - no sharding/replication. Measured, not guessed: 100M vectors on one
  2-core box with a 5.84 GiB memory limit (compact mode), 8M notes through the whole vecdb layer;
  past one machine's disk there is nothing. Milvus/Qdrant/Pinecone scale across machines, to
  billions of vectors.
- **Memory and build time against a codes-only index.** turbovec keeps nothing but 4-bit codes (56
  B/vec on GloVe) and builds in 2.2 s with no clustering; vecindex keeps the rows to re-score, so its
  smallest mode is 153 B/vec (2-bit codes + int8 rows) and its build is 8 s. turbovec tops out at
  recall@10 0.85 on GloVe and scans every code of every row per query. No graph index here either.
- **Embedding quality.** The default is `multilingual-e5-small` (118M params) - mid-tier. Larger
  open models (e5-mistral-7b, NV-Embed) and commercial APIs (OpenAI text-embedding-3-large,
  Voyage, Cohere) score meaningfully higher on standard retrieval benchmarks. `bench/real_val.c` ([vecdb.md](vecdb.md))
  gives a real number for THIS repo on a real SQuAD-based task (gold@10 up to 85.6%, near the
  87.1% ceiling this specific embedder allows) - not a claim of beating stronger embedders, a
  measurement of how close to lossless the ANN index itself is relative to whatever embedder
  it's given.
- **No reranker, until this round** - now real (`src/rerank.c`/`py/rerank_srv.py`, measured +19.4 points
  gold@1 on the SQuAD subset where it has room to help - see [vecdb.md](vecdb.md)). Still only one reranker
  model wired up and tested (`cross-encoder/ms-marco-MiniLM-L-6-v2`, English-only); a multilingual
  swap (`cross-encoder/mmarco-mMiniLMv2-L12-H384-v1` or similar) is not itself tested here yet.
- **No multi-tenancy/auth/replication/HA** - this is a library, not a hosted service. Out of
  scope for what it is, not a bug, but a real category difference from a managed vector-DB
  product.
- **No independent review, no production traffic.** Every test in this repo was written and run
  by the same person who wrote the code being tested. Real bugs were found this way (the whole
  point of writing real tests instead of just reading the code), but "no known gaps" is not the
  same claim as "reviewed/battle-tested by others."

**Net:** as a *vector search engine*, this is a solid, well-tested single-node library that, on one
machine, builds and loads far faster than FAISS and Qdrant and searches faster at high recall - it does
not compete with Pinecone/Qdrant on multi-machine scale or with top embedding APIs on raw retrieval
quality, and says so with real numbers rather than assuming either way. As a *memory layer for an agent* (event
lifetime, decay, metamemory, deterministic consolidation), it does something none of the
mainstream RAG/vector-DB products offer natively - which is the more interesting half of this
project, not "does it beat Qdrant."
