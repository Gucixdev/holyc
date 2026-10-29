/* vserv_proto.c - the two request protocols: binary frames (first byte 0xB5) and HTTP/1.1.
 * Part of vserv.c, which includes it at this point; not compiled on its own. */

/* ---- binary protocol ---- */

static void vs_bin_reply_(VsWorker *w, VsConn *c, u8 status, u16 count, const char *payload, u32 nbytes)
{
    char *o = w->out;
    o[0] = (char)VS_MAGIC; o[1] = (char)status;
    o[2] = (char)(count & 0xff); o[3] = (char)(count >> 8);
    o[4] = (char)(nbytes & 0xff); o[5] = (char)((nbytes >> 8) & 0xff); o[6] = (char)((nbytes >> 16) & 0xff); o[7] = (char)(nbytes >> 24);
    if (nbytes && payload != o + 8) memmove(o + 8, payload, nbytes);
    vs_send_(w, c, o, (i32)(8 + nbytes));
}

static u32 vs_le32_(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static i64 vs_le64_(const u8 *p) { return (i64)((u64)vs_le32_(p) | ((u64)vs_le32_(p + 4) << 32)); }

/* returns bytes consumed, 0 if the frame is incomplete, -1 to close */
static i32 vs_binary_(VsWorker *w, VsConn *c)
{
    const u8 *b = (const u8 *)c->buf;
    u32 nb;
    i32 op, k, i, got;
    if (c->len < 8) return 0;
    op = b[1]; k = (i32)(b[2] | (b[3] << 8)); nb = vs_le32_(b + 4);
    if (nb > (u32)vs_reqcap - 8) return -1;
    if ((u32)c->len < 8 + nb) return 0;
    b += 8;
    switch (op) {
    case 1:
        if (nb != (u32)vs_dim * 4) { vs_bin_reply_(w, c, 2, 0, (const char *)0, 0); break; }
        memcpy(w->vec, b, nb);
        if (k < 1) k = 10;
        if (k > VS_MAXK) k = VS_MAXK;
        got = vs_search_(w, w->vec, k);
        {   char *o = w->out + 8;
            for (i = 0; i < got; ++i) { memcpy(o, &w->ids[i], 8); memcpy(o + 8, &w->sc[i], 4); o += 12; }
            vs_bin_reply_(w, c, 0, (u16)got, w->out + 8, (u32)got * 12u); }
        break;
    case 2:
        if (nb != 8 + (u32)vs_dim * 4) { vs_bin_reply_(w, c, 2, 0, (const char *)0, 0); break; }
        memcpy(w->vec, b + 8, (size_t)vs_dim * 4);
        vs_bin_reply_(w, c, store_add(vs_store, w->vec, vs_le64_(b)) == 0 ? 0 : 1, 0, (const char *)0, 0);
        break;
    case 3:
        if (nb != 8) { vs_bin_reply_(w, c, 2, 0, (const char *)0, 0); break; }
        vs_bin_reply_(w, c, 0, (u16)store_delete(vs_store, vs_le64_(b)), (const char *)0, 0);
        break;
    case 4: store_build(vs_store); vs_bin_reply_(w, c, 0, 0, (const char *)0, 0); break;
    case 5: vs_bin_reply_(w, c, (u8)(vs_path && store_save(vs_store, vs_path) == 0 ? 0 : 1), 0, (const char *)0, 0); break;
    case 6: { i64 v[4]; vs_stats_(v); vs_bin_reply_(w, c, 0, 4, (const char *)v, sizeof(v)); } break;
    default: vs_bin_reply_(w, c, 3, 0, (const char *)0, 0); break;
    }
    return (i32)(8 + nb);
}

/* ---- HTTP/1.1 ---- */

/* offset just past "\r\n\r\n" in p[0..n), or -1. SSE2: 16 bytes per step looking for '\r' (the
 * picohttpparser idea, reduced to what this server needs); scalar check of each hit. */
static i32 vs_head_end_(const char *p, i32 n)
{
    i32 i = 0;
#if defined(__SSE2__)
    const __m128i cr = _mm_set1_epi8('\r');
    for (; i + 16 <= n; i += 16) {
        i32 m = _mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128((const __m128i *)(const void *)(p + i)), cr));
        while (m) {
            i32 j = i + tq_ctz_((u32)m);
            m &= m - 1;
            if (j + 3 < n && p[j + 1] == '\n' && p[j + 2] == '\r' && p[j + 3] == '\n') return j + 4;
        }
    }
    if (i >= 3) i -= 3;                          /* a terminator may straddle the last full block */
#endif
    for (; i + 3 < n; ++i)
        if (p[i] == '\r' && p[i + 1] == '\n' && p[i + 2] == '\r' && p[i + 3] == '\n') return i + 4;
    return -1;
}

static i32 vs_ieq_(const char *a, const char *b, i32 n)
{
    i32 i;
    for (i = 0; i < n; ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (x != y) return 0;
    }
    return 1;
}

