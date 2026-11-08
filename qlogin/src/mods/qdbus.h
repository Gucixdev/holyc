/* qdbus.h - hand-written D-Bus wire library for qlogin (no libdbus, no GLib, no systemd). stb-style, one header.
 *
 *   #define QDBUS_IMPLEMENTATION   in exactly one .c file (after #define _GNU_SOURCE)
 *
 * Client AND server side: marshalling with D-Bus alignment, message parse with SCM_RIGHTS file descriptors, EXTERNAL auth with
 * UNIX_FD negotiation, Hello, synchronous call (unrelated messages are queued, not lost), replies, errors, signals.
 * Only little-endian ('l') messages are accepted.  Not thread-safe per connection.  Heap buffers, no static message store.
 */
#ifndef QDBUS_H
#define QDBUS_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define QD_MAXFD 16
#define QD_MAXBODY (4u << 20)

typedef struct { unsigned char *b; size_t n, cap; } qd_buf;
typedef struct { size_t lenpos, start; } qd_arr;
typedef struct { const unsigned char *p; size_t pos, len; int bad; } qd_rd;

typedef struct qd_msg {
	int type;                       /* 1 call 2 return 3 error 4 signal */
	int flags;                      /* 1 = no reply expected */
	uint32_t serial, reply_serial, blen;
	const char *path, *iface, *member, *sender, *dest, *err, *sig;   /* never NULL; point into raw */
	const unsigned char *body;
	int fds[QD_MAXFD];              /* received descriptors; owned by whoever takes them (qd_msg_free closes the rest) */
	int nfds;
	unsigned char *raw;
	struct qd_msg *next;
} qd_msg;

typedef struct {
	int fd;
	uint32_t serial;
	unsigned char *rb; size_t rn, rcap;
	int fdq[64]; int nfdq;
	qd_msg *qhead, *qtail;
	char name[48];                  /* our unique name after Hello */
} qd_conn;

/* marshalling */
void qd_put(qd_buf *o, const void *d, size_t n);
void qd_align(qd_buf *o, size_t a);
void qd_put_u8(qd_buf *o, unsigned v);
void qd_put_u32(qd_buf *o, uint32_t v);
void qd_put_u64(qd_buf *o, uint64_t v);
void qd_put_str(qd_buf *o, const char *s);      /* s and o */
void qd_put_sig(qd_buf *o, const char *s);      /* g */
qd_arr qd_arr_begin(qd_buf *o, size_t elem_align);
void qd_arr_end(qd_buf *o, qd_arr a);
void qd_variant_u32(qd_buf *o, uint32_t v);     /* the common variants; others: qd_put_sig + value */
void qd_variant_bool(qd_buf *o, int v);
void qd_variant_u64(qd_buf *o, uint64_t v);
void qd_variant_str(qd_buf *o, const char *s);
void qd_variant_obj(qd_buf *o, const char *s);

/* reading */
uint32_t qd_r_u32(qd_rd *r);
uint64_t qd_r_u64(qd_rd *r);
int qd_r_u8(qd_rd *r);
const char *qd_r_str(qd_rd *r);
const char *qd_r_sig(qd_rd *r);
void qd_r_skip(qd_rd *r, const char **sig);     /* skip one value of the complete type at *sig */
size_t qd_r_array(qd_rd *r, size_t elem_align); /* returns the end position of the array */
size_t qd_sig_len(const char *sig);             /* length of the first complete type */

/* connection */
int qd_open(qd_conn *c, const char *addr);      /* connect + EXTERNAL auth + Hello; 0 ok */
void qd_close(qd_conn *c);
int qd_recv(qd_conn *c, qd_msg *m, int timeout_ms);   /* 1 got a message, 0 timeout, -1 closed/bad */
void qd_msg_free(qd_msg *m);
uint32_t qd_send_call(qd_conn *c, const char *dest, const char *path, const char *iface, const char *member,
                      const char *sig, const qd_buf *body, const int *fds, int nfds);
int qd_send_return(qd_conn *c, const qd_msg *req, const char *sig, const qd_buf *body, const int *fds, int nfds);
int qd_send_error(qd_conn *c, const qd_msg *req, const char *errname, const char *text);
int qd_send_signal(qd_conn *c, const char *dest, const char *path, const char *iface, const char *member,
                   const char *sig, const qd_buf *body);
