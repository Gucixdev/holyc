/* vecindex.h - the C API of vecindex.
 *
 * One opaque handle per store, plain C types, a status or a count from every call. Nothing else is exported from
 * the library, so any language with a C foreign-function interface can bind it without a compiler: Python ctypes,
 * Go cgo, Rust extern "C", Node koffi or N-API, Java FFM (Panama), C# P/Invoke, Zig @cImport, Lua FFI.
 *
 *   ./run lib                      .build/libvecindex.so and .build/libvecindex.a, then tests/api_test.c against both
 *   cc -Iinclude app.c -L.build -lvecindex -lm -lpthread
 *
 * Vectors are float[dim], any length: they are stored unit-normalised and compared by inner product (cosine). Scores
 * are -|q - v|^2 of the unit vectors, so 0 is identical and -4 opposite; ids are yours, any 64-bit value.
 *
 * Threads: every function may be called from any thread on the same handle at once. Searches run side by side;
 * writes (add, delete, set_tag, build, save) are serialised against them by one readers-writer lock.
 *
 * Durability: a store opened from a file keeps a journal of every add and delete next to it (PATH.j). vi_sync flushes
 * the journal to the disk; vi_save rewrites the file and starts a fresh journal; vi_snapshot does the same from a
 * child process while this one goes on serving. Opening the file again replays the journal. */
#ifndef VECINDEX_H
#define VECINDEX_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define VI_API __declspec(dllexport)
#elif defined(__GNUC__)
#define VI_API __attribute__((visibility("default")))
#else
#define VI_API
#endif

#if defined(__INT64_TYPE__) && defined(__GNUC__)
__extension__ typedef __INT64_TYPE__ vi_i64;   /* long long on 32-bit targets, fine under -std=c89 -pedantic */
#elif defined(_MSC_VER)
typedef __int64 vi_i64;
#else
#include <stdint.h>
typedef int64_t vi_i64;
#endif

typedef struct vi_db vi_db;

#define VI_VERSION "0.3.0"

/* flags for vi_open */
#define VI_LIVE 1        /* index added rows as they come and rebuild the index as it ages, from a thread of its own */

/* The library's version, VI_VERSION of the build. */
VI_API const char *vi_version(void);

/* Opens the store in the file at path, or creates it with dim dimensions when there is no such file (dim 0: it must
 * exist). A file that exists but cannot be read is an error, never silently replaced. path NULL: a store in memory
 * only. Returns NULL on failure. */
VI_API vi_db *vi_open(const char *path, int dim, int flags);

/* Flushes the journal to the disk (as vi_sync), waits for a running snapshot, and frees the handle. It does not
 * rewrite the file: the next vi_open replays the journal. */
VI_API void vi_close(vi_db *db);

/* Dimensions; live rows (added and not deleted). */
VI_API int vi_dim(const vi_db *db);
VI_API vi_i64 vi_count(vi_db *db);

/* Adds a row, or replaces the row with this id (one change: a crash leaves the old row or the new one, never
 * neither). 0 on success, -1 when the vector has a NaN or an infinity (it is refused) or memory ran out. */
VI_API int vi_add(vi_db *db, vi_i64 id, const float *vec);

/* vi_add for n rows (vecs: n x dim, row after row); returns how many went in. */
VI_API int vi_add_batch(vi_db *db, int n, const vi_i64 *ids, const float *vecs);

/* Removes the row with this id; returns 1, or 0 when there is none. */
VI_API int vi_delete(vi_db *db, vi_i64 id);

/* Removes every row whose id is in [lo, hi] - all the chunks of a document when ids are doc << 20 | chunk;
 * returns how many. */
VI_API int vi_delete_range(vi_db *db, vi_i64 lo, vi_i64 hi);

/* The stored (unit-normalised) vector of id into out[dim]: 0, or -1 when there is no such row. */
VI_API int vi_get(vi_db *db, vi_i64 id, float *out);

/* vi_add with a text kept beside the row (a chunk of a document, a note: any bytes, len of them, at most 16 MB - 1;
 * len -1: a 0-terminated string). The text comes back with vi_get_text and feeds the words side of
 * vi_search_hybrid. 0, or -1 as vi_add (or a bad length). */
