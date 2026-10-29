/* vserv.c - vecindex over a socket: libc + pthreads + Linux epoll, and TLS through the vendored BearSSL
 * (third_party/bearssl, MIT, no malloc inside). Nothing else.
 *
 *   vserv [-s SOCK] [-p PORT] [-b ADDR] [-t THREADS] [-d DIM] [-x] [-L] [-r RECALL] [-c CERT.pem -k KEY.pem [-a CA.pem] [-P] [-T]] STORE.vxi
 *     -s PATH   Unix socket, created mode 0600 (default ./vecindex.sock when -p is not given)
 *     -p PORT   TCP port, bound to 127.0.0.1 unless -b gives another address
 *     -t N      worker threads (default: online CPUs)
 *     -d DIM    dimension of a new, empty store when STORE.vxi does not exist yet
 *     -x        save the store to STORE.vxi on SIGINT / SIGTERM
 *     -L        live: added rows are indexed as they come and the index is rebuilt in the background as it ages
 *               (grown 2x, the data drifted, a quarter deleted) - no build calls needed (store_set_live)
 *     -r R      keep recall@10 at R (e.g. 0.95): the probes are tuned on the queries the server answers, after every
 *               build or rebuild (store_set_target_recall)
 *     -c, -k    TLS: certificate chain and private key (PEM, EC or RSA). The TCP port then takes only TLS: HTTPS,
 *               or the binary protocol inside TLS (-P lets plaintext in too). The Unix socket takes TLS as well and
 *               still accepts plaintext from local clients, unless -T
 *     -a FILE   mutual TLS: a client must present a certificate that chains to one of the CA certificates in FILE
 *               (or is one of them), else the handshake fails
 *
 * The store is opened with store_load (mmap: ready in milliseconds whatever its size). Every socket speaks
 * two protocols, told apart by the first byte of a request:
 *
 *   binary (first byte 0xB5): fixed little-endian frames, nothing to parse
 *     request  u8 0xB5 | u8 op | u16 k | u32 nbytes | payload[nbytes]
 *     response u8 0xB5 | u8 status (0 ok) | u16 count | u32 nbytes | payload[nbytes]
 *     op 1 search  payload: dim x f32            reply: count x { i64 id, f32 score }
 *     op 2 add     payload: i64 id, dim x f32
 *     op 3 delete  payload: i64 id               reply count: rows removed
 *     op 4 build   op 5 save   op 6 stats        stats reply: i64 live, dim, pending, built
 *
 *   HTTP/1.1 (anything else), for curl and friends; keep-alive, Content-Length bodies:
 *     GET /health | GET /stats | POST /search?k=10 | POST /add?id=N | POST /delete?id=N | POST /build | POST /save
 *     vectors as a JSON array ([0.1, -0.2, ...]) or raw little-endian f32 with
 *     Content-Type: application/octet-stream; replies are JSON.
 *
 * TLS (when -c/-k are given): a connection is TLS when its first byte is a handshake record (0x16). TLS 1.2 only,
 * ECDHE key exchange (forward secrecy), AEAD suites only (AES-GCM - AES-NI when the CPU has it - and
 * ChaCha20-Poly1305), sessions resumable from one cache shared by every worker. Client certificates with -a. On
 * the Unix socket TLS is optional unless -T (the socket never leaves the machine, and its 0600 mode decides who
 * may connect at all).
 *
 * Memory: an idle connection is one small struct. Its request buffer (one request of this store's
 * dimension) is allocated on first read and reused for every later request on it; responses are built in a
 * per-thread buffer, so a request allocates nothing (a client that stops reading gets its unsent tail
 * buffered until it catches up). TLS adds about 37 KB a connection: the engine state and room for one full
 * 16 KB record in each direction, which the protocol requires.
 *
 *   ./run builds it (BearSSL first, into .build/libbearssl.a); by hand:
 *   gcc -std=c89 -pedantic -Wall -Wextra -O2 -march=native -I. -isystem third_party/bearssl/inc vserv.c \
 *       .build/libbearssl.a -lm -lpthread -o vserv          (-DVSERV_NO_TLS: plaintext only, no BearSSL)
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "store.c"
#include <errno.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif
#if !defined(VSERV_NO_TLS)
#define VSERV_TLS 1
#if defined(__GNUC__) && (!defined(__STDC_VERSION__) || __STDC_VERSION__ < 199901L)
#define inline __inline__                       /* BearSSL's headers use C99 static inline; GCC has it in C89 mode as __inline__ */
#include "bearssl.h"
#undef inline
#else
#include "bearssl.h"
#endif
#endif