int qd_send_signal_fds(qd_conn *c, const char *dest, const char *path, const char *iface, const char *member,
                       const char *sig, const qd_buf *body, const int *fds, int nfds);
/* synchronous call: 0 = method return in *reply, 1 = D-Bus error in *reply, -1 = transport failure/timeout */
int qd_call(qd_conn *c, qd_msg *reply, const char *dest, const char *path, const char *iface, const char *member,
            const char *sig, const qd_buf *body, const int *fds, int nfds, int timeout_ms);
const char *qd_system_address(void);            /* DBUS_SYSTEM_BUS_ADDRESS or the default socket */
#ifdef __cplusplus
}
#endif
#endif /* QDBUS_H */

#ifdef QDBUS_IMPLEMENTATION
#ifndef QDBUS_IMPLEMENTATION_DONE
#define QDBUS_IMPLEMENTATION_DONE
#ifndef _GNU_SOURCE
#error "define _GNU_SOURCE before the first system include when using QDBUS_IMPLEMENTATION"
#endif
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

void qd_put(qd_buf *o, const void *d, size_t n)
{
	if (o->n + n > o->cap) {
		size_t nc = (o->n + n) * 2 + 128;
		unsigned char *nb = realloc(o->b, nc);
		if (!nb) abort();
		o->b = nb; o->cap = nc;
	}
	if (n) memcpy(o->b + o->n, d, n);
	o->n += n;
}
void qd_align(qd_buf *o, size_t a) { static const unsigned char z[8]; while (o->n % a) qd_put(o, z, 1); }
void qd_put_u8(qd_buf *o, unsigned v) { unsigned char c = v; qd_put(o, &c, 1); }
void qd_put_u32(qd_buf *o, uint32_t v) { qd_align(o, 4); qd_put(o, &v, 4); }
void qd_put_u64(qd_buf *o, uint64_t v) { qd_align(o, 8); qd_put(o, &v, 8); }
void qd_put_str(qd_buf *o, const char *s) { uint32_t l = strlen(s); qd_put_u32(o, l); qd_put(o, s, l + 1); }
void qd_put_sig(qd_buf *o, const char *s) { qd_put_u8(o, strlen(s)); qd_put(o, s, strlen(s) + 1); }
qd_arr qd_arr_begin(qd_buf *o, size_t elem_align)
{
	qd_arr a;
	qd_align(o, 4);
	a.lenpos = o->n;
	qd_put_u32(o, 0);
	qd_align(o, elem_align);
	a.start = o->n;
	return a;
}
void qd_arr_end(qd_buf *o, qd_arr a) { uint32_t l = o->n - a.start; memcpy(o->b + a.lenpos, &l, 4); }
void qd_variant_u32(qd_buf *o, uint32_t v) { qd_put_sig(o, "u"); qd_put_u32(o, v); }
void qd_variant_bool(qd_buf *o, int v) { qd_put_sig(o, "b"); qd_put_u32(o, v != 0); }
void qd_variant_u64(qd_buf *o, uint64_t v) { qd_put_sig(o, "t"); qd_put_u64(o, v); }
void qd_variant_str(qd_buf *o, const char *s) { qd_put_sig(o, "s"); qd_put_str(o, s); }
void qd_variant_obj(qd_buf *o, const char *s) { qd_put_sig(o, "o"); qd_put_str(o, s); }