VI_API int vi_add_text(vi_db *db, vi_i64 id, const float *vec, const char *text, int len);

/* The text of id: its length (0: the row has none), the first min(length, cap) bytes copied into buf, no 0 added;
 * -1 when there is no such row. A length above cap means buf was too small: call again with a bigger one. */
VI_API int vi_get_text(vi_db *db, vi_i64 id, char *buf, int cap);

/* The ids in [lo, hi] that have a row, ascending: the smallest max of them into ids; returns how many (-1: out of
 * memory). With ids made as doc << 20 | chunk, vi_range(db, doc << 20, (doc << 20) | 0xfffff, ...) lists a
 * document's chunks: search the small chunks, then hand the model the chunks around a hit, or the whole document.
 * For more than max, call again from the last id + 1. */
VI_API int vi_range(vi_db *db, vi_i64 lo, vi_i64 hi, vi_i64 *ids, int max);

/* A 64-bit tag on a row, for filtered search: 0, or -1 when there is no such row. */
VI_API int vi_set_tag(vi_db *db, vi_i64 id, vi_i64 tag);
VI_API int vi_get_tag(vi_db *db, vi_i64 id, vi_i64 *tag);

/* The k nearest rows to query: their ids and scores into ids[k] and scores[k], best first; returns how many. Rows
 * added since the last build are found too (an exact scan of them, or the live index). */
VI_API int vi_search(vi_db *db, const float *query, int k, vi_i64 *ids, float *scores);

/* The same, over rows whose tag has every bit of all set and, when any is not 0, at least one bit of any. */
VI_API int vi_search_tagged(vi_db *db, const float *query, int k, vi_i64 all, vi_i64 any, vi_i64 *ids, float *scores);

/* Hybrid search: the vector query (NULL: words only) and the words of text (tlen bytes, -1: 0-terminated; NULL: the
 * vector only) against the rows' texts (BM25), fused by rank - so an exact name, a number or a rare word is found
 * even where the embedding blurs it. ids and scores (the fused score, bigger = better) get up to k hits; returns how
 * many. The first such search builds the word index over every text; later writes keep it current. */
VI_API int vi_search_hybrid(vi_db *db, const float *query, const char *text, int tlen, int k, vi_i64 *ids, float *scores);

/* nq queries (queries: nq x dim) on up to threads threads (0: every core); ids and scores get k slots a query, a
 * missing hit has id -1. */
VI_API void vi_search_batch(vi_db *db, const float *queries, int nq, int k, vi_i64 *ids, float *scores, int threads);

/* Builds the index over every row (searches before it work, by exact scan). */
VI_API void vi_build(vi_db *db);

/* Keeps recall@10 at target (e.g. 0.95): every build measures recall against an exact search and picks the fewest
 * probes that reach it. 0 turns it off. */
VI_API void vi_set_recall(vi_db *db, float target);

/* Memory per row before the first row is added: code_bits 4 (default) or 2 for what a search scans; row_bytes 4
 * (default, f32 rows, the most precise re-scoring) or 1 (int8 rows, a quarter of the room). Returns 0, or -1 when
 * the store already has rows or a value is not one of these. */
VI_API int vi_set_layout(vi_db *db, int code_bits, int row_bytes);

/* Writes the whole store to path (NULL: the file it was opened from) and starts a fresh journal: 0 or -1. */
VI_API int vi_save(vi_db *db, const char *path);

/* Flushes the journal of the file the store was opened from to the disk (the first call on a store that has no file
 * yet writes it whole): 0 or -1. */
VI_API int vi_sync(vi_db *db);

/* Writes the store to path (NULL: its own file) from a child process, while this one goes on answering searches and
 * taking writes: 0 started, 1 one is already running, -1 failed. vi_snapshot_wait tells how it ended. */
VI_API int vi_snapshot(vi_db *db, const char *path);

/* The last snapshot: 0 done, 1 still running, -1 failed, 2 none yet. block 1 waits for a running one. */
VI_API int vi_snapshot_wait(vi_db *db, int block);

#ifdef __cplusplus
}
#endif

#endif /* VECINDEX_H */