#define VS_MAGIC     0xB5
#define VS_MAXK      1000
#define VS_EVENTS    128
#define VS_IDLE_S    60.0
#define VS_OUT_CAP   (64 + VS_MAXK * 48)          /* the largest reply: JSON for VS_MAXK hits */

typedef struct VsTls VsTls;

typedef struct VsConn {
    i32 fd, len, cap, out_len, out_off, closing;
    u8 tcp, checked;                            /* accepted on the TCP port; its first byte looked at (TLS or not) */
    char *buf;                                  /* request bytes (plaintext), allocated on first read */
    char *out;                                  /* reply bytes not yet out (under TLS: not yet in the engine) */
    double last;
    VsTls *tls;                                 /* NULL: plaintext */
    struct VsConn *prev, *next;                 /* the worker's list, for the idle sweep */
} VsConn;

typedef struct { i32 fd, tcp; } VsListen;       /* epoll data for a listening socket */

typedef struct {
    i32 ep;
    pthread_t th;
    VsConn *conns;
    char *out;                                  /* VS_OUT_CAP reply scratch */
    f32 *vec;                                   /* dim floats: an aligned copy of the query */
    i64 *ids;
    f32 *sc;
} VsWorker;

#ifdef VSERV_TLS
struct VsTls {
    br_ssl_server_context sc;
    br_x509_minimal_context xc;                 /* validates client certificates (-a) */
    unsigned char io[BR_SSL_BUFSIZE_BIDI];      /* one full record each way */
};
static br_x509_certificate *vs_chain;
static size_t vs_nchain;
static br_skey_decoder_context vs_skey;         /* the decoded private key, for the server's lifetime */
static i32 vs_keytype, vs_issuer;
static br_x509_trust_anchor *vs_tas;
static size_t vs_ntas;

/* One session cache for every worker: a client that comes back is handed to whichever worker the kernel wakes,
 * so a per-thread cache would miss about half the resumptions. BearSSL's LRU is not thread-safe; a mutex around
 * its two calls is all it needs (a lookup is a short tree walk). */
typedef struct {
    const br_ssl_session_cache_class *vtable;
    br_ssl_session_cache_lru lru;
    pthread_mutex_t mu;
} VsCache;
#define VS_CACHE_BYTES (256 * 1024)             /* ~2500 sessions */
static VsCache vs_cache;
static unsigned char *vs_cache_mem;

static void vs_cache_save_(const br_ssl_session_cache_class **ctx, br_ssl_server_context *sc, const br_ssl_session_parameters *p)
{
    VsCache *c = (VsCache *)(void *)ctx;
    pthread_mutex_lock(&c->mu);
    c->lru.vtable->save(&c->lru.vtable, sc, p);
    pthread_mutex_unlock(&c->mu);
}
static int vs_cache_load_(const br_ssl_session_cache_class **ctx, br_ssl_server_context *sc, br_ssl_session_parameters *p)
{
    VsCache *c = (VsCache *)(void *)ctx;
    int r;
    pthread_mutex_lock(&c->mu);
    r = c->lru.vtable->load(&c->lru.vtable, sc, p);
    pthread_mutex_unlock(&c->mu);
    return r;
}
static const br_ssl_session_cache_class vs_cache_class = { sizeof(VsCache), vs_cache_save_, vs_cache_load_ };
#else
struct VsTls { i32 unused; };
#endif
static i32 vs_tls_on, vs_plain_ok, vs_unix_tls_only;

static RagStore *vs_store;
static const char *vs_path;
static i32 vs_dim, vs_reqcap;
static VsListen vs_ls[2];
static i32 vs_nls;
static volatile sig_atomic_t vs_stop;

static double vs_now_(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec; }
static void vs_on_signal_(int sig) { (void)sig; vs_stop = 1; }

/* ---- small formatting helpers (no printf on the hot path) ---- */

