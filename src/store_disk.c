/* store_disk.c - saving and loading the .vxi file, and the journal of changes since the last save.
 * Part of store.c, which includes it at this point; not compiled on its own. */

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
