/* api_test.c - the C API (include/vecindex.h) through the library, as a program in another language would use it:
 * nothing but the header and the exported functions. ./run builds it against libvecindex.so and libvecindex.a. */
#include "vecindex.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

#define DIM 32
#define N 3000

static int fails;

static void check(int cond, const char *what)
{
    printf(cond ? "  ok   %s\n" : "  FAIL %s\n", what);
    if (!cond) ++fails;
}

static unsigned rng_(unsigned *s) { *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5; return *s; }

static void vec_(int i, float *v)
{
    unsigned s = (unsigned)i * 2654435761u + 99u;
    int d;
    for (d = 0; d < DIM; ++d) v[d] = (float)(rng_(&s) % 2001u) / 1000.0f - 1.0f;
}

/* how many of the sampled rows come back first for their own vector */
static int self_found_(vi_db *db, int from, int to, int step, int skip_mod)
{
    float v[DIM], sc[1];
    vi_i64 id[1];
    int i, hit = 0;
    for (i = from; i < to; i += step) {
        if (skip_mod && i % skip_mod == 0) continue;
        vec_(i, v);
        if (vi_search(db, v, 1, id, sc) == 1 && id[0] == 1000 + i) ++hit;
    }
    return hit;
}

int main(void)
{
    char path[64], line[200];
    float v[DIM], w[DIM], *qs, *bs, sc[10];
    vi_i64 ids[N], got[10], *bi, tag = 0;
    vi_db *db;
    int i, n, same, want, inner;
    FILE *f;

    sprintf(path, "/tmp/vecindex_api_%d.vxi", (int)getpid());
    printf("C API (vecindex %s)\n", vi_version());
    check(strlen(vi_version()) > 0, "vi_version");

    db = vi_open(path, DIM, 0);
    check(db != (vi_db *)0 && vi_dim(db) == DIM, "vi_open creates a new file");
    if (!db) return 1;
    check(access(path, F_OK) == 0, "the file exists right after vi_open");
    qs = (float *)malloc((size_t)N * DIM * sizeof(float));
    for (i = 0; i < N; ++i) { ids[i] = 1000 + i; vec_(i, qs + (size_t)i * DIM); }
    check(vi_add_batch(db, N, ids, qs) == N && vi_count(db) == N, "vi_add_batch: 3000 rows");
    n = self_found_(db, 0, N, 7, 0);
    sprintf(line, "vi_search before any build (exact scan of the new rows): %d/%d", n, (N + 6) / 7);
    check(n == (N + 6) / 7, line);
    vi_build(db);
    n = self_found_(db, 0, N, 7, 0);
    sprintf(line, "vi_search after vi_build: %d/%d", n, (N + 6) / 7);
    check(n * 100 >= ((N + 6) / 7) * 99, line);

    {   volatile float z = 0.0f; v[0] = z / z; }
    for (i = 1; i < DIM; ++i) v[i] = 1.0f;
    check(vi_add(db, 77, v) == -1 && vi_count(db) == N, "vi_add refuses a NaN");
    for (i = 0; i < N; i += 10) vi_delete(db, 1000 + i);
    check(vi_count(db) == N - N / 10, "vi_delete: 300 rows gone");
    vec_(20, v);
    n = vi_search(db, v, 10, got, sc);
    for (i = 0, same = 0; i < n; ++i) if (got[i] == 1020) same = 1;
    check(n == 10 && !same, "a deleted row is not found");

    vec_(7, v);
    check(vi_add(db, 1006, v) == 0 && vi_count(db) == N - N / 10, "vi_add of an existing id replaces its row");
    n = vi_search(db, v, 2, got, sc);
    check(n == 2 && got[0] + got[1] == 1006 + 1007 && got[0] != got[1] && sc[1] > -1e-4f, "the replaced row has the new vector, once");
    vec_(6, v);
    vi_add(db, 1006, v);                                          /* back as it was */
    for (i = 0; i < 20; ++i) { vec_(5000 + i, v); vi_add(db, ((vi_i64)7 << 20) | i, v); }
    check(vi_delete_range(db, (vi_i64)7 << 20, ((vi_i64)8 << 20) - 1) == 20 && vi_count(db) == N - N / 10,
          "vi_delete_range: all 20 chunks of document 7");

    vec_(5, v);
    check(vi_get(db, 1005, w) == 0, "vi_get");
    {   float nn = 0.0f, err = 0.0f;
        for (i = 0; i < DIM; ++i) nn += v[i] * v[i];
        nn = (float)sqrt((double)nn);
        for (i = 0; i < DIM; ++i) err += (float)fabs((double)(w[i] - v[i] / nn));
        check(err < 1e-4f, "vi_get returns the row, unit-normalised");
    }
    check(vi_get(db, 999999, w) == -1, "vi_get of an unknown id: -1");

    for (i = 1; i < N; i += 3) vi_set_tag(db, 1000 + i, 5);
    check(vi_get_tag(db, 1004, &tag) == 0 && tag == 5, "vi_set_tag / vi_get_tag");
    vec_(9, v);
    n = vi_search_tagged(db, v, 10, 4, 0, got, sc);
    for (i = 0, same = 1; i < n; ++i) if ((got[i] - 1000 - 1) % 3 != 0) same = 0;
    check(n == 10 && same, "vi_search_tagged: only rows whose tag has bit 4");

    bi = (vi_i64 *)malloc((size_t)100 * 10 * sizeof(vi_i64));
    bs = (float *)malloc((size_t)100 * 10 * sizeof(float));
    vi_search_batch(db, qs, 100, 10, bi, bs, 0);
    for (i = 0, same = 0; i < 100; ++i) {
        int k, eq = 1;
        n = vi_search(db, qs + (size_t)i * DIM, 10, got, sc);
        for (k = 0; k < n; ++k) if (bi[i * 10 + k] != got[k]) eq = 0;
        same += eq;
    }
    sprintf(line, "vi_search_batch == vi_search on %d/100 queries", same);
    check(same == 100, line);

    check(vi_snapshot(db, (const char *)0) == 0, "vi_snapshot starts a background snapshot of the store's file");
    for (i = 0; i < 200; ++i) { vec_(N + i, v); vi_add(db, 1000 + N + i, v); }
    check(vi_snapshot_wait(db, 1) == 0, "vi_snapshot_wait: written");
    for (i = 200; i < 300; ++i) { vec_(N + i, v); vi_add(db, 1000 + N + i, v); }
    vec_(9, v);
    vi_add(db, 1008, v);                                          /* replaced after the snapshot: from the journal */
    for (i = 0; i < 5; ++i) { vec_(6000 + i, v); vi_add(db, ((vi_i64)9 << 20) | i, v); }
    vi_delete_range(db, (vi_i64)9 << 20, ((vi_i64)9 << 20) | 3);  /* 4 of the 5 go, also from the journal */
    want = (int)vi_count(db);
    check(vi_sync(db) == 0, "vi_sync");
    vi_close(db);

    db = vi_open(path, 0, 0);
    check(db != (vi_db *)0, "vi_open of the existing file (dim 0: it must exist)");
    if (!db) return 1;
    sprintf(line, "reopened: %d rows of %d (the snapshot plus its journal)", (int)vi_count(db), want);
    check(vi_count(db) == want, line);
    n = self_found_(db, 0, N + 300, 11, 10);
    inner = 0;
    for (i = 0; i < N + 300; i += 11) if (i % 10) ++inner;
    sprintf(line, "every row found again: %d/%d", n, inner);
    check(n * 100 >= inner * 99, line);
    vec_(9, v);
    n = vi_search(db, v, 2, got, sc);
    check(n == 2 && got[0] + got[1] == 1008 + 1009 && got[0] != got[1], "a replace replayed from the journal");
    vec_(6004, v);
    n = vi_search(db, v, 1, got, sc);
    check(n == 1 && got[0] == (((vi_i64)9 << 20) | 4), "a range delete replayed from the journal");
    check(vi_save(db, (const char *)0) == 0, "vi_save to its own file");
    vi_close(db);

    f = fopen(path, "wb");
    if (f) { fputs("not a vecindex file", f); fclose(f); }
    check(vi_open(path, DIM, 0) == (vi_db *)0, "a file that cannot be read is an error, not replaced");
    remove(path);
    sprintf(line, "%s.j", path); remove(line);
    sprintf(line, "%s.jn", path); remove(line);

    db = vi_open((const char *)0, 16, VI_LIVE);
    check(db != (vi_db *)0 && vi_set_layout(db, 2, 1) == 0, "an in-memory store, live, 2-bit codes + int8 rows");
    for (i = 0; i < 500; ++i) { vec_(i, v); vi_add(db, 1000 + i, v); }
    check(vi_set_layout(db, 4, 4) == -1, "vi_set_layout once there are rows: -1");
    check(vi_save(db, (const char *)0) == -1 && vi_sync(db) == -1, "vi_save / vi_sync without a file: -1");
    vi_close(db);
    check(vi_open((const char *)0, 0, 0) == (vi_db *)0, "vi_open of nothing with dim 0: NULL");

    free(qs); free(bi); free(bs);
    printf("C API: %s\n", fails ? "FAILED" : "OK");
    return fails ? 1 : 0;
}