static char *vs_i64_(char *p, i64 v)
{
    char tmp[24];
    i32 n = 0;
    u64 u = v < 0 ? (u64)(-(v + 1)) + 1u : (u64)v;
    if (v < 0) *p++ = '-';
    do { tmp[n++] = (char)('0' + (i32)(u % 10u)); u /= 10u; } while (u);
    while (n) *p++ = tmp[--n];
    return p;
}

/* a score with 6 decimals; scores are -||q-v||^2 on unit vectors, so within [-4, 0] */
static char *vs_f32_(char *p, f32 v)
{
    i64 x;
    i32 i;
    char frac[6];
    if (v != v) { memcpy(p, "null", 4); return p + 4; }
    if (v < 0.0f) { *p++ = '-'; v = -v; }
    if (v > 1e15f) v = 1e15f;
    x = (i64)(v * 1e6f + 0.5f);
    p = vs_i64_(p, x / 1000000);
    *p++ = '.';
    x %= 1000000;
    for (i = 5; i >= 0; --i) { frac[i] = (char)('0' + (i32)(x % 10)); x /= 10; }
    memcpy(p, frac, 6);
    return p + 6;
}

static char *vs_str_(char *p, const char *s) { size_t n = strlen(s); memcpy(p, s, n); return p + n; }

/* ---- sending ---- */

static void vs_ep_mod_(VsWorker *w, VsConn *c, u32 ev)
{
    struct epoll_event e;
    e.events = ev; e.data.ptr = c;
    epoll_ctl(w->ep, EPOLL_CTL_MOD, c->fd, &e);
}

/* keep bytes for later (after what is already queued) and wait for EPOLLOUT */
static void vs_queue_(VsWorker *w, VsConn *c, const char *p, i32 n)
{
    i32 rest = c->out ? c->out_len - c->out_off : 0;
    char *o = (char *)malloc((size_t)rest + (size_t)n);
    if (!o) { c->closing = 1; return; }
    if (rest) memcpy(o, c->out + c->out_off, (size_t)rest);
    memcpy(o + rest, p, (size_t)n);
    free(c->out);
    c->out = o; c->out_len = rest + n; c->out_off = 0;
    vs_ep_mod_(w, c, EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET);
}

#ifdef VSERV_TLS
/* encrypted records to the socket: 1 = all out, 0 = the socket is full, -1 = error */
static i32 vs_tls_tx_(VsConn *c)
{
    br_ssl_engine_context *e = &c->tls->sc.eng;
    for (;;) {
        size_t len;
        unsigned char *b;
        ssize_t k;
        if (!(br_ssl_engine_current_state(e) & BR_SSL_SENDREC)) return 1;
        b = br_ssl_engine_sendrec_buf(e, &len);
        k = send(c->fd, b, len, MSG_NOSIGNAL);
        if (k > 0) { br_ssl_engine_sendrec_ack(e, (size_t)k); continue; }
        if (k < 0 && errno == EINTR) continue;
        if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        return -1;
    }
}

/* plaintext into the engine, as much as it takes now; returns the bytes taken */
static i32 vs_tls_put_(VsConn *c, const char *p, i32 n)
{
    br_ssl_engine_context *e = &c->tls->sc.eng;
    i32 done = 0;
    while (done < n && (br_ssl_engine_current_state(e) & BR_SSL_SENDAPP)) {
        size_t len;
        unsigned char *b = br_ssl_engine_sendapp_buf(e, &len);
        if (!len) break;
        if (len > (size_t)(n - done)) len = (size_t)(n - done);
        memcpy(b, p + done, len);
        br_ssl_engine_sendapp_ack(e, len);
        done += (i32)len;
    }
    return done;
}

static void vs_tls_send_(VsWorker *w, VsConn *c, const char *p, i32 n)
{
    br_ssl_engine_context *e = &c->tls->sc.eng;
    i32 r;
    while (n > 0 && !c->out) {
        i32 k = vs_tls_put_(c, p, n);
        p += k; n -= k;
        if (n == 0) break;
        br_ssl_engine_flush(e, 0);                /* the engine is full: records out first */
        r = vs_tls_tx_(c);
        if (r < 0) { c->closing = 1; return; }
        if (r == 0 || (k == 0 && !(br_ssl_engine_current_state(e) & BR_SSL_SENDAPP))) break;
    }
    if (n > 0) vs_queue_(w, c, p, n);
    br_ssl_engine_flush(e, 0);
    r = vs_tls_tx_(c);
    if (r < 0) c->closing = 1;
    else if (r == 0) vs_ep_mod_(w, c, EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET);
}
#endif

