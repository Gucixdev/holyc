/* store.c - a RAG-shaped vector store on top of engine.c.
 *
 * The engine is an index KERNEL (row ids, no persistence, immutable).
 * A RAG corpus needs your own doc ids on results, incremental add,
 * delete, metadata filtering, concurrent readers, bad-input rejection,
 * and surviving a restart without rebuilding. This wraps all of that
 * around the engine without touching it.
 *
 *   store_add(_meta) / store_add_batch : append (vector, doc_id[, meta]),
 *       unit-normalised; a vector with a NaN / Inf component is rejected
 *       (return -1), never poisons the index.
 *   store_delete           : tombstone a doc_id.
 *   store_build            : compact + rebuild the committed index over
 *       every live vector; clears the pending tail.
 *   store_search           : committed index + an exact scan of the not-
 *       yet-built pending tail, merged (same -||q-v||^2 scale); returns
 *       YOUR doc ids, skips tombstones. Safe to call from many threads.
 *   store_search_filter    : same, keeping only rows whose i64 meta tag
 *       matches (all_bits set) and (any_bits, if non-zero, intersect).
 *   store_save / store_load : the compacted vectors + doc ids + meta +
 *       build goal AND the built index itself to one .vxi file, so load
 *       is O(file), not O(rebuild).
 *
 * Memory, so a store can grow far past RAM:
 *   - rows live in column chunks of 65536 rows (vectors, doc ids, tags). A chunk never moves, so growing never
 *     copies anything (no realloc spike, no second copy left behind) and row pointers stay valid;
 *   - past STORE_SPILL_MIN bytes new chunks are pages of an unlinked file (store_set_spill; /var/tmp by default)
 *     instead of anonymous memory: the kernel writes them back and drops them under pressure instead of
 *     swapping or OOM-killing. A loaded store points its chunks straight into the .vxi mapping;
 *   - what a search scans (the 4-bit codes, the dead-row bitset) stays in RAM: d/2 bytes + 1 bit a row. The
 *     f32 rows are only read for the few hundred candidates re-scored per query; when those reads start missing
 *     the page cache, the kernel is asked for all of them at once (MADV_WILLNEED) instead of one fault at a time;
 *   - doc ids that arrive in increasing order (the usual case) need no id map at all: lookups binary-search the
 *     id column. Only rows added out of order go into a hash of row numbers (8 bytes a slot);
 *   - for the largest stores two knobs trade a little recall for a lot of room (store_set_codes,
 *     store_set_vec_bytes): 2-bit codes (d/4 bytes a row in RAM instead of d/2) and int8 rows on disk (d + 4
 *     bytes instead of 4d). 100M rows of 96 dimensions then need 2.8 GB of RAM and 11.6 GB of disk.
 *
 * Self-tuning: store_set_target_recall(s, 0.95) and the store keeps that recall@10 on its own - every build
 * measures recall against exact search and picks the fewest IVF probes that reach it, on the queries it has
 * actually been answering (a reservoir of the last STORE_QLOG) or, before there are enough, on its own rows;
 * store_tune re-measures on demand, as the data or the traffic drift.
 *
 * Live mode: store_set_live(s, 1, grow) and nobody has to call store_build again. A thread of the store's own codes
 * each added row into its closest cluster (until then the pending tail's exact scan finds it); when the index has
 * aged - grown by `grow`, a window of new rows sitting far from every centroid (the data moved), a quarter of it
 * deleted - it rebuilds on a snapshot of the rows while searches go on with the old index, and swaps the new one in;
 * with a target recall set it re-tunes after each. The work runs on half the cores. store_maintain runs one such
 * step by hand. store_save writes the appended rows into the file's plain layout, and deletes no longer cost the
 * saved index: compaction renumbers the rows inside it instead of dropping it.
 *
 * Concurrency: an rwlock - store_search(_filter) take the read lock and
 * touch no shared mutable state (own malloc scratch, engine_search_into
 * path) except the query reservoir, behind its own mutex that a search only
 * try-locks (a busy reservoir skips the query, nobody waits); everything
 * that mutates takes the write lock.
 *
 *   gcc -std=c89 -pedantic -Wall -Wextra -D_POSIX_C_SOURCE=200112L -O2 \
 *       -march=native -Isrc tests/store_test.c -lm -lpthread -o store_test && ./store_test
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE     /* pthread_rwlockattr_setkind_np, sync_file_range, RUSAGE_THREAD */
#endif
#define ARENA_FLAT_RESERVE_BYTES ((size_t)16 << 30)
#define ENGINE_THREADS
#define ENGINE_SERIALIZE
#define PRINT_IMPLEMENTATION
#include "engine.c"
#include "print.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/resource.h>
#include "hdict.c"
#include "tq.c"

#define STORE_HAS_SPILL 1

/* From this many rows the TurboQuant scan is grouped into clusters (IVF) and a query scans only the closest few.
 * Below it a flat scan is fast enough (200k rows: ~1.3 ms) and always exact-ish. The cluster count follows the usual
 * rule, 4 sqrt(rows) (1.2M rows: 4351 clusters of ~270; 100M: 40000 of 2500), but no cluster is planned smaller
 * than STORE_IVF_ROWS rows. Finer clusters than the old fixed 2000 rows: 30-40% more queries a second at the same
 * recall on GloVe-100, and a faster build. */
#ifndef STORE_IVF_MIN
#define STORE_IVF_MIN 600000
#endif
#ifndef STORE_IVF_ROWS
#define STORE_IVF_ROWS 256
#endif
static i32 store_ivf_min_ = STORE_IVF_MIN;   /* the self-test lowers it to reach the IVF paths on a small store */
static i32 store_nclusters_(i32 n)
{
    i32 a = (i32)(4.0 * eng_sqrt_((f64)n)), b = n / STORE_IVF_ROWS;
    return n < store_ivf_min_ ? 0 : (a < b ? a : b);
}
#ifndef STORE_QLOG
#define STORE_QLOG 256      /* queries kept for tuning (a reservoir sample of everything answered) */
#endif
#ifndef STORE_SPILL_MIN
#define STORE_SPILL_MIN ((i64)256 << 20)   /* column bytes kept in anonymous memory before chunks go to the spill file */
#endif

/* exact re-scoring touches rows at random: ask for the row a few candidates ahead while scoring this one */
#define STORE_PF_AHEAD 4
static void store_prefetch_row_(const f32 *row, i32 dim)
{
#if defined(__GNUC__)
    const char *p = (const char *)row, *end = (const char *)(row + dim);
    for (; p < end; p += 64) __builtin_prefetch(p, 0, 1);
#else
    (void)row; (void)dim;
#endif
}

#define STORE_MAGIC   0x34584956u   /* 'VIX4' - v4 = goal gains max_candidates */
#define STORE_VERSION 5             /* v5: 64-byte aligned sections, the scan index saved too, loaded by mmap */
#define STORE_VERSION_I8 6          /* v6: v5 + a 16-byte extension after the header (int8 rows) and their scales */

/* ---- column chunks ---- */
#define STORE_CSHIFT 16                           /* 65536 rows a chunk */
#define STORE_CROWS  ((i64)1 << STORE_CSHIFT)
enum { STORE_ANON = 0, STORE_SPILL = 1, STORE_MAPPED = 2 };

typedef struct {
    u8   **chunk;       /* [nchunk] chunk base pointers */
    u8    *how;         /* [nchunk] STORE_ANON / STORE_SPILL (offset in foff) / STORE_MAPPED (inside the loaded file) */
    i64   *foff;        /* [nchunk] spill-file offset of a STORE_SPILL chunk */
    i32    nchunk, ccap;
    size_t esz;         /* bytes a row */
    size_t elem;        /* bytes a scalar (4: f32 rows, 8: i64), for the little-endian file format */
} SCol;
#define SCOL_AT(c, r) ((c)->chunk[(size_t)(r) >> STORE_CSHIFT] + ((size_t)(r) & (size_t)(STORE_CROWS - 1)) * (c)->esz)

/* doc id -> row for rows added out of order: open addressing over row numbers (row + 1, 0 = empty) with a 32-bit
 * tag of the id's hash beside it, so a probe reads the id column only when the tags match */
typedef struct { u32 *row, *tag; u32 cap, n, bits; } SIdx;

typedef struct {
    FILE    *jf;            /* write-ahead journal of adds/deletes since the last full save (store_sync) */
    char    *jpath;
    i32      tq_probe;      /* IVF: clusters scanned per query (0 = the index default) */
    i32      tq_threads;    /* threads one query's scan may use (0 = auto: up to 4 on big stores) */
    i32      tq_rerank;     /* candidates re-scored exactly per query (0 = auto: max(20 k, 256)) */
    f32      tune_target;   /* recall@10 every build keeps by choosing the probes (0 = off) */
    f32      tune_recall;   /* what the last tuning measured (-1: never tuned) */
    f32     *qlog;          /* [STORE_QLOG x dim] a reservoir sample of the queries answered, unit length */
    i32      qlog_n;
    i64      qlog_seen;
    u32      qlog_rng;
    pthread_mutex_t qlog_mu;
    i32      tuned_n;       /* rows in the index when it was last tuned (0: not tuned since it was built) */
    i32      tuned_log;     /* that tuning ran on logged traffic (not on the store's own rows) */
    /* live mode (store_set_live): a thread of the store's own indexes new rows and rebuilds the index as it ages */
    i32      live_on;
    f32      live_grow;     /* rebuild once the rows added since the build reach this share of the rows built; 0: never */
    pthread_t mt;
    pthread_mutex_t mt_mu;  /* held through one maintenance step; store_build / store_save / store_tune take it too */
    pthread_mutex_t rb_mu;  /* held by a rebuild from its snapshot to its swap: store_build / store_save wait for it */
    pthread_t rb_th;        /* the rebuild running beside the live thread (rb_live: not joined yet) */
    i32      rb_live;
    pthread_mutex_t mt_cmu; /* with mt_cv: wakes the thread (rows pending, or stop) */
    pthread_cond_t  mt_cv;
    i32      mt_stop;
    i32      dead_built;    /* tombstones the last rebuild already left out of the index */
    i64      mods, rebuilt_mods;      /* adds + deletes so far, and when the last rebuild started */
    f64      win_sum;       /* the added rows' squared distances to their centroids, this window */
    i32      win_n, drifted;            /* rows in the window; windows that sat too far since the build */
    i64      n_indexed, n_rebuilds, rebuild_us;   /* what maintenance did (store_live_stats) */
    i64      rebuilding;    /* 1 while a live rebuild runs */
    Tq      *tq;            /* fast mode: the TurboQuant-style index */
    i32      fast;          /* 1: store_build skips the auto-tuner (engine_build_fast) */
    f32      fast_target;   /* recall@k the fast build grows nprobe to (default 0.9) */
    Arena   *arena;         /* the RagStore struct lives here */
    Arena    idx_arena;     /* committed engine index; arena_reset each build/load */
    i32      dim;

    SCol     vec, idc, metac;   /* the rows: unit vectors, caller doc ids, caller i64 tags (0 = none) */
    SCol     vsc;           /* int8 rows only: the f32 scale of each row */
    i32      vbytes;        /* 4: f32 rows; 1: int8 rows (each row times its scale) */
    i32      codebits;      /* 4 or 2: the scan index's code width (store_set_codes) */
    f32     *addbuf;        /* [dim] the row being added, normalised (under the write lock) */
    u64     *dead;          /* tombstones, one bit a row */
    i64      dead_words;
    i32      live;          /* rows that are not tombstoned */
    i32      count;
    i32      built_upto;    /* [0, built_upto) is in the index; the rest pends */
    EngIndex *idx;
    EngGoal  goal;
    pthread_rwlock_t lock;
    void    *map;           /* store_load's private copy-on-write mapping of the file (columns and tq point in) */
    size_t   map_len;

    i32      idmap_ok;      /* 0: nsorted / ids_h / dup are rebuilt on the next lookup (after a load or a compaction) */
    i32      nsorted;       /* rows [0, nsorted) carry strictly increasing ids: found by binary search, nothing stored */
    SIdx     ids_h;         /* the other rows: id -> newest row */
    H64      dup;           /* row -> the previous row with the same id (only rows that repeat an id) */

    char    *spill_dir;     /* NULL: never spill */
    i64      spill_min;     /* spill once the columns hold this many bytes */
    int      spill_fd;      /* -1 until the first spilled chunk */
    i64      spill_off;
    i64      col_bytes;     /* bytes of column chunks allocated so far */
    i32      pf_cold;       /* re-scoring missed the page cache lately: prefetch candidates with MADV_WILLNEED */
    u32      pf_tick;
} RagStore;

/* row accessors (also for tests and tools) */
#define STORE_VEC(s, r)  ((f32 *)(void *)SCOL_AT(&(s)->vec, r))      /* f32 stores only (vbytes == 4) */
#define STORE_I8(s, r)   ((i8 *)(void *)SCOL_AT(&(s)->vec, r))
#define STORE_SC(s, r)   (*(f32 *)(void *)SCOL_AT(&(s)->vsc, r))
#define STORE_ID(s, r)   (*(i64 *)(void *)SCOL_AT(&(s)->idc, r))
#define STORE_META(s, r) (*(i64 *)(void *)SCOL_AT(&(s)->metac, r))
#define STORE_DEAD(s, r) ((i32)(((s)->dead[(size_t)(r) >> 6] >> ((u32)(r) & 63u)) & 1u))
#define STORE_KILL(s, r) ((s)->dead[(size_t)(r) >> 6] |= (u64)1 << ((u32)(r) & 63u))

static TqCol store_tqcol_(const SCol *c)
{
    TqCol t;
    t.chunk = c->chunk; t.shift = STORE_CSHIFT; t.esz = c->esz; t.i8 = 0; t.dim = 0; t.scale = (const TqCol *)0;
    return t;
}

/* the vector rows as the index builder reads them (int8 rows carry their scale column along) */
static TqCol store_vcol_(const RagStore *s, TqCol *sc)
{
    TqCol t = store_tqcol_(&s->vec);
    if (s->vbytes == 1) { *sc = store_tqcol_(&s->vsc); t.i8 = 1; t.dim = s->dim; t.scale = sc; }
    return t;
}

/* ---- chunk allocation: anonymous memory, or pages of the spill file once the store is big ---- */

static void store_spill_open_(RagStore *s)
{
    char *tmpl;
    int fd;
    if (!s->spill_dir) return;
    tmpl = (char *)malloc(strlen(s->spill_dir) + 32);
    sprintf(tmpl, "%s/vecindex-spill-XXXXXX", s->spill_dir);
    fd = mkstemp(tmpl);
    if (fd >= 0) { unlink(tmpl); fcntl(fd, F_SETFD, FD_CLOEXEC); s->spill_fd = fd; s->spill_off = 0; }
    else { free(s->spill_dir); s->spill_dir = (char *)0; }      /* no usable directory: stay in memory */
    free(tmpl);
}