/* ---- reading ---- */
static void qd_ralign(qd_rd *r, size_t a) { while (r->pos % a) r->pos++; }
uint32_t qd_r_u32(qd_rd *r)
{
	uint32_t v = 0;
	qd_ralign(r, 4);
	if (r->pos + 4 > r->len) { r->bad = 1; return 0; }
	memcpy(&v, r->p + r->pos, 4); r->pos += 4;
	return v;
}
uint64_t qd_r_u64(qd_rd *r)
{
	uint64_t v = 0;
	qd_ralign(r, 8);
	if (r->pos + 8 > r->len) { r->bad = 1; return 0; }
	memcpy(&v, r->p + r->pos, 8); r->pos += 8;
	return v;
}
int qd_r_u8(qd_rd *r) { if (r->pos >= r->len) { r->bad = 1; return 0; } return r->p[r->pos++]; }
const char *qd_r_str(qd_rd *r)
{
	uint32_t l = qd_r_u32(r);
	const char *s;
	if (r->bad || (size_t)l + 1 > r->len - r->pos || r->p[r->pos + l] != 0) { r->bad = 1; return ""; }
	s = (const char *)r->p + r->pos; r->pos += l + 1;
	return s;
}
const char *qd_r_sig(qd_rd *r)
{
	int l = qd_r_u8(r);
	const char *s = (const char *)r->p + r->pos;
	if (r->bad || (size_t)l + 1 > r->len - r->pos || r->p[r->pos + l] != 0) { r->bad = 1; return ""; }
	r->pos += l + 1;
	return s;
}
size_t qd_r_array(qd_rd *r, size_t elem_align)
{
	uint32_t l = qd_r_u32(r);
	qd_ralign(r, elem_align);
	if (r->bad || (size_t)l > r->len - r->pos) { r->bad = 1; return r->pos; }
	return r->pos + l;
}
size_t qd_sig_len(const char *s)
{
	const char *t = s;
	switch (*t) {
	case 0: return 0;
	case 'a': return 1 + qd_sig_len(t + 1);
	case '(': case '{': {
		int d = 0;
		do { if (*t == '(' || *t == '{') d++; else if (*t == ')' || *t == '}') d--; t++; } while (*t && d > 0);
		return t - s;
	}
	default: return 1;
	}
}
static size_t qd_type_align(char c)
{
	switch (c) {
	case 'y': case 'g': case 'v': return 1;
	case 'n': case 'q': return 2;
	case 'x': case 't': case 'd': case '(': case '{': return 8;
	default: return 4;
	}
}
void qd_r_skip(qd_rd *r, const char **sig)
{
	char c = **sig;
	size_t tl = qd_sig_len(*sig);
	const char *t = *sig;
	if (!tl) { r->bad = 1; return; }
	*sig += tl;
	if (r->bad) return;
	switch (c) {
	case 'y': r->pos += 1; break;
	case 'n': case 'q': qd_ralign(r, 2); r->pos += 2; break;
	case 'b': case 'u': case 'i': case 'h': qd_ralign(r, 4); r->pos += 4; break;
	case 'x': case 't': case 'd': qd_ralign(r, 8); r->pos += 8; break;
	case 's': case 'o': qd_r_str(r); break;
	case 'g': qd_r_sig(r); break;
	case 'v': { const char *s = qd_r_sig(r); if (!r->bad) qd_r_skip(r, &s); break; }
	case 'a': { size_t end = qd_r_array(r, qd_type_align(t[1])); r->pos = end; break; }
	case '(': case '{': {
		const char *s = t + 1;
		qd_ralign(r, 8);
		while (*s != ')' && *s != '}' && !r->bad) qd_r_skip(r, &s);
		break;
	}
	default: r->bad = 1;
	}
	if (r->pos > r->len) r->bad = 1;
}

/* ---- messages ---- */
void qd_msg_free(qd_msg *m)
{
	for (int i = 0; i < m->nfds; i++) if (m->fds[i] >= 0) close(m->fds[i]);
	free(m->raw);
	memset(m, 0, sizeof *m);
}

static void qd_field(qd_buf *h, int code, char type, const char *s, uint32_t u)
{
	char sig[2] = { type, 0 };
	qd_align(h, 8);
	qd_put_u8(h, code);
	qd_put_sig(h, sig);
	if (type == 'u') qd_put_u32(h, u); else if (type == 'g') qd_put_sig(h, s); else qd_put_str(h, s);
}

static int qd_send_raw(qd_conn *c, const unsigned char *b, size_t n, const int *fds, int nfds)
{
	size_t off = 0;
	while (off < n) {
		struct msghdr mh = {0};
		struct iovec iv = { (void *)(b + off), n - off };
		union { struct cmsghdr h; char buf[CMSG_SPACE(sizeof(int) * QD_MAXFD)]; } cu;
		ssize_t w;
		mh.msg_iov = &iv; mh.msg_iovlen = 1;
		if (off == 0 && nfds > 0) {
			struct cmsghdr *cm;
			memset(&cu, 0, sizeof cu);
			mh.msg_control = cu.buf; mh.msg_controllen = CMSG_SPACE(sizeof(int) * nfds);
			cm = CMSG_FIRSTHDR(&mh);
			cm->cmsg_level = SOL_SOCKET; cm->cmsg_type = SCM_RIGHTS; cm->cmsg_len = CMSG_LEN(sizeof(int) * nfds);
			memcpy(CMSG_DATA(cm), fds, sizeof(int) * nfds);
		}
		w = sendmsg(c->fd, &mh, MSG_NOSIGNAL);
		if (w < 0 && errno == EINTR) continue;
		if (w <= 0) return -1;
		off += w;
	}
	return 0;
}