/* send what we can; keep the rest on the connection and wait for EPOLLOUT */
static void vs_send_(VsWorker *w, VsConn *c, const char *p, i32 n)
{
#ifdef VSERV_TLS
    if (c->tls) { vs_tls_send_(w, c, p, n); return; }
#endif
    if (c->out) { vs_queue_(w, c, p, n); return; }      /* never ahead of bytes already waiting */
    while (n > 0) {
        ssize_t k = send(c->fd, p, (size_t)n, MSG_NOSIGNAL);
        if (k > 0) { p += k; n -= (i32)k; continue; }
        if (k < 0 && errno == EINTR) continue;
        if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        c->closing = 1;
        return;
    }
    if (n > 0) vs_queue_(w, c, p, n);
}

/* reply bytes still on their way out */
static i32 vs_pending_(const VsConn *c)
{
    if (c->out) return 1;
#ifdef VSERV_TLS
    if (c->tls && (br_ssl_engine_current_state(&c->tls->sc.eng) & BR_SSL_SENDREC)) return 1;
#endif
    return 0;
}

static void vs_flush_(VsWorker *w, VsConn *c)
{
#ifdef VSERV_TLS
    if (c->tls) {
        br_ssl_engine_context *e = &c->tls->sc.eng;
        i32 r = vs_tls_tx_(c);
        if (r < 0) { c->closing = 1; return; }
        if (r == 0) return;
        while (c->out && c->out_off < c->out_len) {
            i32 k = vs_tls_put_(c, c->out + c->out_off, c->out_len - c->out_off);
            c->out_off += k;
            br_ssl_engine_flush(e, 0);
            r = vs_tls_tx_(c);
            if (r < 0) { c->closing = 1; return; }
            if (r == 0 || k == 0) return;         /* the next EPOLLOUT goes on */
        }
        if (c->out) { free(c->out); c->out = (char *)0; c->out_len = c->out_off = 0; }
        vs_ep_mod_(w, c, EPOLLIN | EPOLLRDHUP | EPOLLET);
        return;
    }
#endif
    while (c->out && c->out_off < c->out_len) {
        ssize_t k = send(c->fd, c->out + c->out_off, (size_t)(c->out_len - c->out_off), MSG_NOSIGNAL);
        if (k > 0) { c->out_off += (i32)k; continue; }
        if (k < 0 && errno == EINTR) continue;
        if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        c->closing = 1;
        return;
    }
    free(c->out); c->out = (char *)0; c->out_len = c->out_off = 0;
    vs_ep_mod_(w, c, EPOLLIN | EPOLLRDHUP | EPOLLET);
}

/* ---- the store operations both protocols share ---- */

static i32 vs_search_(VsWorker *w, const f32 *q, i32 k) { return store_search(vs_store, q, k, w->ids, w->sc); }

static void vs_stats_(i64 *v)
{
    v[0] = store_count(vs_store); v[1] = vs_dim; v[2] = store_pending(vs_store); v[3] = store_built(vs_store);
}

#include "vserv_proto.c"

/* ---- connection lifecycle ---- */

static void vs_close_(VsWorker *w, VsConn *c)
{
#ifdef VSERV_TLS
    if (c->tls) {
        br_ssl_engine_context *e = &c->tls->sc.eng;
        if (!(br_ssl_engine_current_state(e) & BR_SSL_CLOSED)) { br_ssl_engine_close(e); vs_tls_tx_(c); }   /* close_notify, best effort */
        explicit_bzero(c->tls, sizeof(VsTls));     /* session keys do not outlive the connection */
        free(c->tls);
    }
#endif
    epoll_ctl(w->ep, EPOLL_CTL_DEL, c->fd, (struct epoll_event *)0);
    {   /* closing over unread input makes the kernel send a reset that can overtake the last reply: say FIN first,
         * then drop whatever the peer already sent */
        char sink[4096];
        i32 n = 16;
        shutdown(c->fd, SHUT_WR);
        while (n-- > 0 && read(c->fd, sink, sizeof(sink)) > 0) {}
    }
    close(c->fd);
    if (c->prev) c->prev->next = c->next; else w->conns = c->next;
    if (c->next) c->next->prev = c->prev;
    free(c->buf); free(c->out); free(c);
}

