/* vecindex.c - the C API of include/vecindex.h over store.c.
 *
 * Built as a shared library with -fvisibility=hidden, so only the VI_API functions are exported; everything else in
 * the translation unit (the store, the indexes) stays internal. ./run lib builds .build/libvecindex.so and
 * .build/libvecindex.a and runs tests/api_test.c against both. By hand:
 *
 *   gcc -std=c89 -pedantic -O2 -march=native -fPIC -shared -fvisibility=hidden -D_POSIX_C_SOURCE=200112L \
 *       -Isrc -Iinclude src/vecindex.c -o libvecindex.so -lm -lpthread
 *
 * Or compile it into a program like any other part of vecindex: one translation unit, -Isrc -Iinclude. */
#include "store.c"
#include "vecindex.h"

struct vi_db {
    Arena arena;                /* the store lives in it */
    RagStore *s;
    char *path;                 /* the file it was opened from; NULL: in memory */
};

static char *vi_strdup_(const char *p)
{
    char *d = (char *)malloc(strlen(p) + 1);
    if (d) strcpy(d, p);
    return d;
}

VI_API const char *vi_version(void) { return VI_VERSION; }

VI_API vi_db *vi_open(const char *path, int dim, int flags)
{
    vi_db *db = (vi_db *)calloc(1, sizeof(vi_db));
    struct stat st;
    if (!db) return (vi_db *)0;
    if (path && stat(path, &st) == 0) {
        db->s = store_load(&db->arena, path);                    /* there is a file: load it or fail, never replace it */
        if (!db->s) { arena_free(&db->arena); free(db); return (vi_db *)0; }
    } else {
        if (dim <= 0) { free(db); return (vi_db *)0; }
        db->s = store_new(&db->arena, (i32)dim);
    }
    if (path) {
        db->path = vi_strdup_(path);
        if (!db->path || (!db->s->jf && store_save(db->s, path) != 0)) {   /* a new file exists from the start, */
            store_destroy(db->s); arena_free(&db->arena); free(db->path); free(db);   /* with its journal */
            return (vi_db *)0;
        }
    }
    if (flags & VI_LIVE) store_set_live(db->s, 1, 1.0f);
    return db;
}

VI_API void vi_close(vi_db *db)
{
    if (!db) return;
    if (db->path) store_sync(db->s, db->path);
    store_destroy(db->s);                                        /* waits for a running snapshot */
    arena_free(&db->arena);
    free(db->path);
    free(db);
}

/* ---- tenants without a server: a directory of stores, opened on first use ---- */

typedef struct vi_ten { char name[65]; vi_db *db; struct vi_ten *next; } vi_ten;
struct vi_dir {
    char *path;
    int dim, flags;
    pthread_mutex_t mu;         /* the list, and opening a store into it */
    vi_ten *head;
};

VI_API vi_dir *vi_dir_open(const char *path, int dim, int flags)
{
    struct stat st;
    vi_dir *d;
    if (!path || stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) return (vi_dir *)0;
    d = (vi_dir *)calloc(1, sizeof(vi_dir));
    if (!d || !(d->path = vi_strdup_(path))) { free(d); return (vi_dir *)0; }
    d->dim = dim; d->flags = flags;
    pthread_mutex_init(&d->mu, (const pthread_mutexattr_t *)0);
    return d;
}

VI_API vi_db *vi_dir_get(vi_dir *d, const char *name)
{
    vi_ten *t;
    size_t n = name ? strlen(name) : 0, i;
    if (n < 1 || n > 64) return (vi_db *)0;
    for (i = 0; i < n; ++i) {
        char ch = name[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) return (vi_db *)0;
    }
    pthread_mutex_lock(&d->mu);
    for (t = d->head; t; t = t->next) if (!strcmp(t->name, name)) break;
    if (!t) {                                                    /* first use: open it, or create it there */
        char *p = (char *)malloc(strlen(d->path) + n + 6);
        t = (vi_ten *)calloc(1, sizeof(vi_ten));
        if (p && t) {
            sprintf(p, "%s/%s.vxi", d->path, name);
            t->db = vi_open(p, d->dim, d->flags);
        }
        free(p);
        if (t && t->db) { memcpy(t->name, name, n + 1); t->next = d->head; d->head = t; }
        else { free(t); t = (vi_ten *)0; }
    }
    pthread_mutex_unlock(&d->mu);
    return t ? t->db : (vi_db *)0;
}

VI_API void vi_dir_close(vi_dir *d)
{
    vi_ten *t, *nx;
    if (!d) return;
    for (t = d->head; t; t = nx) { nx = t->next; vi_close(t->db); free(t); }
    pthread_mutex_destroy(&d->mu);
    free(d->path);
    free(d);
}

VI_API int vi_dim(const vi_db *db) { return db->s->dim; }

VI_API vi_i64 vi_count(vi_db *db) { return (vi_i64)store_count(db->s); }

VI_API int vi_add(vi_db *db, vi_i64 id, const float *vec) { return store_put(db->s, vec, (i64)id, 0); }

VI_API int vi_add_batch(vi_db *db, int n, const vi_i64 *ids, const float *vecs)
{
    int i, ok = 0;
    for (i = 0; i < n; ++i) if (store_put(db->s, vecs + (size_t)i * (size_t)db->s->dim, (i64)ids[i], 0) == 0) ++ok;
    return ok;
}

VI_API int vi_delete(vi_db *db, vi_i64 id) { return store_delete(db->s, (i64)id); }

VI_API int vi_delete_range(vi_db *db, vi_i64 lo, vi_i64 hi) { return store_delete_range(db->s, (i64)lo, (i64)hi); }