static uint32_t qd_send_msg(qd_conn *c, int type, int flags, const char *dest, const char *path, const char *iface, const char *member,
                            uint32_t reply_to, const char *err, const char *sig, const qd_buf *body, const int *fds, int nfds)
{
	qd_buf h = {0}, out = {0};
	unsigned char fix[12] = { 'l', (unsigned char)type, (unsigned char)flags, 1 };
	uint32_t bl = body ? body->n : 0, fl, s = c->serial++;
	if (!s) s = c->serial++;
	if (path) qd_field(&h, 1, 'o', path, 0);
	if (iface) qd_field(&h, 2, 's', iface, 0);
	if (member) qd_field(&h, 3, 's', member, 0);
	if (err) qd_field(&h, 4, 's', err, 0);
	if (reply_to) qd_field(&h, 5, 'u', NULL, reply_to);
	if (dest) qd_field(&h, 6, 's', dest, 0);
	if (sig && *sig) qd_field(&h, 8, 'g', sig, 0);
	if (nfds > 0) qd_field(&h, 9, 'u', NULL, nfds);
	fl = h.n;
	memcpy(fix + 4, &bl, 4); memcpy(fix + 8, &s, 4);
	qd_put(&out, fix, 12); qd_put(&out, &fl, 4); qd_put(&out, h.b, h.n);
	qd_align(&out, 8);
	if (body) qd_put(&out, body->b, body->n);
	if (qd_send_raw(c, out.b, out.n, fds, nfds)) s = 0;
	free(h.b); free(out.b);
	return s;
}

uint32_t qd_send_call(qd_conn *c, const char *dest, const char *path, const char *iface, const char *member, const char *sig,
                      const qd_buf *body, const int *fds, int nfds)
{ return qd_send_msg(c, 1, 0, dest, path, iface, member, 0, NULL, sig, body, fds, nfds); }
int qd_send_return(qd_conn *c, const qd_msg *req, const char *sig, const qd_buf *body, const int *fds, int nfds)
{ return qd_send_msg(c, 2, 1, req->sender[0] ? req->sender : NULL, NULL, NULL, NULL, req->serial, NULL, sig, body, fds, nfds) ? 0 : -1; }
int qd_send_error(qd_conn *c, const qd_msg *req, const char *errname, const char *text)
{
	qd_buf b = {0};
	int rc;
	qd_put_str(&b, text ? text : "");
	rc = qd_send_msg(c, 3, 1, req->sender[0] ? req->sender : NULL, NULL, NULL, NULL, req->serial, errname, "s", &b, NULL, 0) ? 0 : -1;
	free(b.b);
	return rc;
}
int qd_send_signal(qd_conn *c, const char *dest, const char *path, const char *iface, const char *member, const char *sig, const qd_buf *body)
{ return qd_send_msg(c, 4, 1, dest, path, iface, member, 0, NULL, sig, body, NULL, 0) ? 0 : -1; }

int qd_send_signal_fds(qd_conn *c, const char *dest, const char *path, const char *iface, const char *member, const char *sig,
                       const qd_buf *body, const int *fds, int nfds)
{ return qd_send_msg(c, 4, 1, dest, path, iface, member, 0, NULL, sig, body, fds, nfds) ? 0 : -1; }

/* ---- receiving ---- */
static int qd_fill(qd_conn *c, int timeout_ms)
{
	struct pollfd p = { c->fd, POLLIN, 0 };
	struct msghdr mh = {0};
	struct iovec iv;
	union { struct cmsghdr h; char buf[CMSG_SPACE(sizeof(int) * QD_MAXFD * 2)]; } cu;
	ssize_t r;
	int pr;
	do pr = poll(&p, 1, timeout_ms); while (pr < 0 && errno == EINTR);
	if (pr == 0) return 0;
	if (pr < 0) return -1;
	if (c->rcap - c->rn < 4096) {
		size_t nc = c->rcap * 2 + 8192;
		unsigned char *nb = realloc(c->rb, nc);
		if (!nb) return -1;
		c->rb = nb; c->rcap = nc;
	}
	iv.iov_base = c->rb + c->rn; iv.iov_len = c->rcap - c->rn;
	mh.msg_iov = &iv; mh.msg_iovlen = 1; mh.msg_control = cu.buf; mh.msg_controllen = sizeof cu.buf;
	r = recvmsg(c->fd, &mh, MSG_CMSG_CLOEXEC);
	if (r < 0 && (errno == EINTR || errno == EAGAIN)) return 0;
	if (r <= 0) return -1;
	c->rn += r;
	for (struct cmsghdr *cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm)) {
		if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
			int n = (cm->cmsg_len - CMSG_LEN(0)) / sizeof(int), fds[QD_MAXFD * 2];
			if (n > QD_MAXFD * 2) n = QD_MAXFD * 2;
			memcpy(fds, CMSG_DATA(cm), n * sizeof(int));
			for (int i = 0; i < n; i++) { if (c->nfdq < 64) c->fdq[c->nfdq++] = fds[i]; else close(fds[i]); }
		}
	}
	return 1;
}