/* parse and answer every complete request in the buffer */
static void vs_process_(VsWorker *w, VsConn *c)
{
    while (c->len > 0 && !c->closing && !c->out) {
        i32 used = (u8)c->buf[0] == VS_MAGIC ? vs_binary_(w, c) : vs_http_(w, c);
        if (used < 0) { c->closing = 1; break; }
        if (used == 0) break;
        c->len -= used;
        if (c->len) memmove(c->buf, c->buf + used, (size_t)c->len);
    }
}

#ifdef VSERV_TLS
static VsTls *vs_tls_new_(VsWorker *w)
{
    static const uint16_t ec_suites[] = { BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256, BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
                                          BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384 };
    static const uint16_t rsa_suites[] = { BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256, BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
                                           BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384 };
    VsTls *t = (VsTls *)malloc(sizeof(VsTls));
    if (!t) return (VsTls *)0;
    if (vs_keytype == BR_KEYTYPE_EC) {
        br_ssl_server_init_full_ec(&t->sc, vs_chain, vs_nchain, (unsigned)vs_issuer, br_skey_decoder_get_ec(&vs_skey));
        br_ssl_engine_set_suites(&t->sc.eng, ec_suites, 3);
    } else {
        br_ssl_server_init_full_rsa(&t->sc, vs_chain, vs_nchain, br_skey_decoder_get_rsa(&vs_skey));
        br_ssl_engine_set_suites(&t->sc.eng, rsa_suites, 3);
    }
    br_ssl_engine_set_versions(&t->sc.eng, BR_TLS12, BR_TLS12);
    if (vs_ntas) {                                 /* mutual TLS: the client's chain must end at one of our CAs */
        br_x509_minimal_init_full(&t->xc, vs_tas, vs_ntas);
        br_ssl_engine_set_default_rsavrfy(&t->sc.eng);
        br_ssl_engine_set_default_ecdsa(&t->sc.eng);
        br_ssl_server_set_trust_anchor_names_alt(&t->sc, vs_tas, vs_ntas);
        br_ssl_engine_set_x509(&t->sc.eng, &t->xc.vtable);
    }
    br_ssl_server_set_cache(&t->sc, &vs_cache.vtable);
    (void)w;
    br_ssl_engine_set_buffer(&t->sc.eng, t->io, sizeof(t->io), 1);
    if (!br_ssl_server_reset(&t->sc)) { free(t); return (VsTls *)0; }
    return t;
}

/* move records in, plaintext up to the parsers, replies back down, until nothing moves */
static void vs_tls_readable_(VsWorker *w, VsConn *c)
{
    br_ssl_engine_context *e = &c->tls->sc.eng;
    for (;;) {
        unsigned st = br_ssl_engine_current_state(e);
        if (st & BR_SSL_CLOSED) { c->closing = 1; return; }
        if (st & BR_SSL_SENDREC) {                /* handshake messages and replies */
            i32 r = vs_tls_tx_(c);
            if (r < 0) { c->closing = 1; return; }
            if (r == 0) vs_ep_mod_(w, c, EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET);
        }
        if ((st & BR_SSL_RECVAPP) && !c->out) {   /* requests wait while replies are queued */
            size_t len;
            unsigned char *b = br_ssl_engine_recvapp_buf(e, &len);
            i32 room = c->cap - c->len;
            if (room <= 0) { c->closing = 1; return; }
            if (len > (size_t)room) len = (size_t)room;
            memcpy(c->buf + c->len, b, len);
            br_ssl_engine_recvapp_ack(e, len);
            c->len += (i32)len;
            c->last = vs_now_();
            vs_process_(w, c);
            if (c->closing) return;
            continue;
        }
        if (st & BR_SSL_RECVREC) {
            size_t len;
            unsigned char *b = br_ssl_engine_recvrec_buf(e, &len);
            ssize_t k = read(c->fd, b, len);
            if (k > 0) { br_ssl_engine_recvrec_ack(e, (size_t)k); c->last = vs_now_(); continue; }
            if (k == 0) { c->closing = 1; return; }
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) c->closing = 1;
            return;
        }
        return;
    }
}
#endif