VI_API int vi_get(vi_db *db, vi_i64 id, float *out) { return store_get_vec(db->s, (i64)id, out); }

static i32 vi_len_(const char *text, int len) { return len < 0 ? (text ? (i32)strlen(text) : 0) : (i32)len; }

VI_API void vi_set_aggression(vi_db *db, float a) { store_set_aggression(db->s, a); }

VI_API int vi_set_memory(vi_db *db, int tier) { return store_set_memory(db->s, (i32)tier); }

VI_API void vi_memory(vi_db *db, vi_i64 *hot, vi_i64 *cold)
{
    i64 h, c;
    store_tiers(db->s, &h, &c);
    if (hot) *hot = (vi_i64)h;
    if (cold) *cold = (vi_i64)c;
}

VI_API int vi_add_text(vi_db *db, vi_i64 id, const float *vec, const char *text, int len)
{ return store_put_text(db->s, vec, (i64)id, 0, text, vi_len_(text, len)); }

VI_API int vi_get_text(vi_db *db, vi_i64 id, char *buf, int cap) { return store_get_text(db->s, (i64)id, buf, (i32)cap); }

struct vi_batch { StoreWb w; };

VI_API vi_batch *vi_batch_begin(vi_db *db)
{
    vi_batch *b = (vi_batch *)malloc(sizeof(vi_batch));
    if (b) store_batch_init(&b->w, db->s);
    return b;
}

VI_API int vi_batch_add(vi_batch *b, vi_i64 id, const float *vec) { return store_batch_put(&b->w, vec, (i64)id, 0); }

VI_API int vi_batch_add_text(vi_batch *b, vi_i64 id, const float *vec, const char *text, int len)
{ return store_batch_put_text(&b->w, vec, (i64)id, 0, text, vi_len_(text, len)); }

VI_API int vi_batch_delete(vi_batch *b, vi_i64 id) { return store_batch_delete(&b->w, (i64)id); }

VI_API int vi_batch_delete_range(vi_batch *b, vi_i64 lo, vi_i64 hi) { return store_batch_delete_range(&b->w, (i64)lo, (i64)hi); }

VI_API int vi_batch_commit(vi_batch *b, int sync)
{
    i32 r = store_batch_commit(&b->w, (i32)sync);
    store_batch_free(&b->w);
    free(b);
    return r;
}

VI_API void vi_batch_abort(vi_batch *b)
{
    if (!b) return;
    store_batch_free(&b->w);
    free(b);
}

VI_API int vi_range(vi_db *db, vi_i64 lo, vi_i64 hi, vi_i64 *ids, int max)
{ return store_range(db->s, (i64)lo, (i64)hi, (i64 *)ids, (i32)max); }

VI_API int vi_search_hybrid(vi_db *db, const float *query, const char *text, int tlen, int k, vi_i64 *ids, float *scores)
{ return store_search_hybrid(db->s, query, text, vi_len_(text, tlen), (i32)k, (i64 *)ids, scores); }

VI_API int vi_set_tag(vi_db *db, vi_i64 id, vi_i64 tag) { return store_set_meta(db->s, (i64)id, (i64)tag) >= 0 ? 0 : -1; }

VI_API int vi_get_tag(vi_db *db, vi_i64 id, vi_i64 *tag)
{
    i64 m = 0;
    if (store_get_meta(db->s, (i64)id, &m) != 0) return -1;
    if (tag) *tag = (vi_i64)m;
    return 0;
}

VI_API int vi_search(vi_db *db, const float *query, int k, vi_i64 *ids, float *scores)
{
    return k > 0 ? store_search(db->s, query, (i32)k, (i64 *)ids, scores) : 0;
}

VI_API int vi_search_tagged(vi_db *db, const float *query, int k, vi_i64 all, vi_i64 any, vi_i64 *ids, float *scores)
{
    return k > 0 ? store_search_filter(db->s, query, (i32)k, (i64 *)ids, scores, (i64)all, (i64)any) : 0;
}

VI_API void vi_search_batch(vi_db *db, const float *queries, int nq, int k, vi_i64 *ids, float *scores, int threads)
{
    if (nq > 0 && k > 0) store_search_batch(db->s, queries, (i32)nq, (i32)k, (i64 *)ids, scores, (i32)threads);
}

VI_API void vi_build(vi_db *db) { store_build(db->s); }

VI_API void vi_set_recall(vi_db *db, float target) { store_set_target_recall(db->s, target); }

VI_API int vi_set_layout(vi_db *db, int code_bits, int row_bytes)
{
    if ((code_bits != 4 && code_bits != 2 && code_bits != 1) || (row_bytes != 4 && row_bytes != 1) || store_count(db->s) > 0) return -1;
    if (store_set_vec_bytes(db->s, (i32)row_bytes) != 0) return -1;
    store_set_codes(db->s, (i32)code_bits);
    return 0;
}

VI_API int vi_set_cascade(vi_db *db, int on) { return store_set_cascade(db->s, (i32)on); }

VI_API int vi_save(vi_db *db, const char *path)
{
    const char *p = path ? path : db->path;
    return p ? store_save(db->s, p) : -1;
}

VI_API int vi_sync(vi_db *db) { return db->path ? store_sync(db->s, db->path) : -1; }

VI_API int vi_snapshot(vi_db *db, const char *path)
{
    const char *p = path ? path : db->path;
    return p ? store_snapshot(db->s, p) : -1;
}

VI_API int vi_snapshot_wait(vi_db *db, int block) { return store_snapshot_poll(db->s, block); }