/* try to cut one whole message out of the stream buffer: 1 = got, 0 = need more, -1 = garbage */
static int qd_parse(qd_conn *c, qd_msg *m)
{
	uint32_t fl, bl, ser;
	size_t hlen, total;
	qd_rd r;
	uint32_t nfd = 0;
	if (c->rn < 16) return 0;
	if (c->rb[0] != 'l' || c->rb[3] != 1) return -1;
	memcpy(&bl, c->rb + 4, 4); memcpy(&ser, c->rb + 8, 4); memcpy(&fl, c->rb + 12, 4);
	if (fl > 65536 || bl > QD_MAXBODY) return -1;
	hlen = (16 + (size_t)fl + 7) & ~(size_t)7;
	total = hlen + bl;
	if (c->rn < total) return 0;
	memset(m, 0, sizeof *m);
	m->raw = malloc(total + 1);
	if (!m->raw) return -1;
	memcpy(m->raw, c->rb, total);
	m->type = c->rb[1]; m->flags = c->rb[2]; m->serial = ser; m->blen = bl;
	m->path = m->iface = m->member = m->sender = m->dest = m->err = m->sig = "";
	r = (qd_rd){ m->raw, 16, 16 + fl, 0 };
	while (r.pos < r.len && !r.bad) {
		int code;
		const char *sg;
		qd_ralign(&r, 8);
		if (r.pos >= r.len) break;
		code = qd_r_u8(&r);
		sg = qd_r_sig(&r);
		if (r.bad) break;
		if (*sg == 's' || *sg == 'o') {
			const char *s = qd_r_str(&r);
			switch (code) { case 1: m->path = s; break; case 2: m->iface = s; break; case 3: m->member = s; break;
			case 4: m->err = s; break; case 6: m->dest = s; break; case 7: m->sender = s; break; }
		} else if (*sg == 'u') {
			uint32_t v = qd_r_u32(&r);
			if (code == 5) m->reply_serial = v; else if (code == 9) nfd = v;
		} else if (*sg == 'g') {
			const char *s = qd_r_sig(&r);
			if (code == 8) m->sig = s;
		} else { const char *s = sg; qd_r_skip(&r, &s); }
	}
	m->body = m->raw + hlen;
	m->raw[total] = 0;
	if (r.bad || (m->type == 1 && !*m->path) || nfd > QD_MAXFD) { qd_msg_free(m); return -1; }
	memmove(c->rb, c->rb + total, c->rn - total);
	c->rn -= total;
	for (uint32_t i = 0; i < nfd; i++) {
		if (c->nfdq > 0) { m->fds[m->nfds++] = c->fdq[0]; memmove(c->fdq, c->fdq + 1, --c->nfdq * sizeof(int)); }
	}
	return 1;
}

int qd_recv(qd_conn *c, qd_msg *m, int timeout_ms)
{
	if (c->qhead) {
		qd_msg *q = c->qhead;
		c->qhead = q->next;
		if (!c->qhead) c->qtail = NULL;
		*m = *q; m->next = NULL;
		free(q);
		return 1;
	}
	for (;;) {
		int pr = qd_parse(c, m), f;
		if (pr) return pr;
		f = qd_fill(c, timeout_ms);
		if (f <= 0) return f;
	}
}