static void vs_readable_(VsWorker *w, VsConn *c)
{
    if (!c->buf) {
        c->buf = (char *)malloc((size_t)vs_reqcap);
        if (!c->buf) { c->closing = 1; return; }
        c->cap = vs_reqcap;
    }
    if (vs_tls_on && !c->checked) {                /* a TLS ClientHello starts with a handshake record, 0x16 */
        u8 b0;
        ssize_t k = recv(c->fd, &b0, 1, MSG_PEEK);
        if (k == 0) { c->closing = 1; return; }
        if (k < 0) { if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) c->closing = 1; return; }
        c->checked = 1;
#ifdef VSERV_TLS
        if (b0 == 0x16) {
            c->tls = vs_tls_new_(w);
            if (!c->tls) { c->closing = 1; return; }
        } else
#endif
        if (c->tcp ? !vs_plain_ok : vs_unix_tls_only) {
            static const char msg[] = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: 32\r\n"
                                      "Connection: close\r\n\r\n{\"error\":\"this port speaks TLS\"}";
            vs_send_(w, c, msg, (i32)sizeof(msg) - 1);
            c->closing = 1;
            return;
        }
    }
#ifdef VSERV_TLS
    if (c->tls) { vs_tls_readable_(w, c); return; }
#endif
    for (;;) {
        ssize_t k;
        if (c->len == c->cap) { c->closing = 1; return; }  /* the parsers always make room or close */
        k = read(c->fd, c->buf + c->len, (size_t)(c->cap - c->len));
        if (k > 0) {
            c->len += (i32)k;
            c->last = vs_now_();
            vs_process_(w, c);
            if (c->closing || c->out) return;
            continue;
        }
        if (k == 0) { c->closing = 1; return; }
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) c->closing = 1;
        return;
    }
}

static void vs_accept_(VsWorker *w, const VsListen *l)
{
    for (;;) {
        struct epoll_event e;
        VsConn *c;
        i32 fd = accept4(l->fd, (struct sockaddr *)0, (socklen_t *)0, SOCK_NONBLOCK | SOCK_CLOEXEC), one = 1;
        if (fd < 0) return;                      /* EAGAIN: another worker took it, or none left */
        if (l->tcp) setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        c = (VsConn *)calloc(1, sizeof(VsConn));
        if (!c) { close(fd); continue; }
        c->fd = fd; c->last = vs_now_(); c->tcp = (u8)l->tcp;
        c->next = w->conns; if (w->conns) w->conns->prev = c; w->conns = c;
        e.events = EPOLLIN | EPOLLRDHUP | EPOLLET; e.data.ptr = c;
        if (epoll_ctl(w->ep, EPOLL_CTL_ADD, fd, &e) != 0) { vs_close_(w, c); continue; }
    }
}

static void *vs_worker_(void *arg)
{
    VsWorker *w = (VsWorker *)arg;
    struct epoll_event ev[VS_EVENTS];
    double sweep = vs_now_();
    while (!vs_stop) {
        i32 n = epoll_wait(w->ep, ev, VS_EVENTS, 1000), i;
        for (i = 0; i < n; ++i) {
            void *p = ev[i].data.ptr;
            VsConn *c;
            if (p == (void *)&vs_ls[0] || p == (void *)&vs_ls[1]) { vs_accept_(w, (const VsListen *)p); continue; }
            c = (VsConn *)p;
            if (ev[i].events & (EPOLLERR | EPOLLHUP)) { vs_close_(w, c); continue; }
            if (ev[i].events & EPOLLOUT) {
                vs_flush_(w, c);
                if (!c->closing && !vs_pending_(c)) vs_readable_(w, c);   /* requests that waited behind the backlog */
            }
            if (!c->closing && (ev[i].events & (EPOLLIN | EPOLLRDHUP))) vs_readable_(w, c);
            if (c->closing) {                            /* a last reply still going out keeps it open until EPOLLOUT */
                if (vs_pending_(c)) vs_flush_(w, c);
                if (!vs_pending_(c)) vs_close_(w, c);
            }
        }
        if (vs_now_() - sweep > 1.0) {           /* drop connections idle for VS_IDLE_S */
            VsConn *c = w->conns, *nx;
            double now = vs_now_();
            sweep = now;
            for (; c; c = nx) { nx = c->next; if (now - c->last > VS_IDLE_S) vs_close_(w, c); }
        }
    }
    while (w->conns) vs_close_(w, w->conns);
    return (void *)0;
}

/* ---- setup ---- */

