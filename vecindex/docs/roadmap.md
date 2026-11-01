# Roadmap

The owner's list and where each item stands (2026-11-01). "Not run" means written and compiled, tests and benchmarks
still to run on his machine.

Scope: vecindex is the vector database alone, with no dependencies, built as one project. The note database, agent
memory (mem.c and its rule engine) and the cross-encoder re-rank moved to `agent_launcher/` beside it in holyc,
a separate project that uses vecindex as a library; vecindex itself is back in holyc as `vecindex/`.

Done and tested: quantization with an exact re-rank (4, 2 and 1-bit codes), SIMD (AVX-512, AVX2, NEON, a scalar path
elsewhere), hybrid BM25 search, concurrent updates (one rwlock, live indexing), background snapshots instead of
replication, a line protocol beside binary and HTTP, system logs instead of users and roles, any language through
the C ABI and the protocols, i64 ids, one CPU knob, texts with chunk ranges, vfind.

Done, not run: memory tiers (hot codes in RAM, cold rows on disk, mlock), tenants (`vserv DIR`, `vi_dir_*`), write
batches (all or none, CRC-32C journal records, fsync on commit), integrity (BLAKE3 digests of every 64 KiB block, Reed-Solomon parity - 2 for every
32 blocks, interleaved - the table of digests twice, checks at load that rebuild and write back, a scrub, a CRC-32C on
every journal record, `vi_scrub`, `vserv` `scrub`), the cascade (1-bit scan, 4-bit shortlist, exact
re-rank: `vi_set_cascade`, `vserv -C`, bench methods `vecindex-cascade` and `-i8`; `tests/store_test.c` checks the
derived 1-bit codes against a 1-bit build byte for byte, added rows, recall against the 4-bit scan, the file), the
store self-test under ASan and UBSan in `./run`.

Open:
- tags: one 64-bit tag per row today (`vi_search_tagged`). Tags are the structure: hierarchy, relation and
  correlation between rows, several per row, ranges and combined conditions in a filter
- the file format: zones (parts of a store split by id range, time or tag) and namespaces inside a tenant, one
  format readable on every machine (the journal is written in the host's byte order today)
- scale at both ends: the smallest boards (fewest bytes a vector, RAM to spare) and past one machine's RAM
  (hierarchical clustering - IVF over IVF - and shards as files, no ceiling)
- p99 under a steady write load: a burst faster than one core indexes waits in the exact-scan tail, and a rebuild
  holds two indexes; index bursts on more cores and bound the tail
- the cascade's benchmark, and its shortlist tuned rather than fixed at 8 x R
- head-to-head runs: Qdrant (HNSW) and LEANN are in the harness (`py/bench.py`, `bench/run_all.sh`) but have not
  finished a run; Qdrant's TurboQuant mode, the RaBitQ engines (Milvus, LanceDB) and USearch are not in it yet;
  a graph index at very high recall
- streaming chunking; lazy loading beyond what mmap and tenants opened on first use already do
- integrity, next: pages read back from the disk after the load are not checked again (a scrub is); SIMD for BLAKE3
  and the GF(2^8) parity; parity encoding on several threads inside a snapshot child
- versioning: older snapshots kept and readable
- compression of rows and texts
- building the index from the quantized vectors, without a full-precision pass, to cut serving cost
- SSD and SATA disks as the index's home, not only RAM (cold rows are on disk already; codes read by block next)
- a fuzzer for the store's file loader and the server's protocols (the fuzzers left with the agent layer)
- packages over the C ABI for Python, Rust and Go (the bindings in `docs/api.md` are run by hand)
- the constraint over all of it: nothing that needs an IT department to run