int qd_call(qd_conn *c, qd_msg *reply, const char *dest, const char *path, const char *iface, const char *member, const char *sig,
            const qd_buf *body, const int *fds, int nfds, int timeout_ms)
{
	uint32_t s = qd_send_call(c, dest, path, iface, member, sig, body, fds, nfds);
	qd_msg *held = NULL, *tail = NULL;   /* unrelated messages seen meanwhile go back to the FRONT of the queue afterwards */
	int rc = -1;
	if (!s) return -1;
	for (int i = 0; i < 4096; i++) {
		qd_msg m;
		int pr = qd_parse(c, &m);
		if (pr == 0) { pr = qd_fill(c, timeout_ms); if (pr <= 0) break; continue; }
		if (pr < 0) break;
		if ((m.type == 2 || m.type == 3) && m.reply_serial == s) { *reply = m; rc = m.type == 3; break; }
		{
			qd_msg *q = malloc(sizeof *q);
			if (!q) { qd_msg_free(&m); break; }
			*q = m; q->next = NULL;
			if (tail) tail->next = q; else held = q;
			tail = q;
		}
	}
	if (held) { tail->next = c->qhead; c->qhead = held; if (!c->qtail) c->qtail = tail; }
	return rc;
}

/* ---- connect ---- */
const char *qd_system_address(void)
{
	const char *a = getenv("DBUS_SYSTEM_BUS_ADDRESS");
	return a && *a ? a : "unix:path=/run/dbus/system_bus_socket";
}

static int qd_line(int fd, char *line, size_t n)
{
	size_t len = 0;
	while (len + 1 < n && read(fd, line + len, 1) == 1) { len++; if (line[len - 1] == '\n') break; }
	line[len] = 0;
	return len ? 0 : -1;
}

int qd_open(qd_conn *c, const char *addr)
{
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	char hex[64], cred[24], line[256];
	socklen_t sl;
	size_t len;
	int hl = 0;
	qd_msg m;
	memset(c, 0, sizeof *c);
	c->fd = -1; c->serial = 1;
	if (!strncmp(addr, "unix:path=", 10)) {
		len = strcspn(addr + 10, ",");
		if (len >= sizeof a.sun_path) return -1;
		memcpy(a.sun_path, addr + 10, len);
		sl = offsetof(struct sockaddr_un, sun_path) + len + 1;
	} else if (!strncmp(addr, "unix:abstract=", 14)) {
		len = strcspn(addr + 14, ",");
		if (len + 1 >= sizeof a.sun_path) return -1;
		memcpy(a.sun_path + 1, addr + 14, len);
		sl = offsetof(struct sockaddr_un, sun_path) + 1 + len;
	} else return -1;
	if ((c->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0) return -1;
	if (connect(c->fd, (struct sockaddr *)&a, sl) < 0) goto fail;
	snprintf(cred, sizeof cred, "%d", (int)getuid());
	hex[0] = 0;
	for (char *p = cred; *p; p++) hl += snprintf(hex + hl, sizeof hex - hl, "%02x", (unsigned char)*p);
	if (write(c->fd, "\0", 1) != 1) goto fail;
	len = snprintf(line, sizeof line, "AUTH EXTERNAL %s\r\n", hex);
	if (write(c->fd, line, len) < 0 || qd_line(c->fd, line, sizeof line) || strncmp(line, "OK", 2)) goto fail;
	if (write(c->fd, "NEGOTIATE_UNIX_FD\r\n", 19) < 0 || qd_line(c->fd, line, sizeof line) || strncmp(line, "AGREE_UNIX_FD", 13)) goto fail;
	if (write(c->fd, "BEGIN\r\n", 7) < 0) goto fail;
	if (qd_call(c, &m, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "Hello", NULL, NULL, NULL, 0, 3000) != 0) goto fail;
	{ qd_rd r = { m.body, 0, m.blen, 0 }; snprintf(c->name, sizeof c->name, "%s", qd_r_str(&r)); }
	qd_msg_free(&m);
	return 0;
fail:
	close(c->fd); c->fd = -1;
	return -1;
}

void qd_close(qd_conn *c)
{
	while (c->qhead) { qd_msg *q = c->qhead, m; c->qhead = q->next; m = *q; free(q); qd_msg_free(&m); }
	if (c->fd >= 0) close(c->fd);
	for (int i = 0; i < c->nfdq; i++) close(c->fdq[i]);
	free(c->rb);
	memset(c, 0, sizeof *c);
	c->fd = -1;
}
#endif /* QDBUS_IMPLEMENTATION_DONE */
#endif /* QDBUS_IMPLEMENTATION */