static i32 vs_listen_unix_(const char *path)
{
    struct sockaddr_un a;
    struct stat st;
    mode_t old;
    i32 fd;
    if (strlen(path) >= sizeof(a.sun_path)) { fprintf(stderr, "vserv: socket path too long\n"); return -1; }
    if (lstat(path, &st) == 0) {
        if (!S_ISSOCK(st.st_mode)) { fprintf(stderr, "vserv: %s exists and is not a socket\n", path); return -1; }
        unlink(path);                            /* a stale socket from an earlier run */
    }
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    memset(&a, 0, sizeof(a)); a.sun_family = AF_UNIX; strcpy(a.sun_path, path);
    old = umask(077);                            /* the socket file gets 0600: owner only */
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(fd, 1024) != 0) { umask(old); close(fd); return -1; }
    umask(old);
    return fd;
}

static i32 vs_listen_tcp_(const char *addr, i32 port)
{
    struct sockaddr_in a;
    i32 fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0), one = 1;
    if (fd < 0) return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&a, 0, sizeof(a)); a.sin_family = AF_INET; a.sin_port = htons((u16)port);
    if (inet_pton(AF_INET, addr, &a.sin_addr) != 1) { close(fd); return -1; }
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(fd, 1024) != 0) { close(fd); return -1; }
    return fd;
}

static i32 vs_ntas_(void)
{
#ifdef VSERV_TLS
    return vs_ntas > 0;
#else
    return 0;
#endif
}

#include "vserv_pem.c"

int main(int argc, char **argv)
{
    static Arena arena;
    const char *sock = (const char *)0, *bind_addr = "127.0.0.1", *cert = (const char *)0, *key = (const char *)0, *ca = (const char *)0;
    i32 port = 0, nth = 0, dim = 0, save_on_exit = 0, live = 0, i, j;
    f32 recall = 0.0f;
    VsWorker *ws;
    struct sigaction sa;
    double t0;

    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-s") && i + 1 < argc) sock = argv[++i];
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-b") && i + 1 < argc) bind_addr = argv[++i];
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) nth = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) dim = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-x")) save_on_exit = 1;
        else if (!strcmp(argv[i], "-L")) live = 1;
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) recall = (f32)atof(argv[++i]);
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) cert = argv[++i];
        else if (!strcmp(argv[i], "-k") && i + 1 < argc) key = argv[++i];
        else if (!strcmp(argv[i], "-a") && i + 1 < argc) ca = argv[++i];
        else if (!strcmp(argv[i], "-P")) vs_plain_ok = 1;
        else if (!strcmp(argv[i], "-T")) vs_unix_tls_only = 1;
        else if (argv[i][0] != '-' && !vs_path) vs_path = argv[i];
        else { fprintf(stderr, "usage: vserv [-s SOCK] [-p PORT] [-b ADDR] [-t THREADS] [-d DIM] [-x] [-L] [-r RECALL] [-c CERT -k KEY [-a CA] [-P] [-T]] STORE.vxi\n"); return 2; }
    }
    if (!vs_path) { fprintf(stderr, "vserv: no store file given\n"); return 2; }
    if ((cert || key || ca || vs_unix_tls_only) && !(cert && key)) { fprintf(stderr, "vserv: TLS needs both -c CERT and -k KEY\n"); return 2; }
#ifdef VSERV_TLS
    if (cert) {
        if (vs_tls_load_(cert, key, ca)) return 1;
        vs_tls_on = 1;
        vs_cache_mem = (unsigned char *)malloc(VS_CACHE_BYTES);
        br_ssl_session_cache_lru_init(&vs_cache.lru, vs_cache_mem, VS_CACHE_BYTES);
        pthread_mutex_init(&vs_cache.mu, (const pthread_mutexattr_t *)0);
        vs_cache.vtable = &vs_cache_class;
    }
#else
    if (cert) { fprintf(stderr, "vserv: built without TLS (VSERV_NO_TLS)\n"); return 2; }
