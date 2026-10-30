/* store_test.c - the self-test of src/store.c; ./run builds and runs it. */
#include "store.c"

static u32 sd_rng_(u32 *s) { *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5; return *s; }
static f32 sd_f_(u32 *s) { return (f32)sd_rng_(s) / (f32)0xffffffffu - 0.5f; }

/* rows [from, to) of x into s from a thread of its own; `done` tells the searching thread how far it got */
typedef struct { RagStore *s; const f32 *x; i32 dim, from, to; i64 id0; i32 done; } SdAdder;
static void *sd_adder_(void *p)
{
    SdAdder *a = (SdAdder *)p;
    i32 i;
    for (i = a->from; i < a->to; ++i) {
        store_add(a->s, a->x + (size_t)i * a->dim, a->id0 + i);
        __atomic_store_n(&a->done, i + 1, __ATOMIC_RELEASE);
        if (i % 1000 == 999) { struct timespec ts; ts.tv_sec = 0; ts.tv_nsec = 1000000L; nanosleep(&ts, (struct timespec *)0); }
    }
    return (void *)0;
}

/* recall@10 of store s on queries Q against the exact top 10 of rows [0, n) of x (ids id0 + row; gone[r]: deleted) */
static f32 sd_recall_(RagStore *s, const f32 *x, i32 n, i32 dim, const f32 *Q, i32 nq, i64 id0, const u8 *gone)
{
    i32 q, r, j, hits = 0;
    for (q = 0; q < nq; ++q) {
        const f32 *qv = Q + (size_t)q * dim;
        f32 bs[10]; i32 br[10];
        i64 got[10]; f32 gs[10];
        i32 h;
        for (j = 0; j < 10; ++j) { bs[j] = -1e30f; br[j] = -1; }
        for (r = 0; r < n; ++r) if (!gone || !gone[r]) store_topk_put_(bs, br, 10, vs_dot(qv, x + (size_t)r * dim, dim), r);
        h = store_search(s, qv, 10, got, gs);
        for (j = 0; j < h; ++j) { i32 z; for (z = 0; z < 10; ++z) if (br[z] >= 0 && id0 + br[z] == got[j]) { ++hits; break; } }
    }
    return (f32)hits / (f32)(nq * 10);
}