static u8 *store_chunk_alloc_(RagStore *s, size_t bytes, u8 *how, i64 *foff)
{
    void *p;
    if (s->spill_dir && s->col_bytes + (i64)bytes > s->spill_min) {
        if (s->spill_fd < 0) store_spill_open_(s);
        if (s->spill_fd >= 0 && ftruncate(s->spill_fd, (off_t)(s->spill_off + (i64)bytes)) == 0) {
            p = mmap((void *)0, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, s->spill_fd, (off_t)s->spill_off);
            if (p != MAP_FAILED) {
                *how = STORE_SPILL; *foff = s->spill_off;
                s->spill_off += (i64)bytes; s->col_bytes += (i64)bytes;
                return (u8 *)p;
            }
        }
    }
    p = mmap((void *)0, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return (u8 *)0;
    *how = STORE_ANON; *foff = -1;
    s->col_bytes += (i64)bytes;
    return (u8 *)p;
}

static i32 scol_push_(RagStore *s, SCol *c)
{
    u8 *p, how;
    i64 off;
    if (c->nchunk == c->ccap) {
        i32 nc = c->ccap ? c->ccap * 2 : 16;
        c->chunk = (u8 **)realloc(c->chunk, (size_t)nc * sizeof(u8 *));
        c->how = (u8 *)realloc(c->how, (size_t)nc);
        c->foff = (i64 *)realloc(c->foff, (size_t)nc * sizeof(i64));
        c->ccap = nc;
    }
    p = store_chunk_alloc_(s, (size_t)STORE_CROWS * c->esz, &how, &off);
    if (!p) return -1;
    c->chunk[c->nchunk] = p; c->how[c->nchunk] = how; c->foff[c->nchunk] = off;
    ++c->nchunk;
    return 0;
}

/* a chunk that points into the loaded file is read-mostly: before rows are appended to it, copy it out */
static i32 scol_unshare_(RagStore *s, SCol *c, i32 ci, i64 rows_in_use)
{
    u8 *p, how;
    i64 off;
    if (ci >= c->nchunk || c->how[ci] != STORE_MAPPED) return 0;
    p = store_chunk_alloc_(s, (size_t)STORE_CROWS * c->esz, &how, &off);
    if (!p) return -1;
    memcpy(p, c->chunk[ci], (size_t)rows_in_use * c->esz);
    c->chunk[ci] = p; c->how[ci] = how; c->foff[ci] = off;
    return 0;
}

static void scol_free_(SCol *c)
{
    i32 i;
    for (i = 0; i < c->nchunk; ++i)
        if (c->how[i] != STORE_MAPPED) munmap(c->chunk[i], (size_t)STORE_CROWS * c->esz);
    free(c->chunk); free(c->how); free(c->foff);
    c->chunk = (u8 **)0; c->how = (u8 *)0; c->foff = (i64 *)0; c->nchunk = c->ccap = 0;
}

/* rows [0, need) addressable in every column, the bitset long enough. 0 ok, -1 out of memory */
static i32 store_reserve_(RagStore *s, i64 need)
{
    i32 nch = (i32)((need + STORE_CROWS - 1) >> STORE_CSHIFT), ci;
    i64 words = (need + 63) >> 6;
    while (s->vec.nchunk < nch) if (scol_push_(s, &s->vec)) return -1;
    while (s->idc.nchunk < nch) if (scol_push_(s, &s->idc)) return -1;
    while (s->metac.nchunk < nch) if (scol_push_(s, &s->metac)) return -1;
    while (s->vbytes == 1 && s->vsc.nchunk < nch) if (scol_push_(s, &s->vsc)) return -1;
    ci = (i32)((i64)s->count >> STORE_CSHIFT);                   /* the chunk the next row lands in */
    if (scol_unshare_(s, &s->vec, ci, (i64)s->count - ((i64)ci << STORE_CSHIFT))
        || scol_unshare_(s, &s->idc, ci, (i64)s->count - ((i64)ci << STORE_CSHIFT))
        || scol_unshare_(s, &s->metac, ci, (i64)s->count - ((i64)ci << STORE_CSHIFT))
        || (s->vbytes == 1 && scol_unshare_(s, &s->vsc, ci, (i64)s->count - ((i64)ci << STORE_CSHIFT)))) return -1;
    if (words > s->dead_words) {
        i64 nw = s->dead_words ? s->dead_words * 2 : 1024;
        u64 *d;
        while (nw < words) nw *= 2;
        d = (u64 *)realloc(s->dead, (size_t)nw * sizeof(u64));
        if (!d) return -1;
        memset(d + s->dead_words, 0, (size_t)(nw - s->dead_words) * sizeof(u64));
        s->dead = d; s->dead_words = nw;
    }
    return 0;
}

/* a chunk of the spill file just filled up: start writing it back now, so its pages are clean (cheap to drop)
 * by the time memory gets tight, instead of piling up dirty */
static void store_chunk_done_(RagStore *s, i32 ci)
{
#if defined(__linux__) && defined(SYNC_FILE_RANGE_WRITE)
    if (s->spill_fd >= 0) {
        if (ci < s->vec.nchunk && s->vec.how[ci] == STORE_SPILL)
            sync_file_range(s->spill_fd, (off_t)s->vec.foff[ci], (off_t)(STORE_CROWS * (i64)s->vec.esz), SYNC_FILE_RANGE_WRITE);
        if (ci < s->idc.nchunk && s->idc.how[ci] == STORE_SPILL)
            sync_file_range(s->spill_fd, (off_t)s->idc.foff[ci], (off_t)(STORE_CROWS * 8), SYNC_FILE_RANGE_WRITE);
        if (ci < s->metac.nchunk && s->metac.how[ci] == STORE_SPILL)
            sync_file_range(s->spill_fd, (off_t)s->metac.foff[ci], (off_t)(STORE_CROWS * 8), SYNC_FILE_RANGE_WRITE);
        if (ci < s->vsc.nchunk && s->vsc.how[ci] == STORE_SPILL)
            sync_file_range(s->spill_fd, (off_t)s->vsc.foff[ci], (off_t)(STORE_CROWS * 4), SYNC_FILE_RANGE_WRITE);
    }
#else
    (void)s; (void)ci;
#endif
}

/* madvise on [p, p + len), widened to whole pages and kept inside the loaded file's mapping */
static void store_madv_(const RagStore *s, const void *p, size_t len, int adv)
{
    size_t pg = (size_t)sysconf(_SC_PAGESIZE), a = (size_t)p & ~(pg - 1), e = (size_t)p + len;
    if (s->map && (const u8 *)p >= (const u8 *)s->map && (const u8 *)p < (const u8 *)s->map + s->map_len
        && e > (size_t)s->map + s->map_len) e = (size_t)s->map + s->map_len;
    if (e > a) madvise((void *)a, e - a, adv);
}

/* access pattern hint for the file-backed vector chunks: sequential for a build pass, random for searching */
static void store_advise_(RagStore *s, int advice)
{
    i32 i;
    for (i = 0; i < s->vec.nchunk; ++i) {
        i64 rows = (i64)s->count - ((i64)i << STORE_CSHIFT);
        if (rows > STORE_CROWS) rows = STORE_CROWS;
        if (rows > 0 && s->vec.how[i] != STORE_ANON) store_madv_(s, s->vec.chunk[i], (size_t)rows * s->vec.esz, advice);
    }
}

/* 1 when some vector rows live in a file (spilled or loaded): re-scoring may then miss the page cache */
static i32 store_vec_filed_(const RagStore *s)
{
    return s->vec.nchunk > 0 && (s->vec.how[0] != STORE_ANON || s->vec.how[s->vec.nchunk - 1] != STORE_ANON);
}

/* ---- id lookups ---- */

static u64 store_hid_(i64 id) { return h64_mix_((u64)id); }

static void sidx_free_(SIdx *h) { free(h->row); free(h->tag); memset(h, 0, sizeof(*h)); }

static u32 sidx_home_(const SIdx *h, u32 tag) { return (u32)(tag * 2654435761u) >> (32 - h->bits); }

static void sidx_grow_(SIdx *h)
{
    SIdx o = *h;
    u32 i, bits = o.bits ? o.bits + 1 : 10;
    h->bits = bits; h->cap = 1u << bits; h->n = 0;
    h->row = (u32 *)calloc(h->cap, sizeof(u32)); h->tag = (u32 *)malloc(h->cap * sizeof(u32));
    for (i = 0; i < o.cap; ++i)
        if (o.row[i]) {
            u32 j = sidx_home_(h, o.tag[i]);
            while (h->row[j]) j = (j + 1) & (h->cap - 1);
            h->row[j] = o.row[i]; h->tag[j] = o.tag[i]; ++h->n;
        }
    free(o.row); free(o.tag);
}

static i32 sidx_get_(const RagStore *s, i64 id)
{
    const SIdx *h = &s->ids_h;
    u32 tag, j;
    if (!h->n) return -1;
    tag = (u32)(store_hid_(id) >> 32);
    for (j = sidx_home_(h, tag); h->row[j]; j = (j + 1) & (h->cap - 1))
        if (h->tag[j] == tag && STORE_ID(s, h->row[j] - 1) == id) return (i32)(h->row[j] - 1);
    return -1;
}

/* id -> r (replaces an older row of the same id) */
static void sidx_put_(RagStore *s, i64 id, i32 r)
{
    SIdx *h = &s->ids_h;
    u32 tag = (u32)(store_hid_(id) >> 32), j;
    if ((h->n + 1) * 10 > h->cap * 7) sidx_grow_(h);
    for (j = sidx_home_(h, tag); h->row[j]; j = (j + 1) & (h->cap - 1))
        if (h->tag[j] == tag && STORE_ID(s, h->row[j] - 1) == id) { h->row[j] = (u32)r + 1; return; }
    h->row[j] = (u32)r + 1; h->tag[j] = tag; ++h->n;
}

/* newest row carrying doc_id (live or not), -1 if none */
static i32 store_find_(const RagStore *s, i64 id)
{
    i32 r = sidx_get_(s, id), lo = 0, hi = s->nsorted - 1;
    if (r >= 0) return r;
    while (lo <= hi) {
        i32 mid = lo + (hi - lo) / 2;
        i64 v = STORE_ID(s, mid);
        if (v == id) return mid;
        if (v < id) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

static i32 store_prev_(const RagStore *s, i32 r) { return s->dup.n ? h64_get(&s->dup, r) : -1; }

/* book row r (already written) into the lookup structures */
static void store_note_row_(RagStore *s, i32 r)
{
    i64 id = STORE_ID(s, r);
    if (s->nsorted == r && (r == 0 || id > STORE_ID(s, r - 1))) { s->nsorted = r + 1; return; }   /* still in order */
    {
        i32 prev = store_find_(s, id);
        if (prev >= 0) h64_put(&s->dup, r, prev);
        sidx_put_(s, id, r);
    }
}

/* rebuild the lookups and the live count from the id column and the bitset */
static void store_reindex_(RagStore *s)
{
    i32 r, live = 0;
    sidx_free_(&s->ids_h);
    h64_clear(&s->dup);
    s->nsorted = 0;
    for (r = 0; r < s->count; ++r) {
        store_note_row_(s, r);
        if (!STORE_DEAD(s, r)) ++live;
    }
    s->live = live;
}

/* The lookups are only needed to add, delete or look up by id - never to search - so a loaded store builds them
 * on first use. Writers call this under the write lock; readers through store_need_idmap_. */
static i32 store_idmap_ready_(RagStore *s)
{
#if defined(__GNUC__)
    return __atomic_load_n(&s->idmap_ok, __ATOMIC_ACQUIRE);
#else
    return s->idmap_ok;
#endif
}
static void store_idmap_locked_(RagStore *s)
{
    if (s->idmap_ok) return;
    store_reindex_(s);
#if defined(__GNUC__)
    __atomic_store_n(&s->idmap_ok, 1, __ATOMIC_RELEASE);
#else
    s->idmap_ok = 1;
#endif
}
static void store_need_idmap_(RagStore *s)
{
    if (store_idmap_ready_(s)) return;
    pthread_rwlock_wrlock(&s->lock);
    store_idmap_locked_(s);
    pthread_rwlock_unlock(&s->lock);
}

/* ------------------------------------------------------------------ */

static int store_finite_(const f32 *v, i32 n)
{
    i32 d;
    for (d = 0; d < n; ++d) {
        union { f32 f; u32 u; } b;
        b.f = v[d];
        if ((b.u & 0x7f800000u) == 0x7f800000u) return 0;   /* Inf or NaN */
    }
    return 1;
}

static void store_normalise_(f32 *v, i32 dim)
{
    f32 nrm = (f32)eng_sqrt_((f64)vs_dot(v, v, dim));
    i32 d;
    if (nrm <= 0.0f) return;
    nrm = 1.0f / nrm;
    for (d = 0; d < dim; ++d) v[d] *= nrm;
}

RagStore *store_new(Arena *a, i32 dim)
{
    RagStore *s = (RagStore *)arena_alloc(a, sizeof(RagStore));
    const char *sp = getenv("VECINDEX_SPILL");
    memset(s, 0, sizeof(*s));
    s->arena = a;
    s->dim = dim;
    s->idmap_ok = 1;
    s->goal.mode = ENG_MAX_ACC;
    s->goal.k = 10;
    s->goal.max_code_bytes = 1 << 20;   /* unbounded -> most accurate config */
    s->goal.eval_queries = 0;           /* engine default (128) */
    s->goal.n_iters = 12;
    s->goal.seed = 0x5eedu;
    s->vec.esz = (size_t)dim * sizeof(f32); s->vec.elem = sizeof(f32);
    s->vsc.esz = sizeof(f32); s->vsc.elem = sizeof(f32);
    s->vbytes = 4; s->codebits = 4;
    s->addbuf = (f32 *)malloc((size_t)dim * sizeof(f32));
    s->idc.esz = sizeof(i64); s->idc.elem = sizeof(i64);
    s->metac.esz = sizeof(i64); s->metac.elem = sizeof(i64);
    h64_init(&s->dup, 8);
    s->tune_recall = -1.0f;
    s->qlog_rng = 0x9e3779b9u;
    pthread_mutex_init(&s->qlog_mu, (const pthread_mutexattr_t *)0);
    s->live_grow = 1.0f;
    pthread_mutex_init(&s->mt_mu, (const pthread_mutexattr_t *)0);
    pthread_mutex_init(&s->rb_mu, (const pthread_mutexattr_t *)0);
    pthread_mutex_init(&s->mt_cmu, (const pthread_mutexattr_t *)0);
    pthread_cond_init(&s->mt_cv, (const pthread_condattr_t *)0);
    s->spill_fd = -1;
    s->spill_min = STORE_SPILL_MIN;
    if (!sp || strcmp(sp, "off") != 0) {                     /* VECINDEX_SPILL=off keeps everything in memory */
        const char *d = sp && *sp ? sp : "/var/tmp";
        s->spill_dir = (char *)malloc(strlen(d) + 1); strcpy(s->spill_dir, d);
    }
    /* s->goal.max_candidates stays 0 (memset above) - no nprobe cap */
    {   /* writer-preferring: a stream of readers must not starve a
         * store_add / store_build waiting for the write lock.
         * (the *_NP name is an enum, not a macro - gate on __GLIBC__.) */
        pthread_rwlockattr_t at;
        pthread_rwlockattr_init(&at);
#ifdef __GLIBC__
        pthread_rwlockattr_setkind_np(
            &at, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
#endif
        pthread_rwlock_init(&s->lock, &at);
        pthread_rwlockattr_destroy(&at);
    }
    return s;
}

/* Where chunks go once the columns pass min_bytes: a directory on a disk (not a tmpfs - that is RAM again), or
 * NULL to keep everything in anonymous memory. The default is $VECINDEX_SPILL or /var/tmp, from 256 MB. The file
 * is unlinked as soon as it is created, so nothing is left behind, even after a crash. */
void store_set_spill(RagStore *s, const char *dir, i64 min_bytes)
{
    pthread_rwlock_wrlock(&s->lock);
    free(s->spill_dir); s->spill_dir = (char *)0;
    if (dir) { s->spill_dir = (char *)malloc(strlen(dir) + 1); strcpy(s->spill_dir, dir); }
    s->spill_min = min_bytes > 0 ? min_bytes : 0;
    pthread_rwlock_unlock(&s->lock);
}

static void store_live_stop_(RagStore *s);

void store_destroy(RagStore *s)
{
    store_live_stop_(s);
    pthread_mutex_destroy(&s->mt_mu); pthread_mutex_destroy(&s->rb_mu); pthread_mutex_destroy(&s->mt_cmu); pthread_cond_destroy(&s->mt_cv);
    pthread_rwlock_destroy(&s->lock); tq_free(s->tq); s->tq = (Tq *)0;
    pthread_mutex_destroy(&s->qlog_mu); free(s->qlog); s->qlog = (f32 *)0;
    sidx_free_(&s->ids_h); h64_free(&s->dup);
    if (s->jf) fclose(s->jf);
    free(s->jpath); s->jpath = (char *)0;
    arena_free(&s->idx_arena);
    scol_free_(&s->vec); scol_free_(&s->idc); scol_free_(&s->metac); scol_free_(&s->vsc);
    free(s->addbuf); s->addbuf = (f32 *)0;
    free(s->dead); s->dead = (u64 *)0; s->dead_words = 0;
    if (s->map) { munmap(s->map, s->map_len); s->map = (void *)0; }
    if (s->spill_fd >= 0) { close(s->spill_fd); s->spill_fd = -1; }
    free(s->spill_dir); s->spill_dir = (char *)0;
}

/* Bytes the store holds: ram = anonymous memory (what really competes for RAM), file = pages backed by a file
 * (the spill file or the loaded .vxi): resident only while there is room, dropped first under pressure. */
void store_mem(RagStore *s, i64 *ram, i64 *file)
{
    i64 r = 0, f = 0;
    i32 c;
    SCol *cols[4];
    pthread_rwlock_rdlock(&s->lock);
    cols[0] = &s->vec; cols[1] = &s->idc; cols[2] = &s->metac; cols[3] = &s->vsc;
    for (c = 0; c < 4; ++c) {
        i32 i;
        for (i = 0; i < cols[c]->nchunk; ++i) {
            i64 rows = (i64)s->count - ((i64)i << STORE_CSHIFT), b;
            if (rows > STORE_CROWS) rows = STORE_CROWS;
            if (rows < 0) rows = 0;
            b = rows * (i64)cols[c]->esz;
            if (cols[c]->how[i] == STORE_ANON) r += b; else f += b;
        }
    }
    r += s->dead_words * 8 + (i64)s->ids_h.cap * 8 + (i64)s->dup.cap * 13;
    if (s->tq) {
        r += tq_ram_(s->tq);
        if (s->tq->mapped) f += (i64)s->tq->nblk * s->tq->vd * 16 + (s->tq->slot_row ? (i64)s->tq->nblk * 128 : 0);
    }
    pthread_rwlock_unlock(&s->lock);
    if (ram) *ram = r;
    if (file) *file = f;
}

/* IVF (large stores): how many clusters a query scans; 0 = the default (min(16%, 4 sqrt(clusters))). More =
 * better recall, slower. */
void store_set_probe(RagStore *s, i32 nprobe) { s->tq_probe = nprobe; }

/* Threads a single query's scan may use; 0 = auto (up to 4 from 150k rows). 1 suits callers that already run
 * one query per thread. */
void store_set_threads(RagStore *s, i32 nthreads) { s->tq_threads = nthreads > 0 ? nthreads : 0; }

/* How many candidates of the 4-bit scan are re-scored exactly; 0 = auto (max(20 k, 256)). Fewer is faster, more
 * lifts recall when the scan's ranking is coarse. */
void store_set_rerank(RagStore *s, i32 r) { s->tq_rerank = r > 0 ? r : 0; }

static i32 store_ncpu_(void)
{
    long c = sysconf(_SC_NPROCESSORS_ONLN);
    return c < 1 ? 1 : (c > 16 ? 16 : (i32)c);
}

/* threads for work in the background (live indexing, rebuilds, tuning): half the cores, so searches keep the rest */
static i32 store_bg_threads_(void) { i32 c = store_ncpu_() / 2; return c < 1 ? 1 : c; }

static Tq *store_tq_build_(RagStore *s, i32 n)
{
    TqCol sc, col = store_vcol_(s, &sc);
    Tq *t;
    store_advise_(s, MADV_SEQUENTIAL);                           /* the build reads every row in order, twice */
    t = tq_build(&col, n, s->dim, s->goal.seed, store_nclusters_(n), store_ncpu_(), s->codebits);
    store_advise_(s, MADV_RANDOM);                               /* searching re-scores rows at random */
    return t;
}

/* Code width of the scan index from the next build on: 4 (default) or 2. 2-bit codes take d/4 bytes a row in RAM
 * instead of d/2 and scan twice as fast, but rank candidates more coarsely: re-score more of them
 * (store_set_rerank) to keep the recall. */
void store_set_codes(RagStore *s, i32 bits)
{
    pthread_rwlock_wrlock(&s->lock);
    s->codebits = bits == 2 ? 2 : 4;
    pthread_rwlock_unlock(&s->lock);
}

/* Bytes a coordinate in the stored rows: 4 (f32, default) or 1 (int8, with an f32 scale a row: d + 4 bytes instead
 * of 4d, re-scoring error around 0.3% of a score). Only on an empty store; returns 0, or -1 if it has rows.
 * int8 rows have no f32 copy to lend out, so store_vec_ptr returns NULL; store_get_vec dequantises. */
i32 store_set_vec_bytes(RagStore *s, i32 bytes)
{
    i32 r = 0;
    pthread_rwlock_wrlock(&s->lock);
    if (s->count > 0 || s->vec.nchunk > 0) r = -1;
    else if (bytes == 1) { s->vbytes = 1; s->vec.esz = (size_t)s->dim; s->vec.elem = 1; }
    else { s->vbytes = 4; s->vec.esz = (size_t)s->dim * sizeof(f32); s->vec.elem = sizeof(f32); }
    pthread_rwlock_unlock(&s->lock);
    return r;
}

/* row r as f32: a pointer into the column, or dequantised into buf[dim] */
static const f32 *store_row_(const RagStore *s, i32 r, f32 *buf)
{
    const i8 *x;
    f32 sc;
    i32 d = 0;
    if (s->vbytes != 1) return STORE_VEC(s, r);
    x = STORE_I8(s, r); sc = STORE_SC(s, r);
#if defined(__AVX512F__)
    for (; d + 16 <= s->dim; d += 16)
        _mm512_storeu_ps(buf + d, _mm512_mul_ps(_mm512_cvtepi32_ps(_mm512_cvtepi8_epi32(_mm_loadu_si128((const __m128i *)(const void *)(x + d)))), _mm512_set1_ps(sc)));
#endif
    for (; d < s->dim; ++d) buf[d] = sc * (f32)x[d];
    return buf;
}

/* q . row r (q unit length): what every exact re-score uses */
static f32 store_dot_(const RagStore *s, const f32 *q, i32 r)
{
    const i8 *x;
    f32 acc = 0.0f;
    i32 d = 0;
    if (s->vbytes != 1) return vs_dot(q, STORE_VEC(s, r), s->dim);
    x = STORE_I8(s, r);
#if defined(__AVX512F__)
    {   __m512 a = _mm512_setzero_ps();
        for (; d + 16 <= s->dim; d += 16)
            a = _mm512_fmadd_ps(_mm512_loadu_ps(q + d), _mm512_cvtepi32_ps(_mm512_cvtepi8_epi32(_mm_loadu_si128((const __m128i *)(const void *)(x + d)))), a);
        acc = _mm512_reduce_add_ps(a); }
#endif
    for (; d < s->dim; ++d) acc += q[d] * (f32)x[d];
    return acc * STORE_SC(s, r);
}

/* store a normalised f32 row into row r (int8: symmetric, scale = max |v| / 127) */
static void store_put_row_(RagStore *s, i32 r, const f32 *v)
{
    i32 d;
    if (s->vbytes != 1) { memcpy(STORE_VEC(s, r), v, (size_t)s->dim * sizeof(f32)); return; }
    {   f32 mx = 0.0f, inv;
        i8 *x = STORE_I8(s, r);
        for (d = 0; d < s->dim; ++d) { f32 a = v[d] < 0.0f ? -v[d] : v[d]; if (a > mx) mx = a; }
        inv = mx > 0.0f ? 127.0f / mx : 0.0f;
        for (d = 0; d < s->dim; ++d) { f32 y = v[d] * inv; x[d] = (i8)(y < 0.0f ? y - 0.5f : y + 0.5f); }
        STORE_SC(s, r) = mx / 127.0f; }
}

/* fast build: one k-means + q4 instead of the auto-tuner's candidate bake-off */
void store_set_fast(RagStore *s, f32 target_recall)
{
    pthread_mutex_lock(&s->mt_mu);
    pthread_rwlock_wrlock(&s->lock);
    s->fast = 1; s->fast_target = target_recall > 0.0f ? target_recall : 0.9f;
    if (!s->tq && s->count > 0 && s->built_upto == s->count) {   /* a loaded, already-built store */
        s->idx = (EngIndex *)0;
        s->tq = store_tq_build_(s, s->count);
    }
    pthread_rwlock_unlock(&s->lock);
    pthread_mutex_unlock(&s->mt_mu);
}

void store_set_goal(RagStore *s, EngGoal g)
{
    pthread_rwlock_wrlock(&s->lock);
    s->goal = g;
    pthread_rwlock_unlock(&s->lock);
}

/* -1 if the vector has a non-finite component (not added) or memory ran out, else 0. */
i32 store_add_meta(RagStore *s, const f32 *vec, i64 doc_id, i64 meta)
{
    i32 r;
    f32 *v;
    if (!store_finite_(vec, s->dim)) return -1;
    pthread_rwlock_wrlock(&s->lock);
    store_idmap_locked_(s);
    if (store_reserve_(s, (i64)s->count + 1) != 0) { pthread_rwlock_unlock(&s->lock); return -1; }
    r = s->count;
    v = s->addbuf;
    memcpy(v, vec, (size_t)s->dim * sizeof(f32));
    store_normalise_(v, s->dim);
    store_put_row_(s, r, v);
    STORE_ID(s, r) = doc_id;
    STORE_META(s, r) = meta;
    s->dead[r >> 6] &= ~((u64)1 << (r & 63));
    store_note_row_(s, r);
    s->live += 1;
    s->count += 1;
    ++s->mods;
    if (((i64)s->count & (STORE_CROWS - 1)) == 0) store_chunk_done_(s, (i32)(((i64)s->count >> STORE_CSHIFT) - 1));
    if (s->jf) {                                                  /* journal: 'A' id meta vec[dim] (the normalised row) */
        u8 op = 'A';
        fwrite(&op, 1, 1, s->jf); fwrite(&doc_id, 8, 1, s->jf); fwrite(&meta, 8, 1, s->jf);
        fwrite(v, sizeof(f32), (size_t)s->dim, s->jf);
    }
    if (s->live_on && ((s->count - s->built_upto) & 255) == 1) pthread_cond_signal(&s->mt_cv);   /* the live thread */
    pthread_rwlock_unlock(&s->lock);
    return 0;
}

i32 store_add(RagStore *s, const f32 *vec, i64 doc_id)
{ return store_add_meta(s, vec, doc_id, 0); }

/* returns how many of the m rows were actually added (bad ones skipped) */
i32 store_add_batch(RagStore *s, const f32 *vecs, const i64 *doc_ids, i32 m)
{
    i32 i, ok = 0;
    for (i = 0; i < m; ++i)
        if (store_add_meta(s, vecs + (size_t)i * s->dim, doc_ids[i], 0) == 0)
            ++ok;
    return ok;
}

i32 store_delete(RagStore *s, i64 doc_id)
{
    i32 r, hit = 0;
    pthread_rwlock_wrlock(&s->lock);
    store_idmap_locked_(s);
    for (r = store_find_(s, doc_id); r >= 0; r = store_prev_(s, r))
        if (!STORE_DEAD(s, r)) { STORE_KILL(s, r); ++hit; --s->live; ++s->mods; }
    if (s->jf) { u8 op = 'D'; fwrite(&op, 1, 1, s->jf); fwrite(&doc_id, 8, 1, s->jf); }
    pthread_rwlock_unlock(&s->lock);
    return hit;
}

/* counters under the read lock: the live thread moves built_upto, adders move count and live */
i32 store_count(const RagStore *s)
{
    i32 r;
    pthread_rwlock_rdlock((pthread_rwlock_t *)&s->lock); r = s->live; pthread_rwlock_unlock((pthread_rwlock_t *)&s->lock);
    return r;
}
i32 store_pending(const RagStore *s)
{
    i32 r;
    pthread_rwlock_rdlock((pthread_rwlock_t *)&s->lock); r = s->count - s->built_upto; pthread_rwlock_unlock((pthread_rwlock_t *)&s->lock);
    return r;
}
/* 1 when a vector index exists and covers every row */
i32 store_built(const RagStore *s)
{
    i32 r;
    pthread_rwlock_rdlock((pthread_rwlock_t *)&s->lock);
    r = (s->idx || s->tq) && s->built_upto == s->count;
    pthread_rwlock_unlock((pthread_rwlock_t *)&s->lock);
    return r;
}

/* O(1)/O(log n) lookups. store_get_vec copies the unit-normalised vector of the newest live row with that id into
 * out[dim]; store_set_meta / store_get_meta touch its i64 tag. All return 0 on success, -1 if the id is not live. */
static i32 store_live_row_(const RagStore *s, i64 doc_id)
{
    i32 r;
    for (r = store_find_(s, doc_id); r >= 0 && STORE_DEAD(s, r); r = store_prev_(s, r)) {}
    return r;
}
/* borrowed pointer to the live row's vector (valid until the next build/compact - adds never move rows; no
 * lock - for single-threaded callers such as vecdb), NULL if the id is not live */
const f32 *store_vec_ptr(const RagStore *s, i64 doc_id)
{
    i32 r;
    store_need_idmap_((RagStore *)s);
    if (s->vbytes == 1) return (const f32 *)0;                   /* int8 rows: no f32 copy to lend (store_get_vec) */
    r = store_live_row_(s, doc_id);
    return r >= 0 ? STORE_VEC(s, r) : (const f32 *)0;
}
i32 store_has(RagStore *s, i64 doc_id)
{
    i32 r;
    store_need_idmap_(s);
    pthread_rwlock_rdlock(&s->lock);
    r = store_live_row_(s, doc_id);
    pthread_rwlock_unlock(&s->lock);
    return r >= 0;
}
i32 store_get_vec(RagStore *s, i64 doc_id, f32 *out)
{
    i32 r;
    store_need_idmap_(s);
    pthread_rwlock_rdlock(&s->lock);
    r = store_live_row_(s, doc_id);
    if (r >= 0) { const f32 *v = store_row_(s, r, out); if (v != out) memcpy(out, v, (size_t)s->dim * sizeof(f32)); }
    pthread_rwlock_unlock(&s->lock);
    return r >= 0 ? 0 : -1;
}
i32 store_set_meta(RagStore *s, i64 doc_id, i64 meta)
{
    i32 r;
    pthread_rwlock_wrlock(&s->lock);
    store_idmap_locked_(s);
    r = store_live_row_(s, doc_id);
    if (r >= 0) STORE_META(s, r) = meta;
    if (r >= 0 && s->jf) { u8 op = 'M'; fwrite(&op, 1, 1, s->jf); fwrite(&doc_id, 8, 1, s->jf); fwrite(&meta, 8, 1, s->jf); }
    pthread_rwlock_unlock(&s->lock);
    return r >= 0 ? 0 : -1;
}
i32 store_get_meta(RagStore *s, i64 doc_id, i64 *meta)
{
    i32 r;
    store_need_idmap_(s);
    pthread_rwlock_rdlock(&s->lock);
    r = store_live_row_(s, doc_id);
    if (r >= 0) *meta = STORE_META(s, r);
    pthread_rwlock_unlock(&s->lock);
    return r >= 0 ? 0 : -1;
}

/* tombstones among rows [0, r), with pre[w] = tombstones in words [0, w) */
static i32 store_dead_before_(const RagStore *s, const i32 *pre, i32 r)
{
    return pre[r >> 6] + ((r & 63) ? tq_pop64_(s->dead[r >> 6] & (((u64)1 << (r & 63)) - 1u)) : 0);
}

/* drop tombstoned rows (moving the survivors down, order kept); a store without tombstones is left untouched. The
 * scan index follows its rows (tq_remap); an engine index cannot, so it is dropped (its rows pend until a build). */
static i32 store_compact_(RagStore *s)
{
    i32 i, w = 0;
    if (s->live == s->count) { if (s->built_upto > s->count) s->built_upto = s->count; return s->count; }
    {
        i64 words = ((i64)s->count + 63) >> 6, x;
        i32 *pre = (i32 *)malloc((size_t)(words + 1) * sizeof(i32)), acc = 0, bu;
        for (x = 0; x <= words; ++x) { pre[x] = acc; if (x < words) acc += tq_pop64_(s->dead[x]); }
        bu = s->built_upto - store_dead_before_(s, pre, s->built_upto);
        if (s->tq) tq_remap(s->tq, s->dead, pre, bu);
        if (s->idx) { s->idx = (EngIndex *)0; arena_reset(&s->idx_arena); bu = 0; }
        s->built_upto = bu;
        s->dead_built = 0;
        free(pre);
    }
    for (i = 0; i < s->count; ++i) {
        if (STORE_DEAD(s, i)) continue;
        if (w != i) {
            memcpy(SCOL_AT(&s->vec, w), SCOL_AT(&s->vec, i), s->vec.esz);
            if (s->vbytes == 1) STORE_SC(s, w) = STORE_SC(s, i);
            STORE_ID(s, w) = STORE_ID(s, i);
            STORE_META(s, w) = STORE_META(s, i);
        }
        ++w;
    }
    memset(s->dead, 0, (size_t)s->dead_words * sizeof(u64));
    s->count = w;
    s->live = w;
#if defined(__GNUC__)
    __atomic_store_n(&s->idmap_ok, 0, __ATOMIC_RELEASE);        /* rows moved: the lookups are rebuilt on next use */
#else
    s->idmap_ok = 0;
#endif
    return w;
}

static f32 store_tune_run_(RagStore *s, i32 nth);

static i32 store_uses_tq_(const RagStore *s) { return s->fast || (s->goal.mode == ENG_MAX_ACC && s->goal.max_code_bytes >= s->dim * 4); }

/* the engine index (the q1-q5 / IVF / HNSW tuner) over rows [0, w), under the write lock */
static void store_engine_build_(RagStore *s, i32 w)
{
    f32 *flat = (f32 *)malloc((size_t)w * (size_t)s->dim * sizeof(f32));   /* the tuner wants one contiguous array */
    i32 i;
    for (i = 0; i < w; ++i) { f32 *dst = flat + (size_t)i * s->dim; const f32 *v = store_row_(s, i, dst); if (v != dst) memcpy(dst, v, (size_t)s->dim * sizeof(f32)); }
    s->idx = engine_build(&s->idx_arena, flat, w, s->dim, s->goal);
    free(flat);
}

void store_build(RagStore *s)
{
    i32 w;
    pthread_mutex_lock(&s->rb_mu);                               /* not in the middle of a live rebuild */
    pthread_mutex_lock(&s->mt_mu);
    pthread_rwlock_wrlock(&s->lock);
    tq_free(s->tq); s->tq = (Tq *)0;                             /* rebuilt below: no use remapping it */
    w = store_compact_(s);
    arena_reset(&s->idx_arena);
    s->tuned_n = 0; s->dead_built = 0; s->win_sum = 0.0; s->win_n = 0; s->drifted = 0;
    /* Auto choice: with no memory budget (the default) the data-oblivious 4-bit scan + exact re-score wins
     * on accuracy and speed and needs no training. The q1-q5 / IVF / HNSW tuner (engine_build) only earns
     * its 10-70 s when the goal is a SMALL index (ENG_MIN_SIZE, or a code budget below f32 size). */
    s->idx = (EngIndex *)0;
    if (w > 0 && store_uses_tq_(s)) s->tq = store_tq_build_(s, w);
    else if (w > 0) store_engine_build_(s, w);
    s->built_upto = w;
    pthread_rwlock_unlock(&s->lock);
    store_tune_run_(s, store_ncpu_());                           /* keep the recall asked for, if one is (searches go on) */
    pthread_mutex_unlock(&s->mt_mu);
    pthread_mutex_unlock(&s->rb_mu);
}

/* ------------------------------------------------------------------ *
 * search - read-locked, own scratch, engine_search_into (arena-free)  *
 * ------------------------------------------------------------------ */

static i32 store_meta_ok_(i64 tag, i64 all_bits, i64 any_bits)
{
    if ((tag & all_bits) != all_bits) return 0;
    if (any_bits && !(tag & any_bits)) return 0;
    return 1;
}

static i64 store_majflt_(void)
{
#if defined(RUSAGE_THREAD)
    struct rusage u;
    return getrusage(RUSAGE_THREAD, &u) == 0 ? (i64)u.ru_majflt : 0;
#else
    return 0;
#endif
}

/* ask the kernel for the pages of every candidate row at once (they come in parallel instead of one blocking
 * fault after another); only for rows that live in a file */
static void store_willneed_(const RagStore *s, const i32 *rows, i32 m)
{
    i32 i;
    for (i = 0; i < m; ++i) {
        i32 r = rows[i];
        if (r < 0 || r >= s->count || s->vec.how[(size_t)r >> STORE_CSHIFT] == STORE_ANON) continue;
        store_madv_(s, SCOL_AT(&s->vec, r), s->vec.esz, MADV_WILLNEED);
        if (s->vbytes == 1) store_madv_(s, SCOL_AT(&s->vsc, r), 4, MADV_WILLNEED);
    }
}

/* exact re-score of candidate rows into the (row, score) top-k */
static void store_rescore_(RagStore *s, const f32 *qn, const i32 *rows, i32 m, i32 k, i64 *out_rows, f32 *out_scores, i32 *have)
{
    i32 i, pf;
    i64 f0 = 0;
#if defined(__GNUC__)
    pf = __atomic_load_n(&s->pf_cold, __ATOMIC_RELAXED);
    {   u32 t = __atomic_add_fetch(&s->pf_tick, 1u, __ATOMIC_RELAXED);
        if (pf && (t & 63u) == 0) pf = 0; }                      /* now and then try without, to notice a warm cache */
#else
    pf = s->pf_cold;
#endif
    if (pf) store_willneed_(s, rows, m);
    else if (store_vec_filed_(s)) f0 = store_majflt_();
    for (i = 0; i < m; ++i) {
        i32 row = rows[i];
        if (i + STORE_PF_AHEAD < m && rows[i + STORE_PF_AHEAD] >= 0 && rows[i + STORE_PF_AHEAD] < s->built_upto)
            store_prefetch_row_((const f32 *)(const void *)SCOL_AT(&s->vec, rows[i + STORE_PF_AHEAD]), (i32)(s->vec.esz / 4));
        if (row < 0 || row >= s->built_upto) continue;
        eng_topk_(out_rows, out_scores, k, have, (i64)row, 2.0f * store_dot_(s, qn, row) - 2.0f);
    }
    if (!pf && store_vec_filed_(s)) {
        i64 df = store_majflt_() - f0;
        i32 want = df > 4 ? 1 : 0;                               /* faults went to the disk: prefetch from now on */
#if defined(__GNUC__)
        __atomic_store_n(&s->pf_cold, want, __ATOMIC_RELAXED);
#else
        s->pf_cold = want;
#endif
    }
}

/* ---- self-tuning: the fewest probes that keep recall@10 at the target ---- */

/* a query into the reservoir (s->qlog): try-locked, so a busy reservoir skips the query and nobody waits */
static void store_qlog_(RagStore *s, const f32 *qn)
{
    i64 slot;
    if (pthread_mutex_trylock(&s->qlog_mu) != 0) return;
    if (!s->qlog) s->qlog = (f32 *)malloc((size_t)STORE_QLOG * (size_t)s->dim * sizeof(f32));
    if (s->qlog) {
        ++s->qlog_seen;
        if (s->qlog_n < STORE_QLOG) slot = s->qlog_n++;
        else slot = (i64)(tq_rng_(&s->qlog_rng) % (u32)(s->qlog_seen < 0x7fffffff ? s->qlog_seen : 0x7fffffff));
        if (slot < STORE_QLOG) memcpy(s->qlog + (size_t)slot * s->dim, qn, (size_t)s->dim * sizeof(f32));
    }
    pthread_mutex_unlock(&s->qlog_mu);
}

/* ---- reading rows without the lock: what the live thread builds, codes and measures on ---- */

/* Rows [0, n) as they are now: the chunk pointers of the vector (and scale) columns and, if asked, the tombstones,
 * copied - adds may grow the columns meanwhile, but no row below n moves while mt_mu is held (only a compaction moves
 * rows, and it runs under mt_mu), and no chunk is ever freed before store_destroy. Taken under the read lock. */
typedef struct { i32 n; u8 **vch, **sch; u64 *dead; TqCol col, scol; } StoreSnap;

static void store_snap_(const RagStore *s, i32 n, i32 with_dead, StoreSnap *sn)
{
    i32 nch = (i32)(((i64)n + STORE_CROWS - 1) >> STORE_CSHIFT);
    size_t nw = ((size_t)n + 63) >> 6;
    memset(sn, 0, sizeof(*sn));
    sn->n = n;
    sn->vch = (u8 **)malloc((size_t)(nch ? nch : 1) * sizeof(u8 *));
    if (nch) memcpy(sn->vch, s->vec.chunk, (size_t)nch * sizeof(u8 *));
    sn->col = store_tqcol_(&s->vec); sn->col.chunk = sn->vch;
    if (s->vbytes == 1) {
        sn->sch = (u8 **)malloc((size_t)(nch ? nch : 1) * sizeof(u8 *));
        if (nch) memcpy(sn->sch, s->vsc.chunk, (size_t)nch * sizeof(u8 *));
        sn->scol = store_tqcol_(&s->vsc); sn->scol.chunk = sn->sch;
        sn->col.i8 = 1; sn->col.dim = s->dim; sn->col.scale = &sn->scol;
    }
    if (with_dead) {
        sn->dead = (u64 *)calloc(nw ? nw : 1, sizeof(u64));
        if (nw) memcpy(sn->dead, s->dead, nw * sizeof(u64));
    }
}

static void store_snap_free_(StoreSnap *sn) { free(sn->vch); free(sn->sch); free(sn->dead); memset(sn, 0, sizeof(*sn)); }

/* q . row r of a column (q unit length; int8 rows times their scale) */
static f32 store_cdot_(const TqCol *c, const f32 *q, i64 r, i32 dim)
{
    const i8 *x;
    f32 acc = 0.0f;
    i32 d = 0;
    if (!c->i8) return vs_dot(q, TQ_ROW(c, r), dim);
    x = (const i8 *)(const void *)TQ_AT(c, r);
#if defined(__AVX512F__)
    {   __m512 a = _mm512_setzero_ps();
        for (; d + 16 <= dim; d += 16)
            a = _mm512_fmadd_ps(_mm512_loadu_ps(q + d), _mm512_cvtepi32_ps(_mm512_cvtepi8_epi32(_mm_loadu_si128((const __m128i *)(const void *)(x + d)))), a);
        acc = _mm512_reduce_add_ps(a); }
#endif
    for (; d < dim; ++d) acc += q[d] * (f32)x[d];
    return acc * *(const f32 *)(const void *)TQ_AT(c->scale, r);
}

/* maintenance counters, read without the lock (store_live_stats) */
static void store_stat_add_(i64 *p, i64 v)
{
#if defined(__GNUC__)
    __atomic_add_fetch(p, v, __ATOMIC_RELAXED);
#else
    *p += v;
#endif
}
static i64 store_stat_get_(const i64 *p)
{
#if defined(__GNUC__)
    return __atomic_load_n(p, __ATOMIC_RELAXED);
#else
    return *p;
#endif
}
static void store_stat_set_(i64 *p, i64 v)
{
#if defined(__GNUC__)
    __atomic_store_n(p, v, __ATOMIC_RELAXED);
#else
    *p = v;
#endif
}

/* the exact top k rows of every query among rows [0, n) of a column (tombstones skipped, a query's own row left out
 * when self[q] >= 0): the rows are read once, each thread takes a range and keeps its own top k, and the ranges are
 * merged at the end */
typedef struct { const TqCol *col; const u64 *dead; i32 dim; const f32 *Q; const i32 *self; i32 nq, k; f32 *ts; i32 *tr; } StoreTruth;

static void store_topk_put_(f32 *bs, i32 *br, i32 k, f32 d, i32 r)
{
    i32 j = k - 1;
    if (!(d > bs[k - 1])) return;
    while (j > 0 && bs[j - 1] < d) { bs[j] = bs[j - 1]; br[j] = br[j - 1]; --j; }
    bs[j] = d; br[j] = r;
}

static void store_truth_rows_(void *p, i64 a, i64 b, i32 tid)
{
    StoreTruth *j = (StoreTruth *)p;
    f32 *ts = j->ts + (size_t)tid * j->nq * j->k;
    i32 *tr = j->tr + (size_t)tid * j->nq * j->k, q;
    i64 r0, r;
    for (r0 = a; r0 < b; r0 += 256)                              /* 256 rows stay in cache across all the queries */
        for (q = 0; q < j->nq; ++q) {
            const f32 *qv = j->Q + (size_t)q * j->dim;
            for (r = r0; r < b && r < r0 + 256; ++r) {
                if (TQ_SKIP(j->dead, r) || (j->self && j->self[q] == (i32)r)) continue;
                store_topk_put_(ts + (size_t)q * j->k, tr + (size_t)q * j->k, j->k, store_cdot_(j->col, qv, r, j->dim), (i32)r);
            }
        }
}

static void store_truth_(const TqCol *col, const u64 *dead, i32 n, i32 dim, const f32 *Q, const i32 *self, i32 nq, i32 k, i32 *truth, i32 nth)
{
    StoreTruth j;
    i32 t, q, x;
    size_t per = (size_t)nq * (size_t)k;
    if (nth > 64) nth = 64;
    j.col = col; j.dead = dead; j.dim = dim; j.Q = Q; j.self = self; j.nq = nq; j.k = k;
    j.ts = (f32 *)malloc((size_t)nth * per * sizeof(f32)); j.tr = (i32 *)malloc((size_t)nth * per * sizeof(i32));
    for (x = 0; x < (i32)((size_t)nth * per); ++x) { j.ts[x] = -1e30f; j.tr[x] = -1; }
    tq_par_(store_truth_rows_, &j, n, nth);
    for (q = 0; q < nq; ++q)                                     /* fold every thread's top k into thread 0's */
        for (t = 1; t < nth; ++t)
            for (x = 0; x < k; ++x)
                if (j.tr[(size_t)t * per + (size_t)q * k + x] >= 0)
                    store_topk_put_(j.ts + (size_t)q * k, j.tr + (size_t)q * k, k, j.ts[(size_t)t * per + (size_t)q * k + x], j.tr[(size_t)t * per + (size_t)q * k + x]);
    memcpy(truth, j.tr, per * sizeof(i32));
    free(j.ts); free(j.tr);
}

/* recall@10 of the index at `probe` clusters against the exact truth (the batch path, re-scored exactly as a search) */
static f32 store_recall_at_(RagStore *s, const f32 *Q, const i32 *self, i32 nq, const i32 *truth, i32 probe, i32 nth)
{
    i32 R = s->tq_rerank > 0 ? s->tq_rerank : 256, q, x, z, hits = 0;
    i32 *rows = (i32 *)malloc((size_t)nq * (size_t)R * sizeof(i32)), *cnt = (i32 *)malloc((size_t)nq * sizeof(i32));
    tq_search_many(s->tq, Q, nq, R, rows, cnt, s->dead, nth, probe);
    for (q = 0; q < nq; ++q) {
        i64 got[11];
        f32 sc[11];
        i32 have = 0, kept = 0;
        store_rescore_(s, Q + (size_t)q * s->dim, rows + (size_t)q * R, cnt[q], 11, got, sc, &have);
        for (x = 0; x < have && kept < 10; ++x) {
            if (self && got[x] == self[q]) continue;             /* a row asking for itself finds itself: not a hit */
            ++kept;
            for (z = 0; z < 10; ++z) if (truth[(size_t)q * 10 + z] == (i32)got[x]) { ++hits; break; }
        }
    }
    free(rows); free(cnt);
    return (f32)hits / (f32)(nq * 10);
}

/* the same under the read lock (the caller holds none): one measurement is all a search ever waits for */
static f32 store_recall_rd_(RagStore *s, const f32 *Q, const i32 *self, i32 nq, const i32 *truth, i32 probe, i32 nth)
{
    f32 r;
    pthread_rwlock_rdlock(&s->lock);
    r = store_recall_at_(s, Q, self, nq, truth, probe, nth);
    pthread_rwlock_unlock(&s->lock);
    return r;
}

/* The queries to tune on, into Q[nq x dim]: the logged traffic when 64 or more were kept (*self = NULL), else a
 * spread of the store's own live rows below n (*self[q] = that row, left out of its own truth). Returns how many. */
static i32 store_tune_queries_(RagStore *s, const TqCol *col, const u64 *dead, i32 n, f32 *Q, i32 nq, i32 **self, i32 *from_log)
{
    i32 q;
    *self = (i32 *)0;
    pthread_mutex_lock(&s->qlog_mu);
    *from_log = s->qlog_n >= 64;
    if (*from_log) {                                             /* real traffic: the queries this store answers */
        if (nq > s->qlog_n) nq = s->qlog_n;
        memcpy(Q, s->qlog, (size_t)nq * (size_t)s->dim * sizeof(f32));
    }
    pthread_mutex_unlock(&s->qlog_mu);
    if (*from_log) return nq;
    *self = (i32 *)malloc((size_t)nq * sizeof(i32));             /* no traffic yet: rows of its own, spread out */
    for (q = 0; q < nq; ++q) {
        i32 r = (i32)((i64)n * q / nq) + q % 7;
        f32 *dst = Q + (size_t)q * s->dim;
        const f32 *v;
        if (r >= n) r = n - 1;
        while (r < n - 1 && TQ_SKIP(dead, r)) ++r;
        v = tq_row_(col, r, dst);
        if (v != dst) memcpy(dst, v, (size_t)s->dim * sizeof(f32));
        (*self)[q] = r;
    }
    return nq;
}

/* The fewest IVF probes whose recall@10 reaches s->tune_target (plus a margin for the sample's own error), measured
 * against exact search on the queries the store has been answering (when 64 or more were kept) or else on a spread of
 * its own rows (their own row left out of the truth). Up to STORE_QLOG queries, fewer on huge stores (the exact search
 * reads every row once per query: about 3e10 multiply-adds at most, ~3 s on 2 cores). The probe count becomes the
 * index's default (saved with it). Returns the recall reached, -1 with no IVF index to tune.
 * Called with mt_mu held and no lock: the exact truth runs on a snapshot with no lock at all, each recall measurement
 * under the read lock, and the result goes in under the write lock - searches go on throughout. nth: threads. */
static f32 store_tune_run_(RagStore *s, i32 nth)
{
    i32 nq, nc, n, lo, hi, p, *self = (i32 *)0, *truth, from_log = 0, np0;
    f32 *Q, rec, rec_hi, goal, target;
    f64 budget;
    StoreSnap sn;
    Tq *t;
    pthread_rwlock_rdlock(&s->lock);
    t = s->tq; n = s->built_upto; target = s->tune_target;
    if (!t || t->nc <= 1 || target <= 0.0f || n < 1000 || t->n != n) { pthread_rwlock_unlock(&s->lock); return -1.0f; }
    nc = t->nc; np0 = t->nprobe;
    store_snap_(s, n, 1, &sn);
    pthread_rwlock_unlock(&s->lock);
    budget = 3e10 / ((f64)n * (f64)s->dim);
    nq = budget > STORE_QLOG ? STORE_QLOG : (budget < 32.0 ? 32 : (i32)budget);
    Q = (f32 *)malloc((size_t)nq * (size_t)s->dim * sizeof(f32));
    nq = store_tune_queries_(s, &sn.col, sn.dead, n, Q, nq, &self, &from_log);
    truth = (i32 *)malloc((size_t)nq * 10 * sizeof(i32));
    store_truth_(&sn.col, sn.dead, n, s->dim, Q, self, nq, 10, truth, nth);
    /* aim 1.5 standard errors above the target, so fresh queries meet it too, not just half of the time */
    goal = target + 1.5f * (f32)sqrt((f64)target * (1.0 - (f64)target) / (10.0 * (f64)nq));
    if (goal > 0.999f) goal = 0.999f;
    /* recall grows with the probes: double from the default until the target is met, then halve the gap */
    hi = np0 > 0 ? np0 : 1;
    rec_hi = store_recall_rd_(s, Q, self, nq, truth, hi, nth);
    lo = 0;
    while (rec_hi < goal && hi < nc) { lo = hi; hi = hi * 2 < nc ? hi * 2 : nc; rec_hi = store_recall_rd_(s, Q, self, nq, truth, hi, nth); }
    if (rec_hi >= goal)
        while (hi - lo > 1) {                                    /* the goal is met at hi and not at lo */
            p = lo + (hi - lo) / 2;
            rec = store_recall_rd_(s, Q, self, nq, truth, p, nth);
            if (rec >= goal) { hi = p; rec_hi = rec; } else lo = p;
        }
    pthread_rwlock_wrlock(&s->lock);
    if (s->tq == t) {                                            /* the index's default from now on (and on disk) */
        t->nprobe = hi; s->tq_probe = 0; s->tune_recall = rec_hi; s->tuned_n = t->n; s->tuned_log = from_log;
    }
    pthread_rwlock_unlock(&s->lock);
    store_snap_free_(&sn);
    free(Q); free(self); free(truth);
    return rec_hi;
}

void store_set_target_recall(RagStore *s, f32 target)
{
    pthread_rwlock_wrlock(&s->lock);
    s->tune_target = target > 0.0f && target < 1.0f ? target : 0.0f;
    pthread_rwlock_unlock(&s->lock);
}

/* tune now (store_build tuned on what it had): returns the recall@10 reached, -1 without an IVF index or a target.
 * Searches go on meanwhile (a few seconds on a million rows). */
f32 store_tune(RagStore *s)
{
    f32 r;
    pthread_mutex_lock(&s->mt_mu);
    r = store_tune_run_(s, store_ncpu_());
    pthread_mutex_unlock(&s->mt_mu);
    return r;
}

f32 store_tuned_recall(RagStore *s) { return s->tune_recall; }

/* ---- live mode: rows indexed as they come, the index rebuilt as it ages ---- */

#ifndef STORE_LIVE_CHUNK
#define STORE_LIVE_CHUNK 4096   /* rows coded per step of live indexing */
#endif
#ifndef STORE_LIVE_MIN
#define STORE_LIVE_MIN 1024     /* rows before a live store builds its first index */
#endif
#ifndef STORE_LIVE_DRIFT
#define STORE_LIVE_DRIFT 1.25   /* rebuild once added rows sit this much farther from their centroids than built ones */
#endif
#ifndef STORE_LIVE_SKEW
#define STORE_LIVE_SKEW 1.2     /* ... or once the cluster sizes are this much more uneven than at the build (tq_skew) */
#endif

/* How well the rows going in fit the clusters, a window of max(1000, a twentieth of the build) rows at a time: a
 * window whose rows sit STORE_LIVE_DRIFT times farther from their centroids than the built rows did means the data
 * has moved away from the clusters (one far window is enough; the old rows added since cannot water it down). Under
 * the write lock, mt_mu held. */
static void store_fit_(RagStore *s, const Tq *t, const TqAdd *ad, i32 m)
{
    i32 i, win = t->nbuilt / 20 > 1000 ? t->nbuilt / 20 : 1000;
    if (t->nc <= 1 || !(t->fit0 > 1e-6f)) return;
    for (i = 0; i < m; ++i) {
        s->win_sum += (f64)ad->d2[i];
        if (++s->win_n >= win) {
            if (s->win_sum > STORE_LIVE_DRIFT * (f64)t->fit0 * (f64)s->win_n) ++s->drifted;
            s->win_sum = 0.0; s->win_n = 0;
        }
    }
}

/* Index the pending tail, STORE_LIVE_CHUNK rows at a time: coded on a snapshot with no lock held (searches and adds go
 * on), then placed under the write lock (a few stores a row). mt_mu held. Returns the rows indexed. */
static i32 store_index_pending_(RagStore *s, i32 nth)
{
    i32 done = 0;
    for (;;) {
        StoreSnap sn;
        TqAdd ad;
        Tq *t;
        i32 r0, m, ok;
        pthread_rwlock_rdlock(&s->lock);
        t = s->tq; r0 = s->built_upto; m = s->count - r0;
        if (!t || s->idx || m <= 0 || t->n != r0) { pthread_rwlock_unlock(&s->lock); break; }
        if (m > STORE_LIVE_CHUNK) m = STORE_LIVE_CHUNK;
        store_snap_(s, r0 + m, 0, &sn);
        pthread_rwlock_unlock(&s->lock);
        if (t->nc > 1 && !(t->fit0 > 0.0f)) tq_fit0(t, &sn.col);   /* a loaded index: what its rows fit like */
        ok = tq_add_prep(t, &sn.col, r0, m, nth, &ad) == 0;
        store_snap_free_(&sn);
        if (!ok) break;
        pthread_rwlock_wrlock(&s->lock);
        if (s->tq == t && s->built_upto == r0) {
            ok = tq_add_put(t, &ad, r0) == 0;
            store_fit_(s, t, &ad, t->n - r0);
            done += t->n - r0;
            s->built_upto = t->n;                                /* what went in: all of it, unless memory ran out */
        } else ok = 0;
        pthread_rwlock_unlock(&s->lock);
        tq_add_free(&ad);
        if (!ok) break;
    }
    store_stat_add_(&s->n_indexed, done);
    return done;
}

/* the same with the write lock held: store_save indexes what came in after its store_index_pending_ */
static void store_index_locked_(RagStore *s)
{
    TqCol sc, col;
    TqAdd ad;
    i32 r0 = s->built_upto, m = s->count - r0;
    if (!s->tq || s->idx || m <= 0 || s->tq->n != r0) return;
    col = store_vcol_(s, &sc);
    if (s->tq->nc > 1 && !(s->tq->fit0 > 0.0f)) tq_fit0(s->tq, &col);
    if (tq_add_prep(s->tq, &col, r0, m, store_ncpu_(), &ad) != 0) return;
    tq_add_put(s->tq, &ad, r0);
    store_fit_(s, s->tq, &ad, s->tq->n - r0);
    store_stat_add_(&s->n_indexed, s->tq->n - r0);
    s->built_upto = s->tq->n;
    tq_add_free(&ad);
}

/* Rebuild the scan index over every row there is, without stopping anything: the clustering and the codes are
 * computed on a snapshot of the rows (tombstones left out) while searches go on with the old index and the live thread
 * keeps adding new rows to it; then the new index catches up on those rows privately and is swapped in under the
 * write lock once only a few are left - so the exact-scan tail does not grow while a rebuild runs. Holds rb_mu
 * throughout (no compaction may move rows under it), mt_mu only for the swap. The caller set s->rebuilding; it is
 * cleared here. 0, or -1 when there was nothing to build. */
static i32 store_rebuild_(RagStore *s, i32 nth)
{
    StoreSnap sn;
    Tq *nt, *old;
    i32 n, nc, live, dead, bits, round, stuck = 0;
    u32 seed;
    f64 t0 = tq_now_();
    pthread_mutex_lock(&s->rb_mu);
    pthread_rwlock_rdlock(&s->lock);
    n = s->count; live = s->live; dead = n - live; bits = s->codebits; seed = s->goal.seed;
    if (live <= 0 || s->idx) {
        pthread_rwlock_unlock(&s->lock); pthread_mutex_unlock(&s->rb_mu); store_stat_set_(&s->rebuilding, 0); return -1;
    }
    store_snap_(s, n, 1, &sn);
    store_stat_set_(&s->rebuilt_mods, s->mods);
    pthread_rwlock_unlock(&s->lock);
    nc = store_nclusters_(live);
    nt = tq_build_skip(&sn.col, n, s->dim, seed, nc, nth, bits, nc > 1 ? sn.dead : (const u64 *)0);
    store_snap_free_(&sn);
    if (!nt) { pthread_mutex_unlock(&s->rb_mu); store_stat_set_(&s->rebuilding, 0); return -1; }
    for (round = 0; round < 8 && !stuck; ++round) {             /* the rows added meanwhile, into the private index */
        i32 c;
        pthread_rwlock_rdlock(&s->lock);
        c = s->count;
        if (c - nt->n > 256) store_snap_(s, c, 0, &sn);
        pthread_rwlock_unlock(&s->lock);
        if (c - nt->n <= 256) break;
        while (nt->n < c && !stuck) {
            TqAdd ad;
            i32 m = c - nt->n < STORE_LIVE_CHUNK ? c - nt->n : STORE_LIVE_CHUNK;
            if (tq_add_prep(nt, &sn.col, nt->n, m, nth, &ad) != 0) { stuck = 1; break; }
            if (tq_add_put(nt, &ad, nt->n) != 0) stuck = 1;
            tq_add_free(&ad);
        }
        store_snap_free_(&sn);
    }
    pthread_mutex_lock(&s->mt_mu);                               /* the live thread, between two chunks */
    pthread_rwlock_wrlock(&s->lock);
    if (!stuck && s->count > nt->n) {                            /* the last few, under the lock (at most a chunk) */
        TqCol sc, col = store_vcol_(s, &sc);
        TqAdd ad;
        i32 m = s->count - nt->n < STORE_LIVE_CHUNK ? s->count - nt->n : STORE_LIVE_CHUNK;
        if (tq_add_prep(nt, &col, nt->n, m, nth, &ad) == 0) { tq_add_put(nt, &ad, nt->n); tq_add_free(&ad); }
    }
    old = s->tq;
    if (old && old->nc > 1 && nt->nc > 1 && s->tuned_n > 0) {   /* the tuned share of the clusters, until re-tuned */
        i32 np = (i32)((f64)old->nprobe * (f64)nt->nc / (f64)old->nc + 0.5);
        nt->nprobe = np < 1 ? 1 : (np > nt->nc ? nt->nc : np);
    }
    s->tq = nt;
    s->built_upto = nt->n;                                       /* rows beyond it (if any) pend, as usual */
    s->tuned_n = 0;
    s->dead_built = nt->nc > 1 ? dead : 0;                       /* a flat index keeps its tombstones' slots */
    s->win_sum = 0.0; s->win_n = 0; s->drifted = 0;
    pthread_rwlock_unlock(&s->lock);
    pthread_mutex_unlock(&s->mt_mu);
    tq_free(old);
    pthread_mutex_unlock(&s->rb_mu);
    store_stat_add_(&s->n_rebuilds, 1);
    store_stat_set_(&s->rebuild_us, (i64)((tq_now_() - t0) * 1e6));
    store_stat_set_(&s->rebuilding, 0);
    return 0;
}

/* Why the index should be rebuilt now, 0 if it should not: 1 there is none yet, 2 the store grew into IVF size, 3 it
 * grew by live_grow since the build, 4 a window of added rows sat STORE_LIVE_DRIFT times farther from their centroids
 * than the built rows (store_fit_: the data moved), 5 a quarter of the index is tombstones, 6 the added rows piled into
 * a few clusters (tq_skew up by STORE_LIVE_SKEW: every query near them scans those big clusters). Nothing changed since the last
 * rebuild: 0. Under the read lock. */
static i32 store_due_(const RagStore *s)
{
    const Tq *t = s->tq;
    i32 dead = s->count - s->live - s->dead_built;
    if (s->idx || s->mods == store_stat_get_(&s->rebuilt_mods)) return 0;   /* an engine index: store_build's business */
    if (!t) return s->live >= STORE_LIVE_MIN ? 1 : 0;
    if (s->live_grow <= 0.0f) return 0;
    if (t->nc <= 1) return store_nclusters_(s->live) > 1 ? 2 : 0;
    if ((f64)t->nadd >= (f64)s->live_grow * (f64)(t->nbuilt > 0 ? t->nbuilt : 1)) return 3;
    if (s->drifted) return 4;
    if ((i64)dead * 4 > (i64)t->n) return 5;
    if (t->csize && t->skew0 > 0.0 && tq_skew(t) > STORE_LIVE_SKEW * t->skew0) return 6;
    return 0;
}

/* re-tune: a target is set and the index was rebuilt, grew by a tenth, or real traffic arrived to tune on */
static i32 store_tune_due_(RagStore *s)
{
    const Tq *t = s->tq;
    i32 nlog;
    if (s->tune_target <= 0.0f || !t || t->nc <= 1 || s->built_upto < 1000 || t->n != s->built_upto) return 0;
    if (s->tuned_n <= 0 || (i64)t->n * 10 >= (i64)s->tuned_n * 11) return 1;
    pthread_mutex_lock(&s->qlog_mu); nlog = s->qlog_n; pthread_mutex_unlock(&s->qlog_mu);
    return !s->tuned_log && nlog >= 64;
}

/* One maintenance step: index the pending rows, rebuild if the index has aged (store_due_) - async: on a thread of
 * its own, so this one goes on indexing meanwhile; else right here - and re-tune if a target recall is set and the
 * index changed. Returns what it did: 1 indexed, 2 rebuilt (or started to), 4 tuned. */
static void *store_rb_main_(void *p) { store_rebuild_((RagStore *)p, store_bg_threads_()); return (void *)0; }

static i32 store_step_(RagStore *s, i32 async)
{
    i32 did = 0, tune = 0, go = 0, nth = store_bg_threads_();
    pthread_mutex_lock(&s->mt_mu);
    if (store_index_pending_(s, nth) > 0) did |= 1;
    pthread_rwlock_rdlock(&s->lock);
    if (!store_stat_get_(&s->rebuilding)) {
        if (store_due_(s)) { store_stat_set_(&s->rebuilding, 1); go = 1; }
        else tune = store_tune_due_(s);                          /* not while the index is about to be replaced */
    }
    pthread_rwlock_unlock(&s->lock);
    if (tune && store_tune_run_(s, nth) >= 0.0f) did |= 4;
    pthread_mutex_unlock(&s->mt_mu);
    if (go) {
        did |= 2;
        if (async && s->rb_live) { pthread_join(s->rb_th, (void **)0); s->rb_live = 0; }   /* the last one has finished */
        if (!async || pthread_create(&s->rb_th, (const pthread_attr_t *)0, store_rb_main_, s) != 0) store_rebuild_(s, nth);
        else s->rb_live = 1;
    }
    return did;
}

/* One maintenance step, now, by hand (what the live thread runs again and again, here with the rebuild in this
 * thread): index the pending rows, rebuild if the index has aged, re-tune if a target recall is set and the index
 * changed. Searches go on throughout (the work takes half the cores); an add waits at most while a chunk of rows is
 * placed or an index swapped. Returns what it did: 1 indexed, 2 rebuilt, 4 tuned. */
i32 store_maintain(RagStore *s) { return store_step_(s, 0); }

static void *store_live_main_(void *p)
{
    RagStore *s = (RagStore *)p;
    for (;;) {
        i32 did, stop;
        pthread_mutex_lock(&s->mt_cmu); stop = s->mt_stop; pthread_mutex_unlock(&s->mt_cmu);
        if (stop) break;
        did = store_step_(s, 1);
        if (s->rb_live && !store_stat_get_(&s->rebuilding)) { pthread_join(s->rb_th, (void **)0); s->rb_live = 0; }
        pthread_mutex_lock(&s->mt_cmu);
        if (!did && !s->mt_stop) {                               /* nothing to do: sleep until an add, at most 50 ms */
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 50000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&s->mt_cv, &s->mt_cmu, &ts);
        }
        pthread_mutex_unlock(&s->mt_cmu);
    }
    if (s->rb_live) { pthread_join(s->rb_th, (void **)0); s->rb_live = 0; }   /* a rebuild in flight finishes first */
    return (void *)0;
}

static void store_live_stop_(RagStore *s)
{
    if (!s->live_on) return;
    pthread_mutex_lock(&s->mt_cmu); s->mt_stop = 1; pthread_cond_signal(&s->mt_cv); pthread_mutex_unlock(&s->mt_cmu);
    pthread_join(s->mt, (void **)0);
    pthread_rwlock_wrlock(&s->lock); s->live_on = 0; pthread_rwlock_unlock(&s->lock);
}

/* Live mode on or off. On: a thread of the store's own indexes rows as they are added - within ~50 ms; until then the
 * exact scan of the pending tail finds them, as always - and rebuilds the index off the lock as it ages: grown by
 * `grow` since the last build (1.0: doubled; 0: never rebuild), the added rows sitting STORE_LIVE_DRIFT times farther
 * from their centroids than the built ones (the data moved), or a quarter of it tombstones. With a target recall
 * (store_set_target_recall) it re-tunes the probes after every rebuild and every tenth more rows. A store with no
 * index yet gets one at STORE_LIVE_MIN rows. */
void store_set_live(RagStore *s, i32 on, f32 grow)
{
    pthread_mutex_lock(&s->mt_mu);
    s->live_grow = grow > 0.0f ? grow : 0.0f;
    pthread_mutex_unlock(&s->mt_mu);
    if (on && !s->live_on) {
        s->mt_stop = 0;
        if (pthread_create(&s->mt, (const pthread_attr_t *)0, store_live_main_, s) == 0) {
            pthread_rwlock_wrlock(&s->lock); s->live_on = 1; pthread_rwlock_unlock(&s->lock);
            pthread_cond_signal(&s->mt_cv);
        }
    } else if (!on) store_live_stop_(s);
}

/* what maintenance has done: rows indexed after a build, rebuilds, seconds the last rebuild took (any may be NULL) */
void store_live_stats(RagStore *s, i64 *indexed, i64 *rebuilds, f64 *last_rebuild_s)
{
    if (indexed) *indexed = store_stat_get_(&s->n_indexed);
    if (rebuilds) *rebuilds = store_stat_get_(&s->n_rebuilds);
    if (last_rebuild_s) *last_rebuild_s = 1e-6 * (f64)store_stat_get_(&s->rebuild_us);
}

static i32 store_search_impl_(RagStore *s, const f32 *query, i32 k,
                              i64 *out_doc_ids, f32 *out_scores,
                              i64 all_bits, i64 any_bits, int filtered)
{
    i32 dim = s->dim, over = filtered ? (k * 4 + 16) : k;
    f32 *qn  = (f32 *)malloc((size_t)dim * sizeof(f32));
    i64 *rid = (i64 *)malloc((size_t)over * sizeof(i64));
    f32 *rsc = (f32 *)malloc((size_t)over * sizeof(f32));
    EngScratch sc;
    f32 *slut = (f32 *)0;
    i32 have = 0, i, got, tail;
    TqCol mcol;

    memcpy(qn, query, (size_t)dim * sizeof(f32));
    store_normalise_(qn, dim);

    pthread_rwlock_rdlock(&s->lock);
    if (s->tune_target > 0.0f) store_qlog_(s, qn);
    mcol = store_tqcol_(&s->metac);

    if (s->tq) {
        i32 R = filtered ? k * 40 + 256 : (s->tq_rerank > 0 ? s->tq_rerank : (k * 20 > 256 ? k * 20 : 256)), m, nth = 1;
        i32 *rows = (i32 *)malloc((size_t)R * sizeof(i32));
        if (s->tq_threads > 0) nth = s->tq_threads;
        else if (s->tq->n >= 150000) { i32 c = store_ncpu_(); nth = c > 4 ? 4 : c; }   /* the scan is memory-bound: 4 threads already saturate the bus */
        m = tq_search(s->tq, qn, R, rows, s->dead, &mcol, all_bits, any_bits, filtered, nth, s->tq_probe);
        store_rescore_(s, qn, rows, m, k, out_doc_ids, out_scores, &have);
        /* a selective filter can leave the probed clusters short of k matches: then scan every cluster's codes
         * (RAM, 4 bits a row) for matches, never every f32 row */
        if (filtered && have < k && s->tq->nc > 1) {
            i32 j, z, n2 = tq_search(s->tq, qn, R, rows, s->dead, &mcol, all_bits, any_bits, filtered, nth, s->tq->nc);
            for (j = z = 0; j < n2; ++j) {
                i32 x, dup = 0;
                for (x = 0; x < have; ++x) if (out_doc_ids[x] == rows[j]) { dup = 1; break; }
                if (!dup) rows[z++] = rows[j];
            }
            store_rescore_(s, qn, rows, z, k, out_doc_ids, out_scores, &have);
        }
        free(rows);
    } else if (s->idx) {
        i32 sf = engine_scratch_floats(s->idx);
        slut = (f32 *)malloc((size_t)sf * sizeof(f32));
        sc.residual = slut;
        sc.lut      = slut + dim;
        sc.lids     = (i64 *)malloc((size_t)over * sizeof(i64));
        sc.lsc      = (f32 *)malloc((size_t)over * sizeof(f32));
        sc.probe    = (i32 *)malloc(64 * sizeof(i32));
        got = engine_search_into(s->idx, qn, over, rid, rsc, &sc);
        for (i = 0; i < got; ++i) {
            i32 row = (i32)rid[i];
            if (row < 0 || row >= s->built_upto || STORE_DEAD(s, row)) continue;
            if (filtered && !store_meta_ok_(STORE_META(s, row), all_bits, any_bits)) continue;
            eng_topk_(out_doc_ids, out_scores, k, &have, (i64)row, rsc[i]);
        }
        free(sc.lids); free(sc.lsc); free(sc.probe);
        /* engine path: a very selective filter can exclude the whole neighbourhood - exact filtered scan */
        if (filtered && have < k) {
            for (i = 0; i < s->built_upto; ++i) {
                i32 j, dup = 0;
                if (STORE_DEAD(s, i) || !store_meta_ok_(STORE_META(s, i), all_bits, any_bits)) continue;
                for (j = 0; j < have; ++j) if (out_doc_ids[j] == i) { dup = 1; break; }
                if (dup) continue;
                eng_topk_(out_doc_ids, out_scores, k, &have, (i64)i, 2.0f * store_dot_(s, qn, i) - 2.0f);
            }
        }
    }

    tail = s->count;
    for (i = s->built_upto; i < tail; ++i) {                    /* the pending tail: exact */
        if (STORE_DEAD(s, i)) continue;
        if (filtered && !store_meta_ok_(STORE_META(s, i), all_bits, any_bits)) continue;
        eng_topk_(out_doc_ids, out_scores, k, &have, (i64)i, 2.0f * store_dot_(s, qn, i) - 2.0f);
    }
    for (i = 0; i < have; ++i) out_doc_ids[i] = STORE_ID(s, out_doc_ids[i]);   /* rows -> doc ids, only for the k kept */

    pthread_rwlock_unlock(&s->lock);
    free(qn); free(rid); free(rsc);
    if (slut) free(slut);
    return have;
}

i32 store_search(RagStore *s, const f32 *query, i32 k,
                 i64 *out_doc_ids, f32 *out_scores)
{ return store_search_impl_(s, query, k, out_doc_ids, out_scores, 0, 0, 0); }

i32 store_search_filter(RagStore *s, const f32 *query, i32 k,
                        i64 *out_doc_ids, f32 *out_scores,
                        i64 all_bits, i64 any_bits)
{ return store_search_impl_(s, query, k, out_doc_ids, out_scores,
                            all_bits, any_bits, 1); }

/* ---- many queries at once ---- */
typedef struct { RagStore *s; const f32 *qn; i32 k, R; const i32 *rows, *cnt; i64 *ids; f32 *sc; } StoreBatch;

static void store_batch_fin_(void *p, i64 a, i64 b, i32 tid)
{
    StoreBatch *j = (StoreBatch *)p;
    RagStore *s = j->s;
    i64 q;
    (void)tid;
    for (q = a; q < b; ++q) {
        const f32 *qn = j->qn + (size_t)q * s->dim;
        i64 *ids = j->ids + (size_t)q * j->k;
        f32 *sc = j->sc + (size_t)q * j->k;
        i32 have = 0, i;
        store_rescore_(s, qn, j->rows + (size_t)q * j->R, j->cnt[q], j->k, ids, sc, &have);
        for (i = s->built_upto; i < s->count; ++i)               /* the pending tail: exact */
            if (!STORE_DEAD(s, i)) eng_topk_(ids, sc, j->k, &have, (i64)i, 2.0f * store_dot_(s, qn, i) - 2.0f);
        for (i = 0; i < have; ++i) ids[i] = STORE_ID(s, ids[i]);
        for (; i < j->k; ++i) { ids[i] = -1; sc[i] = -4.0f; }
    }
}

typedef struct { RagStore *s; const f32 *Q; i32 k; i64 *ids; f32 *sc; } StoreEach;
static void store_each_(void *p, i64 a, i64 b, i32 tid)
{
    StoreEach *j = (StoreEach *)p;
    i64 q;
    (void)tid;
    for (q = a; q < b; ++q) {
        i32 got = store_search(j->s, j->Q + (size_t)q * j->s->dim, j->k, j->ids + (size_t)q * j->k, j->sc + (size_t)q * j->k);
        for (; got < j->k; ++got) { j->ids[(size_t)q * j->k + got] = -1; j->sc[(size_t)q * j->k + got] = -4.0f; }
    }
}

/* nq queries at once, no filter: out_ids / out_scores are nq x k (a slot a query could not fill: id -1). The scan
 * runs cluster by cluster for the whole batch, so codes come in from memory once per batch instead of once per
 * query; the exact re-score then runs query by query on nthreads threads. Same answers as store_search per query.
 * For throughput (bulk jobs, benchmarks); a server answering one query at a time uses store_search. */
void store_search_batch(RagStore *s, const f32 *Q, i32 nq, i32 k, i64 *out_ids, f32 *out_scores, i32 nthreads)
{
    i32 dim = s->dim, R, off, i;
    f32 *qn;
    if (nq <= 0 || k <= 0) return;
    if (nthreads < 1) nthreads = 1;
    if (!s->tq) {                                                /* no scan index: one query at a time, in parallel */
        StoreEach e;
        e.s = s; e.Q = Q; e.k = k; e.ids = out_ids; e.sc = out_scores;
        tq_par2_(store_each_, &e, nq, nthreads);
        return;
    }
    qn = (f32 *)malloc((size_t)nq * (size_t)dim * sizeof(f32));
    memcpy(qn, Q, (size_t)nq * (size_t)dim * sizeof(f32));
    for (i = 0; i < nq; ++i) store_normalise_(qn + (size_t)i * dim, dim);
    pthread_rwlock_rdlock(&s->lock);
    if (s->tune_target > 0.0f) for (i = 0; i < nq; i += 1 + nq / STORE_QLOG) store_qlog_(s, qn + (size_t)i * dim);
    R = s->tq_rerank > 0 ? s->tq_rerank : (k * 20 > 256 ? k * 20 : 256);
    for (off = 0; off < nq; off += 1024) {                       /* batches of 1024 bound the per-query candidates */
        i32 m = nq - off < 1024 ? nq - off : 1024;
        i32 *rows = (i32 *)malloc((size_t)m * (size_t)R * sizeof(i32)), *cnt = (i32 *)malloc((size_t)m * sizeof(i32));
        StoreBatch j;
        tq_search_many(s->tq, qn + (size_t)off * dim, m, R, rows, cnt, s->dead, nthreads, s->tq_probe);
        j.s = s; j.qn = qn + (size_t)off * dim; j.k = k; j.R = R; j.rows = rows; j.cnt = cnt;
        j.ids = out_ids + (size_t)off * k; j.sc = out_scores + (size_t)off * k;
        tq_par2_(store_batch_fin_, &j, m, nthreads);
        free(rows); free(cnt);
    }
    pthread_rwlock_unlock(&s->lock);
    free(qn);
}

/* ------------------------------------------------------------------ *
 * persistence - .vxi: metadata + compacted rows + the built index    *
 * ------------------------------------------------------------------ */

/* .vxi scalars go through engine.c's little-endian helpers (engs_wle_ /
 * engs_rle_, in scope because store.c includes engine.c with
 * ENGINE_SERIALIZE): 4-byte header words, then the EngGoal fields one by
 * one (never the raw struct - padding/enum width is not portable), then
 * the row arrays as LE f32 / i64. A big-endian host byte-swaps here. */
#define STORE_W32(f, p, n) engs_wle_((f), (p), 4, (n))
#define STORE_R32(f, p, n) engs_rle_((f), (p), 4, (n))
#define STORE_W64(f, p, n) engs_wle_((f), (p), 8, (n))
#define STORE_R64(f, p, n) engs_rle_((f), (p), 8, (n))

static int store_goal_w_(FILE *f, const EngGoal *g)
{
    i32 w[7]; union { f32 fv; i32 iv; } tr;
    w[0] = (i32)g->mode;    w[1] = g->k;         w[2] = g->max_code_bytes;
    w[3] = g->eval_queries; w[4] = g->n_iters;   w[5] = (i32)g->seed;
    w[6] = g->max_candidates;
    tr.fv = g->target_recall;
    return STORE_W32(f, w, 7) || STORE_W32(f, &tr.iv, 1) ? -1 : 0;
}
static int store_goal_r_(FILE *f, EngGoal *g)
{
    i32 w[7]; union { f32 fv; i32 iv; } tr;
    if (STORE_R32(f, w, 7) || STORE_R32(f, &tr.iv, 1)) return -1;
    g->mode = (EngMode)w[0]; g->k = w[1];  g->max_code_bytes = w[2];
    g->eval_queries = w[3];  g->n_iters = w[4]; g->seed = (u32)w[5];
    g->max_candidates = w[6];
    g->target_recall = tr.fv;
    return 0;
}

/* ---- journal: PATH.j = 'VXJ1' base_live(i32) base_bytes(i64) dim(i32), then records A/D/M ---- */
#define STORE_JMAGIC 0x314a5856u

/* flush a stdio file all the way to the disk */
static i32 store_fsync_(FILE *f)
{
    if (fflush(f) != 0) return -1;
    return fsync(fileno(f)) == 0 ? 0 : -1;
}

/* make a rename durable: fsync the directory that holds `path` */
static void store_fsync_dir_(const char *path)
{
    char *d = (char *)malloc(strlen(path) + 2), *sl;
    int fd;
    strcpy(d, path);
    sl = strrchr(d, '/');
    if (sl) { if (sl == d) sl[1] = 0; else *sl = 0; } else strcpy(d, ".");
    fd = open(d, O_RDONLY);
    if (fd >= 0) { fsync(fd); close(fd); }
    free(d);
}

static i64 store_file_size_(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (i64)st.st_size : -1;
}

static char *store_jname_(const char *path)
{
    char *j = (char *)malloc(strlen(path) + 3);
    strcpy(j, path); strcat(j, ".j");
    return j;
}

/* the base file at `path` was just written (or loaded): start a fresh journal against it */
static void store_journal_reset_(RagStore *s, const char *path)
{
    char *jn = store_jname_(path);
    u32 magic = STORE_JMAGIC;
    i32 live = s->count, dim = s->dim;
    i64 bytes = store_file_size_(path);
    if (s->jf) { fclose(s->jf); s->jf = (FILE *)0; }
    free(s->jpath); s->jpath = (char *)0;
    s->jf = fopen(jn, "wb");
    if (s->jf) {
        fwrite(&magic, 4, 1, s->jf); fwrite(&live, 4, 1, s->jf); fwrite(&bytes, 8, 1, s->jf); fwrite(&dim, 4, 1, s->jf);
        store_fsync_(s->jf);
        s->jpath = (char *)malloc(strlen(path) + 1); strcpy(s->jpath, path);
    }
    free(jn);
}

/* replay PATH.j onto a freshly loaded store; a torn last record is ignored; a journal that belongs to another
 * base is ignored. Afterwards the journal stays open for appending. */
static void store_journal_replay_(RagStore *s, const char *path)
{
    char *jn = store_jname_(path);
    FILE *f = fopen(jn, "rb");
    u32 magic; i32 live, dim; i64 bytes;
    f32 *vec = (f32 *)malloc((size_t)s->dim * sizeof(f32));
    if (f && fread(&magic, 4, 1, f) == 1 && fread(&live, 4, 1, f) == 1 && fread(&bytes, 8, 1, f) == 1 && fread(&dim, 4, 1, f) == 1
        && magic == STORE_JMAGIC && live == s->count && dim == s->dim && bytes == store_file_size_(path)) {
        long good = ftell(f);
        for (;;) {
            u8 op; i64 id, meta;
            if (fread(&op, 1, 1, f) != 1) break;
            if (op == 'A') {
                if (fread(&id, 8, 1, f) != 1 || fread(&meta, 8, 1, f) != 1 || fread(vec, sizeof(f32), (size_t)s->dim, f) != (size_t)s->dim) break;
                store_add_meta(s, vec, id, meta);
            } else if (op == 'D') {
                if (fread(&id, 8, 1, f) != 1) break;
                store_delete(s, id);
            } else if (op == 'M') {
                if (fread(&id, 8, 1, f) != 1 || fread(&meta, 8, 1, f) != 1) break;
                store_set_meta(s, id, meta);
            } else break;
            good = ftell(f);
        }
        fclose(f); f = (FILE *)0;
        /* drop a torn tail, then append after the good part. If the tail cannot be dropped, run
         * without a journal (as when fopen fails): records appended behind a torn one would never
         * be replayed. */
        if (truncate(jn, (off_t)good) == 0) {
            s->jf = fopen(jn, "ab");
            if (s->jf) { s->jpath = (char *)malloc(strlen(path) + 1); strcpy(s->jpath, path); }
        }
    } else {
        if (f) fclose(f);
        store_journal_reset_(s, path);
    }
    free(vec); free(jn);
}

/* v5 layout: every array starts on a 64-byte boundary, so store_load can map the file and point straight into
 * it (no copy, pages come in when first touched). The scan index (tq) is saved as well: a load is O(header),
 * not a rebuild. */
#define STORE_AL(x) (((size_t)(x) + 63u) & ~(size_t)63u)
static int store_pad_(FILE *f)
{
    static const u8 z[64];
    long pos = ftell(f);
    size_t pad;
    if (pos < 0) return -1;
    pad = STORE_AL(pos) - (size_t)pos;
    return pad && fwrite(z, 1, pad, f) != pad ? -1 : 0;
}

/* rows [0, n) of a column, chunk by chunk */
static int store_col_w_(FILE *f, const SCol *c, i64 n)
{
    i32 i;
    size_t per = c->elem;
    for (i = 0; (i64)i << STORE_CSHIFT < n; ++i) {
        i64 rows = n - ((i64)i << STORE_CSHIFT);
        if (rows > STORE_CROWS) rows = STORE_CROWS;
        if (engs_wle_(f, c->chunk[i], per, (size_t)rows * (c->esz / per))) return -1;
    }
    return 0;
}

/* The blocks go out cluster by cluster - the built ones, then those added since - so the file holds a plain index
 * again (a flat index without slot_row adds rows in order, so its blocks stay in row order) */
static int store_tq_w_(FILE *f, const Tq *t)
{
    i32 h[8], c, nb, i, nc1 = t->nc > 1 ? t->nc : 1, *bl, *cs, bad = 0;
    size_t bb = (size_t)t->vd * 16;
    h[0] = t->dim; h[1] = t->dpad; h[2] = t->n; h[3] = t->nblk + t->xn; h[4] = t->nc; h[5] = t->nprobe;
    h[6] = t->slot_row != (i32 *)0; h[7] = t->bits == 2 ? 2 : 0;                     /* 0 = 4-bit (older files) */
    if (store_pad_(f) || STORE_W32(f, h, 8)
        || store_pad_(f) || STORE_W32(f, t->s1, (size_t)t->dpad)
        || store_pad_(f) || STORE_W32(f, t->s2, (size_t)t->dpad)
        || store_pad_(f) || STORE_W32(f, t->perm, (size_t)t->dpad)) return -1;
    bl = (i32 *)malloc((size_t)(h[3] > 0 ? h[3] : 1) * sizeof(i32));
    cs = (i32 *)malloc((size_t)(nc1 + 1) * sizeof(i32));
    for (nb = 0, c = 0; c < nc1; ++c) { cs[c] = nb; nb += tq_cblocks_(t, c, bl + nb); }
    cs[nc1] = nb;
    if (t->nc > 1 && (store_pad_(f) || STORE_W32(f, t->cent, (size_t)t->nc * (size_t)t->dim)
                      || store_pad_(f) || STORE_W32(f, cs, (size_t)t->nc + 1))) bad = 1;
    if (!bad && h[6]) {
        bad = store_pad_(f) != 0;
        for (i = 0; i < nb && !bad; ++i)
            bad = STORE_W32(f, bl[i] < t->nblk ? t->slot_row + (size_t)bl[i] * 32 : t->xslot + (size_t)(bl[i] - t->nblk) * 32, 32) != 0;
    }
    if (!bad) bad = store_pad_(f) != 0;
    for (i = 0; i < nb && !bad; ++i) bad = fwrite(tq_bptr_(t, bl[i]), 1, bb, f) != bb;
    free(bl); free(cs);
    return bad ? -1 : 0;
}

i32 store_save(RagStore *s, const char *path)
{
    FILE *f;
    char *tmp;
    i32 hdr[4], ext[4];
    i32 live, had_idx;
    u8 kind;

    pthread_mutex_lock(&s->rb_mu);                               /* a compaction must not run under a live rebuild */
    pthread_mutex_lock(&s->mt_mu);
    store_index_pending_(s, store_ncpu_());                      /* the pending tail into the index, searches going on */
    pthread_rwlock_wrlock(&s->lock);
    store_index_locked_(s);                                      /* and what came in meanwhile: the file gets the index */
    had_idx = s->idx != (EngIndex *)0;
    live = store_compact_(s);                                    /* the scan index follows its rows */
    if (had_idx && !s->idx && live > 0) { store_engine_build_(s, live); s->built_upto = live; }   /* an engine index cannot */
    tmp = (char *)malloc(strlen(path) + 5); strcpy(tmp, path); strcat(tmp, ".tmp");
    f = fopen(tmp, "wb");                                        /* the old file stays intact until the new one is complete and on disk */
    if (!f) { free(tmp); pthread_rwlock_unlock(&s->lock); pthread_mutex_unlock(&s->mt_mu); pthread_mutex_unlock(&s->rb_mu); return -1; }

    hdr[0] = (i32)STORE_MAGIC; hdr[1] = s->vbytes == 1 ? STORE_VERSION_I8 : STORE_VERSION;   /* f32 stores stay v5 */
    hdr[2] = s->dim; hdr[3] = live;
    ext[0] = 1; ext[1] = ext[2] = ext[3] = 0;                    /* v6: rows are int8 */
    /* 0: no index (load rebuilds), 1: engine index, 2: the scan index */
    kind = (u8)(s->idx != (EngIndex *)0 && s->built_upto == live ? 1
              : (s->tq && s->built_upto == live && s->tq->n == live ? 2 : 0));

    store_advise_(s, MADV_SEQUENTIAL);                           /* the save streams every row: read ahead */
    if (STORE_W32(f, hdr, 4)
        || (s->vbytes == 1 && STORE_W32(f, ext, 4))
        || store_goal_w_(f, &s->goal)
        || store_pad_(f) || store_col_w_(f, &s->vec, live)
        || (s->vbytes == 1 && (store_pad_(f) || store_col_w_(f, &s->vsc, live)))
        || store_pad_(f) || store_col_w_(f, &s->idc, live)
        || store_pad_(f) || store_col_w_(f, &s->metac, live)
        || fwrite(&kind, 1, 1, f) != 1
        || (kind == 1 && engine_save(s->idx, f))
        || (kind == 2 && store_tq_w_(f, s->tq))) {
        store_advise_(s, MADV_RANDOM);
        fclose(f); remove(tmp); free(tmp); pthread_rwlock_unlock(&s->lock); pthread_mutex_unlock(&s->mt_mu); pthread_mutex_unlock(&s->rb_mu); return -1;
    }
    store_advise_(s, MADV_RANDOM);
    if (store_fsync_(f) != 0 || fclose(f) != 0 || rename(tmp, path) != 0) {
        remove(tmp); free(tmp); pthread_rwlock_unlock(&s->lock); pthread_mutex_unlock(&s->mt_mu); pthread_mutex_unlock(&s->rb_mu); return -1;
    }
    free(tmp);
    store_fsync_dir_(path);
    store_journal_reset_(s, path);
    pthread_rwlock_unlock(&s->lock);
    pthread_mutex_unlock(&s->mt_mu);
    pthread_mutex_unlock(&s->rb_mu);
    return 0;
}

/* Cheap durable save: the first call (or one after the base changed) writes the full file; later calls only
 * flush the journal of what was added/deleted since, so the cost tracks the change, not the store size.
 * store_load replays the journal. Returns 0 on success. */
i32 store_sync(RagStore *s, const char *path)
{
    i32 r;
    if (s->jf && s->jpath && strcmp(s->jpath, path) == 0) {
        pthread_rwlock_wrlock(&s->lock);
        r = store_fsync_(s->jf);
        pthread_rwlock_unlock(&s->lock);
        return r;
    }
    return store_save(s, path);
}

/* n elements of esz bytes at byte offset off: a pointer into the mapping when there is one (no copy), else
 * read (and byte-swapped on a big-endian host) into malloc'd memory. NULL when the range is not inside the file. */
static void *store_take_(u8 *map, size_t len, FILE *f, size_t off, size_t esz, size_t n)
{
    void *p;
    if (off > len || esz * n > len - off) return (void *)0;
    if (map) return (void *)(map + off);
    p = malloc(esz * (n ? n : 1));
    if (!p) return (void *)0;
    if (fseek(f, (long)off, SEEK_SET) != 0 || (n && engs_rle_(f, p, esz, n))) { free(p); return (void *)0; }
    return p;
}

/* a column of `rows` rows at byte offset off: chunks point into the mapping, or are read into fresh chunks */
static i32 store_col_r_(RagStore *s, SCol *c, u8 *map, size_t len, FILE *f, size_t off, i32 rows)
{
    i32 nch = (i32)(((i64)rows + STORE_CROWS - 1) >> STORE_CSHIFT), i;
    size_t per = c->elem;
    if (off > len || (size_t)rows * c->esz > len - off) return -1;
    for (i = 0; i < nch; ++i) {
        i64 n = (i64)rows - ((i64)i << STORE_CSHIFT);
        if (n > STORE_CROWS) n = STORE_CROWS;
        if (map) {
            if (c->nchunk == c->ccap) {
                i32 cc = c->ccap ? c->ccap * 2 : 16;
                c->chunk = (u8 **)realloc(c->chunk, (size_t)cc * sizeof(u8 *)); c->how = (u8 *)realloc(c->how, (size_t)cc);
                c->foff = (i64 *)realloc(c->foff, (size_t)cc * sizeof(i64)); c->ccap = cc;
            }
            c->chunk[c->nchunk] = map + off + (size_t)i * (size_t)STORE_CROWS * c->esz;
            c->how[c->nchunk] = STORE_MAPPED; c->foff[c->nchunk] = -1;
            ++c->nchunk;
        } else {
            if (scol_push_(s, c)) return -1;
            if (fseek(f, (long)(off + (size_t)i * (size_t)STORE_CROWS * c->esz), SEEK_SET) != 0
                || engs_rle_(f, c->chunk[i], per, (size_t)n * (c->esz / per))) return -1;
        }
    }
    return 0;
}

static Tq *store_tq_r_(u8 *map, size_t len, FILE *f, size_t off, i32 rows)
{
    i32 h[8];
    Tq *t;
    size_t o = STORE_AL(off);
    i32 *hp = (i32 *)store_take_(map, len, f, o, 4, 8);
    if (!hp) return (Tq *)0;
    memcpy(h, hp, sizeof(h));
    if (!map) free(hp);
    if (h[0] <= 0 || h[1] < h[0] || h[2] != rows || h[3] < 0 || h[4] < 0) return (Tq *)0;
    t = (Tq *)calloc(1, sizeof(Tq));
    t->dim = h[0]; t->dpad = h[1]; t->n = h[2]; t->nblk = h[3]; t->nc = h[4]; t->nprobe = h[5];
    t->nbuilt = t->n;                                            /* growth counts from the load (fit0: on first add) */
    t->bits = h[7] == 2 ? 2 : 4; t->vd = tq_vd_(t->dpad, t->bits);
    t->mapped = map != (u8 *)0;
    o = STORE_AL(o + 32);
    t->s1 = (f32 *)store_take_(map, len, f, o, 4, (size_t)t->dpad); o = STORE_AL(o + (size_t)t->dpad * 4);
    t->s2 = (f32 *)store_take_(map, len, f, o, 4, (size_t)t->dpad); o = STORE_AL(o + (size_t)t->dpad * 4);
    t->perm = (i32 *)store_take_(map, len, f, o, 4, (size_t)t->dpad); o = STORE_AL(o + (size_t)t->dpad * 4);
    if (t->nc > 1) {
        t->cent = (f32 *)store_take_(map, len, f, o, 4, (size_t)t->nc * (size_t)t->dim);
        o = STORE_AL(o + (size_t)t->nc * (size_t)t->dim * 4);
        t->cstart = (i32 *)store_take_(map, len, f, o, 4, (size_t)t->nc + 1);
        o = STORE_AL(o + ((size_t)t->nc + 1) * 4);
    }
    if (h[6]) { t->slot_row = (i32 *)store_take_(map, len, f, o, 4, (size_t)t->nblk * 32); o = STORE_AL(o + (size_t)t->nblk * 128); }
    t->codes = (u8 *)store_take_(map, len, f, o, 1, (size_t)t->nblk * (size_t)t->vd * 16);
    if (!t->s1 || !t->s2 || !t->perm || (t->nc > 1 && (!t->cent || !t->cstart)) || (h[6] && !t->slot_row) || !t->codes) {
        tq_free(t);
        return (Tq *)0;
    }
    tq_prepare_(t);
    if (map) {                                                   /* the codes are scanned on every query: start reading them now */
        size_t pg = (size_t)sysconf(_SC_PAGESIZE), a = (size_t)t->codes & ~(pg - 1);
        madvise((void *)a, (size_t)t->codes + (size_t)t->nblk * (size_t)t->vd * 16 - a, MADV_WILLNEED);
    }
    return t;
}

/* v5: map the file privately (copy-on-write: the store may still edit rows in place, the file is never
 * written through the mapping) and point the columns and the scan index into it. Nothing is read up front;
 * pages arrive on first touch, the id lookups are built on first use. */
static RagStore *store_load_v5_(Arena *a, const char *path, FILE *f, const i32 *hdr)
{
    RagStore *s;
    i32 live = hdr[3], dim = hdr[2], ext[4], i8rows = hdr[1] == STORE_VERSION_I8;
    struct stat st;
    size_t len, o_vecs, o_vsc = 0, o_ids, o_meta, o_kind;
    u8 *map = (u8 *)0, kind = 0;

    if (live < 0 || dim <= 0 || fstat(fileno(f), &st) != 0) { fclose(f); return (RagStore *)0; }
    len = (size_t)st.st_size;
    if (i8rows && (STORE_R32(f, ext, 4) || ext[0] != 1)) { fclose(f); return (RagStore *)0; }
    s = store_new(a, dim);
    if (i8rows) store_set_vec_bytes(s, 1);
    if (store_goal_r_(f, &s->goal)) { fclose(f); store_destroy(s); return (RagStore *)0; }
    o_vecs = STORE_AL((i8rows ? 32 : 16) + 32);
    if (i8rows) { o_vsc = STORE_AL(o_vecs + (size_t)live * (size_t)dim); o_ids = STORE_AL(o_vsc + (size_t)live * 4); }
    else o_ids = STORE_AL(o_vecs + (size_t)live * (size_t)dim * 4);
    o_meta = STORE_AL(o_ids + (size_t)live * 8);
    o_kind = o_meta + (size_t)live * 8;
    if (o_kind >= len) { fclose(f); store_destroy(s); return (RagStore *)0; }
    if (!eng_host_be_() && live > 0) {
        void *m = mmap((void *)0, len, PROT_READ | PROT_WRITE, MAP_PRIVATE, fileno(f), 0);
        if (m != MAP_FAILED) { map = (u8 *)m; s->map = m; s->map_len = len; }
    }
    if (live > 0) {
        if (store_col_r_(s, &s->vec, map, len, f, o_vecs, live) || store_col_r_(s, &s->idc, map, len, f, o_ids, live)
            || store_col_r_(s, &s->metac, map, len, f, o_meta, live)
            || (i8rows && store_col_r_(s, &s->vsc, map, len, f, o_vsc, live))) { fclose(f); store_destroy(s); return (RagStore *)0; }
        s->dead_words = ((i64)live + 63) >> 6;
        s->dead = (u64 *)calloc((size_t)s->dead_words, sizeof(u64));
        if (map) {                                               /* re-scoring reads rows at random: no read-ahead */
            size_t pg = (size_t)sysconf(_SC_PAGESIZE), b = o_vecs - o_vecs % pg;
            madvise(map + b, o_ids - b, MADV_RANDOM);
        }
    }
    s->count = live;
    s->live = live;
    s->idmap_ok = live == 0;
    if (map) kind = map[o_kind];
    else if (fseek(f, (long)o_kind, SEEK_SET) != 0 || fread(&kind, 1, 1, f) != 1) { fclose(f); store_destroy(s); return (RagStore *)0; }

    if (kind == 1) {
        if (fseek(f, (long)(o_kind + 1), SEEK_SET) != 0) { fclose(f); store_destroy(s); return (RagStore *)0; }
        s->idx = engine_load(&s->idx_arena, f);
        fclose(f);
        if (!s->idx) { store_destroy(s); return (RagStore *)0; }
        s->built_upto = live;
    } else {
        if (kind == 2) s->tq = store_tq_r_(map, len, f, o_kind + 1, live);
        fclose(f);
        s->fast = 1; s->fast_target = 0.97f;                     /* fast stores are saved without an engine index */
        if (s->tq) s->built_upto = live;
        else store_build(s);                                      /* no saved scan index (or an unreadable one): build it */
    }
    store_journal_replay_(s, path);
    return s;
}

RagStore *store_load(Arena *a, const char *path)
{
    FILE *f = fopen(path, "rb");
    i32 hdr[4];
    RagStore *s;
    i32 live, i;
    u8 has_idx;
    if (!f) return (RagStore *)0;
    if (STORE_R32(f, hdr, 4) || (u32)hdr[0] != STORE_MAGIC
        || (hdr[1] != 4 && hdr[1] != STORE_VERSION && hdr[1] != STORE_VERSION_I8)) { fclose(f); return (RagStore *)0; }
    if (hdr[1] == STORE_VERSION || hdr[1] == STORE_VERSION_I8) return store_load_v5_(a, path, f, hdr);

    /* v4: packed sections, no saved scan index - read everything, rebuild the scan */
    s = store_new(a, hdr[2]);
    live = hdr[3];
    if (live < 0 || store_goal_r_(f, &s->goal) || store_reserve_(s, live > 0 ? live : 1)) { fclose(f); store_destroy(s); return (RagStore *)0; }
    for (i = 0; i < live; ++i) if (STORE_R32(f, STORE_VEC(s, i), (size_t)s->dim)) { fclose(f); store_destroy(s); return (RagStore *)0; }
    for (i = 0; i < live; ++i) if (STORE_R64(f, &STORE_ID(s, i), 1)) { fclose(f); store_destroy(s); return (RagStore *)0; }
    for (i = 0; i < live; ++i) if (STORE_R64(f, &STORE_META(s, i), 1)) { fclose(f); store_destroy(s); return (RagStore *)0; }
    s->count = live;
    store_reindex_(s);

    if (fread(&has_idx, 1, 1, f) != 1) { fclose(f); store_destroy(s); return (RagStore *)0; }
    if (has_idx) {
        s->idx = engine_load(&s->idx_arena, f);
        fclose(f);
        if (!s->idx) { store_destroy(s); return (RagStore *)0; }
        s->built_upto = live;
    } else {
        fclose(f);
        s->fast = 1; s->fast_target = 0.97f;                     /* fast stores are saved without an engine index */
        store_build(s);
    }
    store_journal_replay_(s, path);
    return s;
}