#endif
    if (!vs_tls_on) vs_plain_ok = 1;
    if (!sock && !port) sock = "vecindex.sock";
    if (nth <= 0) { long c = sysconf(_SC_NPROCESSORS_ONLN); nth = c > 0 ? (i32)c : 1; }

    t0 = vs_now_();
    vs_store = store_load(&arena, vs_path);
    if (!vs_store) {
        if (dim <= 0) { fprintf(stderr, "vserv: cannot load %s (give -d DIM to start a new store)\n", vs_path); return 1; }
        vs_store = store_new(&arena, dim);
    }
    vs_dim = vs_store->dim;
    store_set_threads(vs_store, 1);              /* one query per thread: the workers are the parallelism */
    if (recall > 0.0f) store_set_target_recall(vs_store, recall);
    if (live) store_set_live(vs_store, 1, 1.0f);
    /* one request: headers + a JSON vector (~24 bytes a number) or the binary frame */
    vs_reqcap = 2048 + vs_dim * 24;
    if (vs_reqcap < 4096) vs_reqcap = 4096;

    if (sock) { vs_ls[vs_nls].fd = vs_listen_unix_(sock); if (vs_ls[vs_nls].fd < 0) { fprintf(stderr, "vserv: cannot listen on %s: %s\n", sock, strerror(errno)); return 1; } ++vs_nls; }
    if (port) { vs_ls[vs_nls].tcp = 1; vs_ls[vs_nls].fd = vs_listen_tcp_(bind_addr, port); if (vs_ls[vs_nls].fd < 0) { fprintf(stderr, "vserv: cannot listen on %s:%d: %s\n", bind_addr, port, strerror(errno)); return 1; } ++vs_nls; }

    memset(&sa, 0, sizeof(sa)); sa.sa_handler = vs_on_signal_;
    sigaction(SIGINT, &sa, (struct sigaction *)0); sigaction(SIGTERM, &sa, (struct sigaction *)0);
    signal(SIGPIPE, SIG_IGN);

    ws = (VsWorker *)calloc((size_t)nth, sizeof(VsWorker));
    for (i = 0; i < nth; ++i) {
        VsWorker *w = &ws[i];
        w->ep = epoll_create1(EPOLL_CLOEXEC);
        w->out = (char *)malloc(VS_OUT_CAP);
        w->vec = (f32 *)malloc((size_t)vs_dim * sizeof(f32));
        w->ids = (i64 *)malloc(VS_MAXK * sizeof(i64));
        w->sc = (f32 *)malloc(VS_MAXK * sizeof(f32));
        for (j = 0; j < vs_nls; ++j) {               /* every worker waits on every listener; the kernel wakes one */
            struct epoll_event e;
            e.events = EPOLLIN | EPOLLEXCLUSIVE; e.data.ptr = &vs_ls[j];
            epoll_ctl(w->ep, EPOLL_CTL_ADD, vs_ls[j].fd, &e);
        }
    }
    fprintf(stderr, "vserv: %s: %d rows x %d, loaded in %.1f ms; %d threads;%s%s%s%s%s%s%s\n", vs_path, store_count(vs_store), vs_dim,
            (vs_now_() - t0) * 1e3, nth, live ? " live;" : "", sock ? " unix " : "", sock ? sock : "", port ? " tcp " : "", port ? bind_addr : "",
            vs_tls_on ? (vs_ntas_() ? " (TLS, client certificates required)" : " (TLS)") : "",
            vs_tls_on ? (vs_plain_ok ? " + plaintext on tcp" : (vs_unix_tls_only ? "" : "; plaintext on unix only")) : "");
    for (i = 0; i < nth; ++i) pthread_create(&ws[i].th, (const pthread_attr_t *)0, vs_worker_, &ws[i]);
    for (i = 0; i < nth; ++i) pthread_join(ws[i].th, (void **)0);

    for (j = 0; j < vs_nls; ++j) close(vs_ls[j].fd);
    if (sock) unlink(sock);
    if (save_on_exit) {
        if (store_save(vs_store, vs_path) == 0) fprintf(stderr, "vserv: saved %s\n", vs_path);
        else fprintf(stderr, "vserv: saving %s failed\n", vs_path);
    }
    for (i = 0; i < nth; ++i) {
        close(ws[i].ep); free(ws[i].out); free(ws[i].vec); free(ws[i].ids); free(ws[i].sc);
    }
#ifdef VSERV_TLS
    explicit_bzero(&vs_skey, sizeof(vs_skey));
    if (vs_cache_mem) { explicit_bzero(vs_cache_mem, VS_CACHE_BYTES); free(vs_cache_mem); }   /* master secrets */
#endif
    free(ws);
    return 0;
}