/* value of query parameter name= in the request target [t, t+tn), or def */
static i64 vs_qparam_(const char *t, i32 tn, const char *name, i64 def)
{
    i32 nl = (i32)strlen(name), i;
    for (i = 0; i < tn; ++i) {
        if ((t[i] == '?' || t[i] == '&') && i + 1 + nl < tn && !memcmp(t + i + 1, name, (size_t)nl) && t[i + 1 + nl] == '=') {
            i64 v = 0, sg = 1;
            i32 j = i + 2 + nl;
            if (j < tn && t[j] == '-') { sg = -1; ++j; }
            for (; j < tn && t[j] >= '0' && t[j] <= '9'; ++j) v = v * 10 + (t[j] - '0');
            return v * sg;
        }
    }
    return def;
}

/* JSON array of exactly vs_dim numbers -> w->vec; 0 ok, -1 bad */
static i32 vs_json_vec_(VsWorker *w, const char *p, i32 n)
{
    const char *e = p + n;
    char num[64];
    i32 d = 0;
    while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p;
    if (p >= e || *p != '[') return -1;
    ++p;
    for (;;) {
        i32 k = 0;
        while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p;
        if (p < e && *p == ']' && d == 0) break;
        while (p < e && k < 63 && ((*p >= '0' && *p <= '9') || *p == '-' || *p == '+' || *p == '.' || *p == 'e' || *p == 'E')) num[k++] = *p++;
        if (k == 0 || d >= vs_dim) return -1;
        num[k] = 0;
        w->vec[d++] = (f32)strtod(num, (char **)0);
        while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p;
        if (p < e && *p == ',') { ++p; continue; }
        if (p < e && *p == ']') break;
        return -1;
    }
    return d == vs_dim ? 0 : -1;
}

static void vs_http_reply_(VsWorker *w, VsConn *c, const char *status, const char *body, i32 blen, i32 keep)
{
    char *o = w->out + VS_OUT_CAP - 256, *p = o;   /* headers go in front of the body, built in place */
    char head[256];
    i32 hn;
    p = head;
    p = vs_str_(p, "HTTP/1.1 "); p = vs_str_(p, status);
    p = vs_str_(p, "\r\nContent-Type: application/json\r\nContent-Length: ");
    p = vs_i64_(p, blen);
    p = vs_str_(p, keep ? "\r\n\r\n" : "\r\nConnection: close\r\n\r\n");
    hn = (i32)(p - head);
    (void)o;
    if (body == w->out + 256) {                  /* body already sits after a 256-byte gap: prepend in place */
        memcpy(w->out + 256 - hn, head, (size_t)hn);
        vs_send_(w, c, w->out + 256 - hn, hn + blen);
    } else {
        vs_send_(w, c, head, hn);
        if (blen) vs_send_(w, c, body, blen);
    }
    if (!keep) c->closing = 1;
}