int main(void)
{
    static Arena arena;
    Arena *a = &arena;
    i32 dim = 128, n = 4000, per_topic, i, ok = 1, nprobed;
    u32 st = 20260901u;
    f32 *centers, *vv;
    i64 *dids;
    RagStore *s, *s2;
    i64 res[16]; f32 sc[16];
    i32 self_hit = 0, del_leak = 0, bad_rej;

    per_topic = n / 40;
    centers = (f32 *)arena_alloc(a, (size_t)40 * dim * sizeof(f32));
    vv = (f32 *)arena_alloc(a, (size_t)n * dim * sizeof(f32));
    dids = (i64 *)arena_alloc(a, (size_t)n * sizeof(i64));
    for (i = 0; i < 40 * dim; ++i) centers[i] = sd_f_(&st) * 6.0f;
    for (i = 0; i < n; ++i) {
        i32 t = (i / per_topic) % 40, d;
        for (d = 0; d < dim; ++d)
            vv[(size_t)i * dim + d] = centers[t * dim + d] + sd_f_(&st) * 0.3f;
        dids[i] = 1000 + i;
    }

    s = store_new(a, dim);
    /* add with a metadata tag = topic bit */
    for (i = 0; i < n - 200; ++i)
        store_add_meta(s, vv + (size_t)i * dim, dids[i],
                       (i64)1 << ((i / per_topic) % 40));
    store_build(s);
    for (i = n - 200; i < n; ++i)
        store_add_meta(s, vv + (size_t)i * dim, dids[i],
                       (i64)1 << ((i / per_topic) % 40));

    /* NaN rejection */
    { f32 *bad = (f32 *)arena_alloc(a, (size_t)dim * sizeof(f32));
      union { f32 f; u32 u; } q; q.u = 0x7fc00000u;   /* qNaN */
      for (i = 0; i < dim; ++i) bad[i] = 0.1f;
      bad[7] = q.f;
      bad_rej = store_add(s, bad, 999999); }
    print_cstr("store  dim="); print_i64(dim);
    print_cstr("  live="); print_i64(store_count(s));
    print_cstr("  pending="); print_i64(store_pending(s));
    print_cstr("  NaN-add rejected="); print_i64(bad_rej == -1); print_cstr("\n");
    if (bad_rej != -1) ok = 0;

    nprobed = 0;
    for (i = 0; i < n; i += 3) {
        i32 h = store_search(s, vv + (size_t)i * dim, 1, res, sc);
        ++nprobed;
        if (h > 0 && res[0] == dids[i]) ++self_hit;
    }
    print_cstr("  self-retrieval@1 = "); print_i64((i64)(100L * self_hit / nprobed));
    print_cstr("%\n");
    if (self_hit * 100 / nprobed < 90) ok = 0;

    /* metadata filter: query a topic-0 vector but restrict to topic-3.
     * Every hit must carry topic-3's bit (the fallback scan guarantees
     * we still get k of them even though topic-0's IVFPQ neighbourhood
     * has none). */
    {
        i64 fr[10]; f32 fs[10];
        i32 h = store_search_filter(s, vv + 0 * dim, 10, fr, fs,
                                    (i64)1 << 3, 0), j, wrong = 0;
        for (j = 0; j < h; ++j) {
            i32 row;
            for (row = 0; row < s->count; ++row)
                if (STORE_ID(s, row) == fr[j] && !STORE_DEAD(s, row)) {
                    if (!(STORE_META(s, row) & ((i64)1 << 3))) ++wrong;
                    break;
                }
        }
        print_cstr("  filter(topic=3 from a topic-0 query): "); print_i64(h);
        print_cstr(" hits, wrong-tag="); print_i64(wrong); print_cstr("\n");
        if (wrong != 0 || h < 10) ok = 0;
    }

    for (i = 0; i < 300; ++i) store_delete(s, dids[i * 5]);
    for (i = 0; i < 300; ++i) {
        i32 h = store_search(s, vv + (size_t)(i * 5) * dim, 5, res, sc), j;
        for (j = 0; j < h; ++j) if (res[j] == dids[i * 5]) ++del_leak;
    }
    print_cstr("  after 300 deletes: live="); print_i64(store_count(s));
    print_cstr("  leaked="); print_i64(del_leak); print_cstr("\n");
    if (del_leak != 0) ok = 0;
    store_build(s);

    /* save / load - index is serialised, load must NOT rebuild */
    {
        f64 t0;
        RagStore *ld;
        i32 same = 0, tried = 0;
        if (store_save(s, "/tmp/store_demo.vxi") != 0) { print_cstr("  save FAIL\n"); ok = 0; }
        t0 = 0.0; (void)t0;
        s2 = store_load(a, "/tmp/store_demo.vxi");
        ld = s2;
        if (!ld) { print_cstr("  load FAIL\n"); ok = 0; }
        else {
            for (i = 0; i < s->count; i += 7) {
                i64 r1[3], r2[3]; f32 c1[3], c2[3];
                i32 h1 = store_search(s,  STORE_VEC(s, i), 3, r1, c1);
                i32 h2 = store_search(ld, STORE_VEC(s, i), 3, r2, c2);
                ++tried;
                if (h1 == h2 && h1 > 0 && r1[0] == r2[0]) ++same;
            }
            print_cstr("  save/load (index serialised): top-1 identical on ");
            print_i64((i64)(100L * same / tried)); print_cstr("%\n");
            if (same * 100 / tried < 98) ok = 0;
            store_destroy(ld);
        }
    }

    store_destroy(s);

    /* fast build (no auto-tuner): must retrieve and round-trip through .vxi too */
    {
        RagStore *f = store_new(a, dim), *fl;
        i32 hit = 0, tried = 0, same = 0;
        n = 6000;                                       /* above the flat-q5 shortcut */
        vv = (f32 *)arena_alloc(a, (size_t)n * dim * sizeof(f32));
        for (i = 0; i < n; ++i) {
            i32 t = (i / 150) % 40, d;
            for (d = 0; d < dim; ++d) vv[(size_t)i * dim + d] = centers[t * dim + d] + sd_f_(&st) * 0.3f;
        }
        store_set_fast(f, 0.9f);
        for (i = 0; i < n; ++i) store_add(f, vv + (size_t)i * dim, 5000 + i);
        store_build(f);
        for (i = 0; i < n; i += 5) {
            i32 h = store_search(f, vv + (size_t)i * dim, 1, res, sc);
            ++tried; if (h > 0 && res[0] == 5000 + i) ++hit;
        }
        print_cstr("  fast build: self-retrieval@1 = "); print_i64((i64)(100L * hit / tried)); print_cstr("%\n");
        if (hit * 100 / tried < 90) ok = 0;
        if (store_save(f, "/tmp/store_demo_fast.vxi") != 0) ok = 0;
        fl = store_load(a, "/tmp/store_demo_fast.vxi");
        if (!fl) ok = 0;
        else {
            tried = 0;
            for (i = 0; i < n; i += 11) {
                i64 r1[1], r2[1]; f32 c1[1], c2[1];
                i32 h1 = store_search(f, vv + (size_t)i * dim, 1, r1, c1), h2 = store_search(fl, vv + (size_t)i * dim, 1, r2, c2);
                ++tried; if (h1 == h2 && h1 > 0 && r1[0] == r2[0]) ++same;
            }
            print_cstr("  fast build: save/load top-1 identical on "); print_i64((i64)(100L * same / tried)); print_cstr("%\n");
            if (same * 100 / tried < 98) ok = 0;
            store_destroy(fl);
        }
        store_destroy(f);
    }

    /* IVF grouping of the TurboQuant scan (normally only for huge stores; exercised here on 6000 rows directly) */
    {
        Tq *flat, *ivf;
        i32 nq = 60, q, j, hit = 0, tot = 0, filt_ok = 1, all_ok = 1;
        u64 *dead = (u64 *)calloc((size_t)(n + 63) / 64, sizeof(u64));
        i64 *meta = (i64 *)calloc((size_t)n, sizeof(i64));
        u8 *hv, *hm;
        TqCol vcol, mcol;
        for (i = 0; i < n; ++i) { meta[i] = (i % 3 == 0) ? 1 : 2; if (i % 11 == 0) dead[i >> 6] |= (u64)1 << (i & 63); }
        mcol = tq_col_flat(meta, sizeof(i64), &hm);
        {   f32 *un = (f32 *)malloc((size_t)n * dim * sizeof(f32));                /* unit rows, as the store keeps them */
            for (i = 0; i < n; ++i) { f32 nr = 0; for (j = 0; j < dim; ++j) nr += vv[(size_t)i * dim + j] * vv[(size_t)i * dim + j]; nr = 1.0f / (f32)sqrt((f64)nr); for (j = 0; j < dim; ++j) un[(size_t)i * dim + j] = vv[(size_t)i * dim + j] * nr; }
            vcol = tq_col_flat(un, (size_t)dim * sizeof(f32), &hv);
            flat = tq_build(&vcol, n, dim, 7u, 0, 2, 4); ivf = tq_build(&vcol, n, dim, 7u, 60, 2, 4);
            for (q = 0; q < nq; ++q) {
                i32 rf[256], ri[256], rn[256], nf, ni, nn2;
                const f32 *qv = un + (size_t)((q * 97) % n) * dim;
                nf = tq_search(flat, qv, 256, rf, dead, &mcol, 0, 0, 0, 1, 0);
                ni = tq_search(ivf, qv, 256, ri, dead, &mcol, 0, 0, 0, 1, 0);           /* the default probe */
                nn2 = tq_search(ivf, qv, 256, rn, dead, &mcol, 0, 0, 0, 1, 60);         /* every cluster = the same rows as flat */
                for (j = 0; j < 10 && j < nf; ++j) { i32 k, f = 0; for (k = 0; k < ni; ++k) if (ri[k] == rf[j]) { f = 1; break; } hit += f; ++tot; }
                { i32 k, same = nf == nn2; for (k = 0; k < nf && k < 200 && same; ++k) { i32 z, f = 0; for (z = 0; z < nn2; ++z) if (rn[z] == rf[k]) { f = 1; break; } if (!f) same = 0; } if (!same) all_ok = 0; }
                for (j = 0; j < ni; ++j) if ((dead[ri[j] >> 6] >> (ri[j] & 63)) & 1u) all_ok = 0;   /* tombstones never come back */
                ni = tq_search(ivf, qv, 64, ri, dead, &mcol, 1, 0, 1, 1, 60);            /* meta filter: only rows with bit 1 */
                for (j = 0; j < ni; ++j) if (!(meta[ri[j]] & 1)) filt_ok = 0;
            }
            print_cstr("  IVF scan: top-10 of the flat scan found "); print_i64((i64)(100L * hit / tot)); print_cstr("%; all clusters == flat: ");
            print_cstr(all_ok ? "yes" : "NO"); print_cstr("; meta filter respected: "); print_cstr(filt_ok ? "yes" : "NO"); print_cstr("\n");
            if (100L * hit / tot < 88 || !all_ok || !filt_ok) ok = 0;
            tq_free(flat); tq_free(ivf); free(un);
        }
        free(dead); free(meta);
    }

    /* columns spilled to a file from the first chunk: same answers as in memory, ids looked up out of order */
    {
        RagStore *m = store_new(a, dim), *d = store_new(a, dim);
        i32 same = 0, tried = 0, found = 0;
        i64 ram = 0, file = 0;
        store_set_spill(m, (const char *)0, 0);
        store_set_spill(d, "/tmp", 0);
        store_set_fast(m, 0.9f); store_set_fast(d, 0.9f);
        for (i = 0; i < n; ++i) {
            i64 id = (i64)((i * 7919) % n) * 3 + 1;              /* ids out of order: the hash path */
            store_add(m, vv + (size_t)i * dim, id); store_add(d, vv + (size_t)i * dim, id);
        }
        store_build(m); store_build(d);
        for (i = 0; i < n; i += 13) {
            i64 r1[5], r2[5]; f32 c1[5], c2[5];
            i32 h1 = store_search(m, vv + (size_t)i * dim, 5, r1, c1), h2 = store_search(d, vv + (size_t)i * dim, 5, r2, c2), x, eq = h1 == h2;
            for (x = 0; x < h1 && eq; ++x) if (r1[x] != r2[x]) eq = 0;
            ++tried; same += eq;
            if (store_has(d, (i64)((i * 7919) % n) * 3 + 1)) ++found;
        }
        store_mem(d, &ram, &file);
        print_cstr("  spilled columns: results identical on "); print_i64((i64)(100L * same / tried));
        print_cstr("%, ids found "); print_i64(found); print_cstr("/"); print_i64(tried);
        print_cstr(", file-backed bytes "); print_i64(file); print_cstr("\n");
        if (same != tried || found != tried || file < (i64)n * dim * 4) ok = 0;
        store_destroy(m); store_destroy(d);
    }

    /* the compact modes: int8 rows + 2-bit codes. Same neighbours as the f32 / 4-bit store, a v6 file that loads back
     * to identical answers, and the 2-bit IVF scan of every cluster equal to the 2-bit flat scan */
    {
        RagStore *ref = store_new(a, dim), *c8 = store_new(a, dim), *cl;
        i32 tried = 0, agree = 0, same = 0, self = 0, x;
        f32 back[128], cosm = 1.0f;
        store_set_fast(ref, 0.9f); store_set_fast(c8, 0.9f);
        if (store_set_vec_bytes(c8, 1) != 0) ok = 0;
        store_set_codes(c8, 2);
        store_set_rerank(c8, 1000);                                  /* 2-bit ranks coarsely: re-score more */
        for (i = 0; i < n; ++i) { store_add(ref, vv + (size_t)i * dim, 7000 + i); store_add(c8, vv + (size_t)i * dim, 7000 + i); }
        if (store_set_vec_bytes(c8, 4) != -1) ok = 0;               /* not once it has rows */
        store_build(ref); store_build(c8);
        for (i = 0; i < n; i += 7) {   /* a hit counts when its exact score is within the int8 error of the exact 10th best: rows of a topic are near
                                       * ties (cosine ~0.998), so the exact ids are a coin toss; real-data recall is in bench/ */
            i64 r1[10], r2[10]; f32 c1[10], c2[10];
            i32 h1 = store_search(ref, vv + (size_t)i * dim, 10, r1, c1), h2 = store_search(c8, vv + (size_t)i * dim, 10, r2, c2);
            for (x = 0; x < h2 && h1 == 10; ++x) {
                f32 ex[128];
                if (store_get_vec(ref, r2[x], ex) == 0) {
                    f32 d2 = 0.0f, qq = 0.0f; i32 z;
                    for (z = 0; z < dim; ++z) { d2 += ex[z] * vv[(size_t)i * dim + z]; qq += vv[(size_t)i * dim + z] * vv[(size_t)i * dim + z]; }
                    if (2.0f * d2 / (f32)sqrt((f64)qq) - 2.0f >= c1[9] - 0.01f) ++agree;   /* within the int8 error */
                }
            }
            tried += h1;
            if (h2 > 0 && r2[0] == 7000 + i) ++self;
        }
        for (i = 0; i < n; i += 97) {
            f32 dot = 0.0f, nn = 0.0f;
            if (store_get_vec(c8, 7000 + i, back) != 0) { ok = 0; continue; }
            for (x = 0; x < dim; ++x) { dot += back[x] * vv[(size_t)i * dim + x]; nn += vv[(size_t)i * dim + x] * vv[(size_t)i * dim + x]; }
            dot /= (f32)sqrt((f64)nn);
            if (dot < cosm) cosm = dot;
        }
        if (store_vec_ptr(c8, 7000) != (const f32 *)0) ok = 0;      /* no f32 copy to lend */
        if (store_save(c8, "/tmp/store_demo_i8.vxi") != 0) ok = 0;
        cl = store_load(a, "/tmp/store_demo_i8.vxi");
        if (!cl || cl->vbytes != 1 || !cl->tq || cl->tq->bits != 2) ok = 0;
        else for (i = 0; i < n; i += 11) {
            i64 r1[5], r2[5]; f32 c1[5], c2[5];
            i32 h1 = store_search(c8, vv + (size_t)i * dim, 5, r1, c1), h2 = store_search(cl, vv + (size_t)i * dim, 5, r2, c2), eq = h1 == h2;
            for (x = 0; x < h1 && eq; ++x) if (r1[x] != r2[x]) eq = 0;
            same += eq;
        }
        print_cstr("  int8 rows + 2-bit codes: top-10 as close as the exact top-10 on "); print_i64((i64)(100L * agree / (tried ? tried : 1)));
        print_cstr("%, self@1 "); print_i64((i64)(100L * self / ((n + 6) / 7))); print_cstr("%, min cosine of a dequantised row ");
        print_i64((i64)(cosm * 10000.0f)); print_cstr("e-4, v6 save/load identical on "); print_i64((i64)(100L * same / ((n + 10) / 11)));
        print_cstr("%\n");
        if (agree * 100 < tried * 95 || self * 100 < ((n + 6) / 7) * 95 || cosm < 0.998f || same != (n + 10) / 11) ok = 0;
        if (cl) store_destroy(cl);
        store_destroy(ref); store_destroy(c8);
        remove("/tmp/store_demo_i8.vxi"); remove("/tmp/store_demo_i8.vxi.j");
    }
    {   /* 2-bit IVF over every cluster == 2-bit flat */
        f32 *un = (f32 *)malloc((size_t)n * dim * sizeof(f32));
        u8 *hv;
        TqCol vcol;
        Tq *flat, *ivf;
        i32 q, all_ok = 1, j;
        for (i = 0; i < n; ++i) { f32 nr = 0; for (j = 0; j < dim; ++j) nr += vv[(size_t)i * dim + j] * vv[(size_t)i * dim + j]; nr = 1.0f / (f32)sqrt((f64)nr); for (j = 0; j < dim; ++j) un[(size_t)i * dim + j] = vv[(size_t)i * dim + j] * nr; }
        vcol = tq_col_flat(un, (size_t)dim * sizeof(f32), &hv);
        flat = tq_build(&vcol, n, dim, 7u, 0, 2, 2); ivf = tq_build(&vcol, n, dim, 7u, 60, 2, 2);
        for (q = 0; q < 40; ++q) {
            i32 rf[200], rn[200], nf, nn2, k;
            const f32 *qv = un + (size_t)((q * 131) % n) * dim;
            nf = tq_search(flat, qv, 200, rf, (const u64 *)0, (const TqCol *)0, 0, 0, 0, 1, 0);
            nn2 = tq_search(ivf, qv, 200, rn, (const u64 *)0, (const TqCol *)0, 0, 0, 0, 1, 60);
            if (nf != nn2) all_ok = 0;
            for (k = 0; k < nf && k < 150 && all_ok; ++k) { i32 z, f = 0; for (z = 0; z < nn2; ++z) if (rn[z] == rf[k]) { f = 1; break; } if (!f) all_ok = 0; }
        }
        {   /* tq_search_many (cluster-major batch) == tq_search, IVF and flat */
            i32 nq2 = 64, R2 = 100, *mr = (i32 *)malloc((size_t)nq2 * R2 * sizeof(i32)), mc[64], z, same2 = 0, pass;
            for (pass = 0; pass < 2; ++pass) {
                Tq *tt = pass ? flat : ivf;
                tq_search_many(tt, un, nq2, R2, mr, mc, (const u64 *)0, 2, 12);
                for (q = 0; q < nq2; ++q) {
                    i32 one[100], no = tq_search(tt, un + (size_t)q * dim, R2, one, (const u64 *)0, (const TqCol *)0, 0, 0, 0, 1, 12), eq = no == mc[q];
                    for (z = 0; z < no && eq; ++z) if (one[z] != mr[(size_t)q * R2 + z]) eq = 0;
                    same2 += eq;
                }
            }
            print_cstr("  tq_search_many == tq_search on "); print_i64(same2); print_cstr("/128 (IVF and flat, 2-bit)\n");
            if (same2 != 2 * nq2) all_ok = 0;
            free(mr);
        }
        print_cstr("  2-bit IVF, every cluster == 2-bit flat: "); print_cstr(all_ok ? "yes" : "NO"); print_cstr("\n");
        if (!all_ok || flat->vd != dim / 2 || ivf->bits != 2) ok = 0;
        tq_free(flat); tq_free(ivf); free(un);
    }

    {   /* two-level clustering (more than TQ_FLAT_MAX clusters): rows land in their truly closest cluster, every
         * cluster == flat, batch == one at a time */
        i32 n2 = 24000, d2 = 32, nc2 = 1500, j, q, inbest = 0, same = 0, all_ok = 1, *mr, mc[32];
        f32 *x2 = (f32 *)malloc((size_t)n2 * d2 * sizeof(f32)), *sc2;
        u8 *hv;
        u32 s2 = 77u;
        TqCol vcol;
        Tq *flat, *ivf;
        for (i = 0; i < n2; ++i) {
            f32 nr = 0;
            for (j = 0; j < d2; ++j) { f32 v = sd_f_(&s2); x2[(size_t)i * d2 + j] = v; nr += v * v; }
            nr = 1.0f / (f32)sqrt((f64)nr);
            for (j = 0; j < d2; ++j) x2[(size_t)i * d2 + j] *= nr;
        }
        vcol = tq_col_flat(x2, (size_t)d2 * sizeof(f32), &hv);
        flat = tq_build(&vcol, n2, d2, 7u, 0, 2, 4); ivf = tq_build(&vcol, n2, d2, 7u, nc2, 2, 4);
        sc2 = (f32 *)malloc((size_t)(ivf->nc + 15) / 16 * 16 * sizeof(f32));
        for (j = 0; j < ivf->nc; ++j) {                         /* the cluster each row sits in vs its closest centroid */
            i32 sl;
            for (sl = ivf->cstart[j] * 32; sl < ivf->cstart[j + 1] * 32; ++sl) {
                i32 row = ivf->slot_row[sl];
                if (row < 0) continue;
                tq_score_(x2 + (size_t)row * d2, ivf->ct, (ivf->nc + 15) / 16, d2, sc2);
                inbest += tq_argmax_(sc2, ivf->nc) == j;
            }
        }
        for (q = 0; q < 30; ++q) {
            i32 rf[100], rn[100], nf, nn2, k;
            const f32 *qv = x2 + (size_t)(q * 797) * d2;
            nf = tq_search(flat, qv, 100, rf, (const u64 *)0, (const TqCol *)0, 0, 0, 0, 1, 0);
            nn2 = tq_search(ivf, qv, 100, rn, (const u64 *)0, (const TqCol *)0, 0, 0, 0, 1, ivf->nc);
            if (nf != nn2) all_ok = 0;
            for (k = 0; k < nf && all_ok; ++k) if (rf[k] != rn[k]) all_ok = 0;
        }
        mr = (i32 *)malloc((size_t)32 * 100 * sizeof(i32));
        tq_search_many(ivf, x2, 32, 100, mr, mc, (const u64 *)0, 2, 40);
        for (q = 0; q < 32; ++q) {
            i32 one[100], no = tq_search(ivf, x2 + (size_t)q * d2, 100, one, (const u64 *)0, (const TqCol *)0, 0, 0, 0, 1, 40), eq = no == mc[q], z;
            for (z = 0; z < no && eq; ++z) if (one[z] != mr[(size_t)q * 100 + z]) eq = 0;
            same += eq;
        }
        print_cstr("  two-level IVF ("); print_i64(ivf->nc); print_cstr(" clusters): rows in their closest cluster ");
        print_i64((i64)(1000L * inbest / n2)); print_cstr("/1000, every cluster == flat: "); print_cstr(all_ok ? "yes" : "NO");
        print_cstr(", batch == one at a time "); print_i64(same); print_cstr("/32\n");
        if (ivf->nc <= TQ_FLAT_MAX || 1000L * inbest / n2 < 900 || !all_ok || same != 32) ok = 0;
        tq_free(flat); tq_free(ivf); free(x2); free(sc2); free(mr);
    }

    {   /* self-tuning: ask for a recall, check it on queries the tuning never saw */
        i32 n3 = 30000, d3 = 32, j, q, nq3 = 200, hits = 0, p95, p80, *tr3;
        f32 *x3 = (f32 *)malloc((size_t)n3 * d3 * sizeof(f32)), *q3 = (f32 *)malloc((size_t)nq3 * d3 * sizeof(f32)), r95, r80;
        u32 s3 = 991u;
        RagStore *t3 = store_new(a, d3);
        for (i = 0; i < n3; ++i) {                              /* 300 topics, rows scattered around them */
            u32 tp = sd_rng_(&s3) % 300u, s4 = 5000u + tp;
            for (j = 0; j < d3; ++j) x3[(size_t)i * d3 + j] = sd_f_(&s4) * 1.0f + sd_f_(&s3) * 1.5f;
            store_add(t3, x3 + (size_t)i * d3, 70000 + i);
        }
        for (q = 0; q < nq3; ++q) {                             /* held-out queries: near a topic, not a row */
            u32 tp = sd_rng_(&s3) % 300u, s4 = 5000u + tp;
            for (j = 0; j < d3; ++j) q3[(size_t)q * d3 + j] = sd_f_(&s4) * 1.0f + sd_f_(&s3) * 1.5f;
        }
        store_ivf_min_ = 2000;
        store_set_target_recall(t3, 0.95f);
        store_build(t3);
        p95 = t3->tq ? t3->tq->nprobe : -1; r95 = store_tuned_recall(t3);
        tr3 = (i32 *)malloc((size_t)nq3 * 10 * sizeof(i32));
        for (q = 0; q < nq3; ++q) store_normalise_(q3 + (size_t)q * d3, d3);
        { TqCol sc3, c3 = store_vcol_(t3, &sc3); store_truth_(&c3, t3->dead, t3->built_upto, d3, q3, (const i32 *)0, nq3, 10, tr3, 2); }
        for (q = 0; q < nq3; ++q) {
            i64 r10[10]; f32 c10[10];
            i32 got = store_search(t3, q3 + (size_t)q * d3, 10, r10, c10), x, z;
            for (x = 0; x < got; ++x) for (z = 0; z < 10; ++z) if (tr3[(size_t)q * 10 + z] + 70000 == r10[x]) { ++hits; break; }
        }
        store_set_target_recall(t3, 0.80f);
        r80 = store_tune(t3);                                   /* now on the 200 queries it has just answered */
        p80 = t3->tq ? t3->tq->nprobe : -1;
        print_cstr("  self-tuning ("); print_i64(t3->tq ? t3->tq->nc : 0); print_cstr(" clusters): target 0.95 -> ");
        print_i64(p95); print_cstr(" probes (measured "); print_i64((i64)(1000.0f * r95)); print_cstr("/1000), held-out recall@10 ");
        print_i64((i64)(1000L * hits / (nq3 * 10))); print_cstr("/1000; target 0.80 on its own traffic -> "); print_i64(p80);
        print_cstr(" probes ("); print_i64((i64)(1000.0f * r80)); print_cstr("/1000)\n");
        if (!t3->tq || t3->tq->nc <= 1 || r95 < 0.95f || 1000L * hits / (nq3 * 10) < 920 || p80 > p95 || r80 < 0.80f || p95 >= t3->tq->nc) ok = 0;
        store_ivf_min_ = STORE_IVF_MIN;
        store_destroy(t3); free(x3); free(q3); free(tr3);
    }

    /* batch search: the same answers as one query at a time (flat scan here; IVF + batch in the 2-bit test above) */
    {
        RagStore *b = store_new(a, dim);
        i32 nq = 300, same = 0, x;
        i64 *bi = (i64 *)malloc((size_t)nq * 10 * sizeof(i64)), r1[10];
        f32 *bs = (f32 *)malloc((size_t)nq * 10 * sizeof(f32)), c1[10];
        store_set_fast(b, 0.9f);
        for (i = 0; i < n; ++i) store_add(b, vv + (size_t)i * dim, 300000 + i);
        store_build(b);
        for (i = 0; i < 40; ++i) store_add(b, vv + (size_t)(i * 13) * dim, 900000 + i);   /* a pending tail too */
        store_delete(b, 300000 + 5);
        store_search_batch(b, vv, nq, 10, bi, bs, 2);
        for (i = 0; i < nq; ++i) {
            i32 h = store_search(b, vv + (size_t)i * dim, 10, r1, c1), eq = 1;
            for (x = 0; x < 10; ++x) if ((x < h ? r1[x] : -1) != bi[(size_t)i * 10 + x]) eq = 0;
            same += eq;
        }
        print_cstr("  batch search == one at a time on "); print_i64(same); print_cstr("/"); print_i64(nq); print_cstr(" queries\n");
        if (same != nq) ok = 0;
        free(bi); free(bs);
        store_destroy(b);
    }

    {   /* Live ingest. (1) rows added to a built index are rows like the built ones: probing every cluster gives the
         * flat scan of all the rows, the batch path agrees, a flat index takes added rows the same way, and an added row
         * lands in its closest cluster. (2) a live store: rows streamed in by another thread while this one searches -
         * indexed by the store's own thread, rebuilt as it grows, tuned to the recall asked for; deletes + save keep
         * the index (nothing rebuilt at load); rows from new topics are noticed as a drift and trigger a rebuild. */
        i32 n4 = 24000, n40 = 16000, d4 = 32, nd = 3000, nq4 = 100, j, q, bits, same = 0, tot = 0, inbest = 0, sameb = 0;
        i32 miss = 0, looks = 0, sameld = 0, mc[32], did = 0, reasons = 0, nsnap = 0, snap_at = 0;
        i32 *mr = (i32 *)malloc((size_t)32 * 100 * sizeof(i32));
        f32 *x4 = (f32 *)malloc((size_t)(n4 + nd) * d4 * sizeof(f32)), *q4 = (f32 *)malloc((size_t)(2 * nq4) * d4 * sizeof(f32));
        f32 *qd = q4 + (size_t)nq4 * d4, sc4[64], r_live, r_saved, r_d0, r_d1, rt;
        u8 *hv, *gone = (u8 *)calloc((size_t)(n4 + nd), 1);
        u32 s5 = 4242u;
        TqCol vcol;
        RagStore *lv, *ld;
        SdAdder ad4;
        pthread_t th;
        i64 nidx = 0, nreb = 0, nreb0 = 0;
        f64 rbs = 0.0;
        for (i = 0; i < n4 + nd + 2 * nq4; ++i) {               /* 200 topics; the drift rows and queries: 30 new ones */
            i32 newt = (i >= n4 && i < n4 + nd) || i >= n4 + nd + nq4;
            u32 tp = newt ? 1000u + sd_rng_(&s5) % 30u : sd_rng_(&s5) % 200u, s6 = 300u + tp;
            f32 *v = i < n4 + nd ? x4 + (size_t)i * d4 : q4 + (size_t)(i - n4 - nd) * d4, nr = 0.0f;
            for (j = 0; j < d4; ++j) { v[j] = sd_f_(&s6) + sd_f_(&s5) * 0.8f; nr += v[j] * v[j]; }
            nr = 1.0f / (f32)sqrt((f64)nr);
            for (j = 0; j < d4; ++j) v[j] *= nr;
        }
        vcol = tq_col_flat(x4, (size_t)d4 * sizeof(f32), &hv);
        for (bits = 4; bits >= 2; bits -= 2) {
            Tq *all = tq_build(&vcol, n4, d4, 7u, 0, 2, bits), *ivf = tq_build(&vcol, n40, d4, 7u, 60, 2, bits);
            Tq *fl = tq_build(&vcol, n40 + 7, d4, 7u, 0, 2, bits);
            TqAdd ta;
            if (tq_add_prep(ivf, &vcol, n40, n4 - n40, 2, &ta) != 0 || tq_add_put(ivf, &ta, n40) != 0) ok = 0;
            if (bits == 4) for (j = 0; j < ta.m; ++j) {          /* the closest centroid, by exact f32 scores */
                tq_score_(x4 + (size_t)(n40 + j) * d4, ivf->ct, (ivf->nc + 15) / 16, d4, sc4);
                inbest += tq_argmax_(sc4, ivf->nc) == ta.cl[j];
            }
            tq_add_free(&ta);
            if (tq_add_prep(fl, &vcol, n40 + 7, n4 - n40 - 7, 2, &ta) != 0 || tq_add_put(fl, &ta, n40 + 7) != 0) ok = 0;
            tq_add_free(&ta);
            for (q = 0; q < 30; ++q) {
                i32 ra[100], ri[100], rf[100], na, ni, nf, k, eq;
                const f32 *qv = x4 + (size_t)(q * 797) * d4;
                na = tq_search(all, qv, 100, ra, (const u64 *)0, (const TqCol *)0, 0, 0, 0, 1, 0);
                ni = tq_search(ivf, qv, 100, ri, (const u64 *)0, (const TqCol *)0, 0, 0, 0, 1, ivf->nc);
                nf = tq_search(fl, qv, 100, rf, (const u64 *)0, (const TqCol *)0, 0, 0, 0, 2, 0);
                eq = na == ni && na == nf;
                for (k = 0; k < na && eq; ++k) if (ra[k] != ri[k] || ra[k] != rf[k]) eq = 0;
                same += eq; ++tot;
            }
            tq_search_many(ivf, x4 + (size_t)n40 * d4, 32, 100, mr, mc, (const u64 *)0, 2, 12);   /* queries: added rows */
            for (q = 0; q < 32; ++q) {
                i32 one[100], no = tq_search(ivf, x4 + (size_t)(n40 + q) * d4, 100, one, (const u64 *)0, (const TqCol *)0, 0, 0, 0, 1, 12), eq = no == mc[q], z;
                for (z = 0; z < no && eq; ++z) if (one[z] != mr[(size_t)q * 100 + z]) eq = 0;
                sameb += eq;
            }
            if (ivf->xn == 0 || fl->xn == 0 || ivf->n != n4 || fl->n != n4) ok = 0;
            tq_free(all); tq_free(ivf); tq_free(fl);
        }
        print_cstr("  live: added rows == built rows (all clusters of built + added == flat of all, flat + added == flat of all) ");
        print_i64(same); print_cstr("/"); print_i64(tot); print_cstr(", batch == one at a time "); print_i64(sameb);
        print_cstr("/64, added rows in their closest cluster "); print_i64((i64)(1000L * inbest / (n4 - n40))); print_cstr("/1000\n");
        if (same != tot || sameb != 64 || 1000L * inbest / (n4 - n40) < 990) ok = 0;

        store_ivf_min_ = 2000;                                   /* IVF from 2000 rows, so a small store has clusters */
        lv = store_new(a, d4);
        store_set_target_recall(lv, 0.95f);
        store_set_live(lv, 1, 1.0f);
        ad4.s = lv; ad4.x = x4; ad4.dim = d4; ad4.from = 0; ad4.to = n4; ad4.id0 = 600000; ad4.done = 0;
        pthread_create(&th, (const pthread_attr_t *)0, sd_adder_, &ad4);
        while ((j = __atomic_load_n(&ad4.done, __ATOMIC_ACQUIRE)) < n4) {   /* every row added so far is found */
            i64 r1[1]; f32 c1[1];
            i32 row;
            if (j == 0) continue;
            row = (i32)(sd_rng_(&s5) % (u32)j);
            if (store_search(lv, x4 + (size_t)row * d4, 1, r1, c1) < 1 || r1[0] != 600000 + row) ++miss;
            ++looks;
            if (j >= snap_at) { if (store_snapshot(lv, "/tmp/store_demo_live_snap.vxi") == 0) ++nsnap; snap_at += n4 / 6; }   /* forks beside the
                                                                  * live thread and the adder */
        }
        pthread_join(th, (void **)0);
        while (looks < 100) {                    /* a slow machine (an emulator) can add every row before 100 lookups */
            i64 r1[1]; f32 c1[1];
            i32 row = (i32)(sd_rng_(&s5) % (u32)n4);
            if (store_search(lv, x4 + (size_t)row * d4, 1, r1, c1) < 1 || r1[0] != 600000 + row) ++miss;
            ++looks;
        }
        if (store_snapshot_poll(lv, 1) != 0) ok = 0;
        {   RagStore *ls = store_load(a, "/tmp/store_demo_live_snap.vxi");
            i32 have = ls ? store_count(ls) : 0, fnd = 0, tr = 0;
            for (j = 0; ls && j < have; j += 97) {
                i64 r1[1]; f32 c1[1];
                ++tr;
                if (store_search(ls, x4 + (size_t)j * d4, 1, r1, c1) == 1 && r1[0] == 600000 + j) ++fnd;
            }
            print_cstr("  live store: "); print_i64(nsnap); print_cstr(" snapshots taken while rows streamed in, the last one loads with ");
            print_i64(have); print_cstr(" rows, found "); print_i64(fnd); print_cstr("/"); print_i64(tr); print_cstr("\n");
            if (!ls || nsnap < 1 || have < 1 || have > n4 || fnd * 100 < tr * 98) ok = 0;
            if (ls) store_destroy(ls);
            remove("/tmp/store_demo_live_snap.vxi"); remove("/tmp/store_demo_live_snap.vxi.j"); remove("/tmp/store_demo_live_snap.vxi.jn");
        }
        for (j = 0; j < 20000 && store_pending(lv) > 0; ++j) { struct timespec ts; ts.tv_sec = 0; ts.tv_nsec = 1000000L; nanosleep(&ts, (struct timespec *)0); }
        store_set_live(lv, 0, 1.0f);                             /* stop the thread; settle by hand */
        for (j = 0; j < 20 && store_maintain(lv); ++j) {}
        store_live_stats(lv, &nidx, &nreb, &rbs);
        rt = store_tuned_recall(lv);
        r_live = sd_recall_(lv, x4, n4, d4, q4, nq4, 600000, (const u8 *)0);
        print_cstr("  live store: "); print_i64(n4); print_cstr(" rows streamed in while searching - added rows found "); print_i64(looks - miss);
        print_cstr("/"); print_i64(looks); print_cstr(", "); print_i64(nidx); print_cstr(" indexed as they came, "); print_i64(nreb);
        print_cstr(" rebuilds, "); print_i64(lv->tq ? lv->tq->nc : 0); print_cstr(" clusters, tuned to "); print_i64((i64)(1000.0f * rt));
        print_cstr("/1000, held-out recall@10 "); print_i64((i64)(1000.0f * r_live)); print_cstr("/1000\n");
        if (miss || looks < 100 || store_pending(lv) != 0 || !lv->tq || lv->tq->nc <= 1 || nreb < 1 || rt < 0.95f || r_live < 0.92f) ok = 0;

        for (i = 0; i < n4; i += 12) { store_delete(lv, 600000 + i); gone[i] = 1; }
        r_live = sd_recall_(lv, x4, n4, d4, q4, nq4, 600000, gone);
        if (store_save(lv, "/tmp/store_demo_live.vxi") != 0) ok = 0;
        r_saved = sd_recall_(lv, x4, n4, d4, q4, nq4, 600000, gone);   /* compacted: the index followed its rows */
        ld = store_load(a, "/tmp/store_demo_live.vxi");
        if (!ld || !ld->tq || ld->tq->xn != 0 || ld->tq->n != store_count(ld) || store_pending(ld) != 0 || ld->tq->nprobe != lv->tq->nprobe) ok = 0;
        else for (q = 0; q < nq4; ++q) {
            i64 r1[10], r2[10]; f32 c1[10], c2[10];
            i32 h1 = store_search(lv, q4 + (size_t)q * d4, 10, r1, c1), h2 = store_search(ld, q4 + (size_t)q * d4, 10, r2, c2), eq = h1 == h2, z;
            for (z = 0; z < h1 && eq; ++z) if (r1[z] != r2[z]) eq = 0;
            for (z = 0; z < h2; ++z) if (r2[z] >= 600000 && r2[z] < 600000 + n4 && gone[r2[z] - 600000]) eq = 0;
            sameld += eq;
        }
        print_cstr("  deletes + save: recall@10 "); print_i64((i64)(1000.0f * r_live)); print_cstr(" -> "); print_i64((i64)(1000.0f * r_saved));
        print_cstr(" after the compaction, loaded without a rebuild, same top-10 on "); print_i64(sameld); print_cstr("/"); print_i64(nq4); print_cstr("\n");
        if (r_saved != r_live || sameld != nq4) ok = 0;

        if (ld) {
            store_live_stats(ld, (i64 *)0, &nreb0, (f64 *)0);
            store_set_live(ld, 0, 0.0f);                         /* no rebuilds: just index the new rows */
            for (i = n4; i < n4 + nd; ++i) store_add(ld, x4 + (size_t)i * d4, 600000 + i);
            store_maintain(ld);
            store_set_probe(ld, 8);
            r_d0 = sd_recall_(ld, x4, n4 + nd, d4, qd, nq4, 600000, gone);
            reasons = ld->drifted;
            store_set_live(ld, 0, 1.0f);                         /* rebuilds allowed again */
            did = store_maintain(ld);
            store_live_stats(ld, (i64 *)0, &nreb, (f64 *)0);
            r_d1 = sd_recall_(ld, x4, n4 + nd, d4, qd, nq4, 600000, gone);
            print_cstr("  drift: "); print_i64(nd); print_cstr(" rows from 30 new topics -> "); print_i64(reasons);
            print_cstr(" far windows, rebuilt "); print_i64(nreb - nreb0); print_cstr("x; recall@10 of their queries at 8 probes ");
            print_i64((i64)(1000.0f * r_d0)); print_cstr(" -> "); print_i64((i64)(1000.0f * r_d1)); print_cstr("/1000\n");
            if (reasons < 1 || !(did & 2) || nreb != nreb0 + 1 || r_d1 < r_d0 + 0.05f) ok = 0;
            store_destroy(ld);
        }
        store_ivf_min_ = STORE_IVF_MIN;
        store_destroy(lv);
        remove("/tmp/store_demo_live.vxi"); remove("/tmp/store_demo_live.vxi.j");
        free(mr); free(x4); free(q4); free(gone);
    }

    /* journal: base save, then only the changes are synced; a reload replays them (torn tail tolerated) */
    {
        RagStore *j = store_new(a, dim), *jl;
        i32 jn = 500, hit = 0, gone = 0, want_live;
        FILE *g;
        store_set_fast(j, 0.9f);
        for (i = 0; i < jn; ++i) store_add(j, vv + (size_t)i * dim, 9000 + i);
        store_build(j);
        if (store_sync(j, "/tmp/store_demo_j.vxi") != 0) ok = 0;            /* first call = full save */
        for (i = jn; i < jn + 60; ++i) store_add(j, vv + (size_t)i * dim, 9000 + i);
        for (i = 0; i < 20; ++i) store_delete(j, 9000 + i * 7);
        store_set_meta(j, 9000 + 3, 42);
        if (store_sync(j, "/tmp/store_demo_j.vxi") != 0) ok = 0;            /* journal flush only */
        want_live = store_count(j);
        g = fopen("/tmp/store_demo_j.vxi.j", "ab"); if (g) { fwrite("\x41\x01\x02", 1, 3, g); fclose(g); }   /* torn record */
        jl = store_load(a, "/tmp/store_demo_j.vxi");
        if (!jl) ok = 0;
        else {
            for (i = jn; i < jn + 60; ++i) { i64 r1[1]; f32 c1[1]; if (store_search(jl, vv + (size_t)i * dim, 1, r1, c1) > 0 && r1[0] == 9000 + i) ++hit; }
            for (i = 0; i < 20; ++i) { i64 r1[3]; f32 c1[3]; i32 h = store_search(jl, vv + (size_t)(i * 7) * dim, 3, r1, c1), q2; for (q2 = 0; q2 < h; ++q2) if (r1[q2] == 9000 + i * 7) ++gone; }
            print_cstr("  journal: live "); print_i64(store_count(jl)); print_cstr(" of "); print_i64(want_live);
            print_cstr(", replayed adds found "); print_i64(hit); print_cstr("/60, deleted still visible "); print_i64(gone); print_cstr("\n");
            if (store_count(jl) != want_live || hit < 55 || gone != 0) ok = 0;
            store_destroy(jl);
        }
        store_destroy(j);
    }

    /* background snapshots: a child writes a copy-on-write image of this moment while rows keep coming. A copy to
     * another file holds that moment only; a snapshot of the store's own file takes the journal over, so the file and
     * its journal hold everything, including what came in while the child wrote. */
    {
        RagStore *b = store_new(a, dim), *lc, *lo;
        const char *P = "/tmp/store_demo_snap.vxi", *C = "/tmp/store_demo_snap_copy.vxi";
        i32 nb = 3000, at_fork, r1, r2, r3, w1, w2, in_copy = 0, late_in_copy = 0, found = 0, gone = 0, self = 0;
        i64 us = 0;
        for (i = 0; i < nb; ++i) store_add(b, vv + (size_t)i * dim, 20000 + i);
        store_build(b);
        if (store_save(b, P) != 0) ok = 0;
        for (i = 0; i < 100; ++i) store_delete(b, 20000 + i * 3);    /* tombstones the child leaves out */
        at_fork = store_count(b);
        r1 = store_snapshot(b, C);
        for (i = nb; i < nb + 400; ++i) store_add(b, vv + (size_t)i * dim, 20000 + i);
        w1 = store_snapshot_poll(b, 1);
        r2 = store_snapshot(b, P);
        for (i = nb + 400; i < nb + 600; ++i) store_add(b, vv + (size_t)i * dim, 20000 + i);
        store_delete(b, 20000 + 1);
        w2 = store_snapshot_poll(b, 1);
        store_snapshot_info(b, (i32 *)0, &r3, &us);
        for (i = nb + 600; i < nb + 700; ++i) store_add(b, vv + (size_t)i * dim, 20000 + i);
        if (store_sync(b, P) != 0) ok = 0;
        lc = store_load(a, C);
        lo = store_load(a, P);
        if (!lc || !lo) ok = 0;
        else {
            for (i = 0; i < nb + 700; i += 7) {
                i64 r1h[3]; f32 c1[3];
                i32 h = store_search(lc, vv + (size_t)i * dim, 3, r1h, c1), q, hit = 0;
                for (q = 0; q < h; ++q) if (r1h[q] == 20000 + i) hit = 1;
                if (i < nb && hit) ++in_copy;
                if (i >= nb && hit) ++late_in_copy;
                h = store_search(lo, vv + (size_t)i * dim, 3, r1h, c1);
                for (q = 0, hit = 0; q < h; ++q) if (r1h[q] == 20000 + i) hit = 1;
                if ((i < 300 && i % 3 == 0) || i == 1) { if (hit) ++gone; }
                else { if (hit) ++found; ++self; }
            }
            print_cstr("  snapshot: a copy of "); print_i64(store_count(lc)); print_cstr(" rows (the moment of the fork: ");
            print_i64(at_fork); print_cstr(", none of the 700 added later); its own file + journal "); print_i64(store_count(lo));
            print_cstr(" of "); print_i64(store_count(b)); print_cstr(", rows found "); print_i64(found); print_cstr("/"); print_i64(self);
            print_cstr(", deleted still visible "); print_i64(gone); print_cstr(", last one took "); print_i64(us / 1000); print_cstr(" ms\n");
            if (r1 != 0 || r2 != 0 || w1 != 0 || w2 != 0 || r3 != 0 || store_count(lc) != at_fork || late_in_copy != 0
                || in_copy * 100 < ((nb + 6) / 7 - 15) * 95 || store_count(lo) != store_count(b) || found != self || gone != 0 || lo->jf2) ok = 0;
            store_destroy(lc); store_destroy(lo);
        }
        store_destroy(b);
        remove(P); remove("/tmp/store_demo_snap.vxi.j"); remove("/tmp/store_demo_snap.vxi.jn");
        remove(C); remove("/tmp/store_demo_snap_copy.vxi.j");
    }
    print_cstr(ok ? "  OK\n" : "  FAIL\n");
    print_flush();
    arena_free(a);
    return ok ? 0 : 1;
}
