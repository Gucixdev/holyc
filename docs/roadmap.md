# Roadmap

The owner's list and where each item stands (2026-10-30). "Not run" means written and compiled, tests and benchmarks
still to run on his machine.

Done and tested: quantization with an exact re-rank (4, 2 and 1-bit codes), SIMD (AVX-512, AVX2, NEON, a scalar path
elsewhere), hybrid BM25 search, concurrent updates (one rwlock, live indexing), background snapshots instead of
replication, a line protocol beside binary and HTTP, system logs instead of users and roles, any language through
the C ABI and the protocols, i64 ids, one CPU knob, texts with chunk ranges, vfind.

Done, not run: memory tiers (hot codes in RAM, cold rows on disk, mlock), tenants (`vserv DIR`, `vi_dir_*`), write
batches (all or none, CRC-32C journal records, fsync on commit), the cascade (1-bit scan, 4-bit shortlist, exact
re-rank: `vi_set_cascade`, `vserv -C`, bench methods `vecindex-cascade` and `-i8`; `tests/store_test.c` checks the
derived 1-bit codes against a 1-bit build byte for byte, added rows, recall against the 4-bit scan, the file).

Open:
- the cascade's benchmark, and its shortlist tuned rather than fixed at 8 x R
- competitors for docs/comparison.md: MongoDB Atlas Vector Search, VAST Data, KIOXIA AiSAQ (SSD-resident ANN),
  "ubase" (not identified yet) - none checked yet
- streaming chunking; lazy loading beyond what mmap and tenants opened on first use already do
- checksums per block, ZFS-style: cryptographic (BLAKE3 or SHA-256) to catch bitrot, checked on read and by a scrub
- versioning: older snapshots kept and readable
- compression of rows and texts
- building the index from the quantized vectors, without a full-precision pass, to cut serving cost
- SSD and SATA disks as the index's home, not only RAM (cold rows are on disk already; codes read by block next)
- no ceiling on scale: hierarchical clustering (IVF over IVF), shards as files
- the constraint over all of it: nothing that needs an IT department to run