/* returns bytes consumed, 0 if incomplete, -1 to close */
static i32 vs_http_(VsWorker *w, VsConn *c)
{
    const char *b = c->buf, *line, *t, *body;
    i32 he = vs_head_end_(b, c->len), i, mlen, tn, clen = 0, keep = 1, raw = 0, got, k;
    char *o;
    if (he < 0) {
        if (c->len >= vs_reqcap) { vs_http_reply_(w, c, "431 Request Header Fields Too Large", "{\"error\":\"headers too large\"}", 29, 0); return -1; }
        return 0;
    }
    /* request line: METHOD SP TARGET SP HTTP/1.x */
    for (mlen = 0; mlen < he && b[mlen] != ' '; ++mlen) {}
    t = b + mlen + 1;
    for (tn = 0; mlen + 1 + tn < he && t[tn] != ' ' && t[tn] != '\r'; ++tn) {}
    if (mlen >= he || tn == 0) { vs_http_reply_(w, c, "400 Bad Request", "{\"error\":\"bad request line\"}", 28, 0); return -1; }
    if (mlen + 1 + tn + 9 <= he && !memcmp(t + tn, " HTTP/1.0", 9)) keep = 0;
    /* headers */
    for (line = b; ; ) {
        const char *nl = (const char *)memchr(line, '\n', (size_t)(b + he - line));
        i32 ll;
        if (!nl) break;
        line = nl + 1;
        ll = (i32)((const char *)memchr(line, '\n', (size_t)(b + he - line + 1)) ? 0 : 0);
        (void)ll;
        if (line >= b + he - 2) break;
        if (b + he - line > 15 && vs_ieq_(line, "content-length:", 15)) {
            const char *v = line + 15;
            while (*v == ' ') ++v;
            for (clen = 0; *v >= '0' && *v <= '9'; ++v) { clen = clen * 10 + (*v - '0'); if (clen > vs_reqcap) break; }
        } else if (b + he - line > 13 && vs_ieq_(line, "content-type:", 13)) {
            const char *v = line + 13;
            while (*v == ' ') ++v;
            if (b + he - v > 24 && vs_ieq_(v, "application/octet-stream", 24)) raw = 1;
        } else if (b + he - line > 11 && vs_ieq_(line, "connection:", 11)) {
            const char *v = line + 11;
            while (*v == ' ') ++v;
            if (b + he - v >= 5 && vs_ieq_(v, "close", 5)) keep = 0;
            if (b + he - v >= 10 && vs_ieq_(v, "keep-alive", 10)) keep = 1;
        }
    }
    if (clen > vs_reqcap - he) { vs_http_reply_(w, c, "413 Content Too Large", "{\"error\":\"body too large\"}", 26, 0); return -1; }
    if (c->len < he + clen) return 0;
    body = b + he;
    o = w->out + 256;                             /* bodies are written here; vs_http_reply_ prepends headers */

#define VS_IS(m, path) (mlen == (i32)strlen(m) && !memcmp(b, m, (size_t)mlen) && tn >= (i32)strlen(path) \
                        && !memcmp(t, path, strlen(path)) && (tn == (i32)strlen(path) || t[strlen(path)] == '?'))
    if (VS_IS("GET", "/health")) {
        vs_http_reply_(w, c, "200 OK", "{\"ok\":true}", 11, keep);
    } else if (VS_IS("GET", "/stats")) {
        i64 v[4]; char *p = o;
        vs_stats_(v);
        p = vs_str_(p, "{\"count\":"); p = vs_i64_(p, v[0]); p = vs_str_(p, ",\"dim\":"); p = vs_i64_(p, v[1]);
        p = vs_str_(p, ",\"pending\":"); p = vs_i64_(p, v[2]); p = vs_str_(p, ",\"built\":"); p = vs_str_(p, v[3] ? "true}" : "false}");
        vs_http_reply_(w, c, "200 OK", o, (i32)(p - o), keep);
    } else if (VS_IS("POST", "/search")) {
        char *p = o;
        if (raw ? clen != vs_dim * 4 : vs_json_vec_(w, body, clen) != 0) {
            vs_http_reply_(w, c, "400 Bad Request", "{\"error\":\"expected a vector of the store's dimension\"}", 54, keep);
        } else {
            if (raw) memcpy(w->vec, body, (size_t)vs_dim * 4);
            k = (i32)vs_qparam_(t, tn, "k", 10);
            if (k < 1) k = 1;
            if (k > VS_MAXK) k = VS_MAXK;
            got = vs_search_(w, w->vec, k);
            p = vs_str_(p, "{\"ids\":[");
            for (i = 0; i < got; ++i) { if (i) *p++ = ','; p = vs_i64_(p, w->ids[i]); }
            p = vs_str_(p, "],\"scores\":[");
            for (i = 0; i < got; ++i) { if (i) *p++ = ','; p = vs_f32_(p, w->sc[i]); }
            p = vs_str_(p, "]}");
            vs_http_reply_(w, c, "200 OK", o, (i32)(p - o), keep);
        }
    } else if (VS_IS("POST", "/add")) {
        i64 id = vs_qparam_(t, tn, "id", -1);
        if (id < 0 || (raw ? clen != vs_dim * 4 : vs_json_vec_(w, body, clen) != 0))
            vs_http_reply_(w, c, "400 Bad Request", "{\"error\":\"need ?id=N and a vector of the store's dimension\"}", 60, keep);
        else {
            if (raw) memcpy(w->vec, body, (size_t)vs_dim * 4);
            if (store_add(vs_store, w->vec, id) == 0) vs_http_reply_(w, c, "200 OK", "{\"ok\":true}", 11, keep);
            else vs_http_reply_(w, c, "400 Bad Request", "{\"error\":\"vector has NaN or Inf\"}", 33, keep);
        }
    } else if (VS_IS("POST", "/delete")) {
        char *p = o;
        p = vs_str_(p, "{\"removed\":"); p = vs_i64_(p, store_delete(vs_store, vs_qparam_(t, tn, "id", -1))); *p++ = '}';
        vs_http_reply_(w, c, "200 OK", o, (i32)(p - o), keep);
    } else if (VS_IS("POST", "/build")) {
        store_build(vs_store);
        vs_http_reply_(w, c, "200 OK", "{\"ok\":true}", 11, keep);
    } else if (VS_IS("POST", "/save")) {
        if (vs_path && store_save(vs_store, vs_path) == 0) vs_http_reply_(w, c, "200 OK", "{\"ok\":true}", 11, keep);
        else vs_http_reply_(w, c, "500 Internal Server Error", "{\"error\":\"save failed\"}", 23, keep);
    } else {
        vs_http_reply_(w, c, "404 Not Found", "{\"error\":\"not found\"}", 21, keep);
    }
#undef VS_IS
    return he + clen;
}
