/* dbus.h - the shared half of the iwd / bluez code: a hand-written D-Bus client (no libdbus, no GLib). stb-style, one header.
 *
 *   #define NETMAN_DBUS_IMPLEMENTATION   in exactly one .c file (wifi.h and btth.h define it for you when their own
 *                                        NETMAN_WIFI_IMPLEMENTATION / NETMAN_BT_IMPLEMENTATION is set)
 *
 * What is here: message encode/decode with D-Bus alignment, the system bus (EXTERNAL auth, Hello), a blocking call with timeout,
 * a property store filled from ObjectManager.GetManagedObjects, Properties.Set, and the output streams every module writes to:
 *   netman_out / netman_err / netman_in   NULL = stdout / stderr / stdin (set them to capture output or feed prompts)
 * Env: DBUS_SYSTEM_BUS_ADDRESS (default unix:path=/run/dbus/system_bus_socket).
 * Not thread-safe: static buffers, one connection at a time.
 */
#ifndef NETMAN_DBUS_H
#define NETMAN_DBUS_H
#include <stdio.h>
#ifdef __cplusplus
extern "C" {
#endif
extern FILE *netman_out, *netman_err;   /* NULL = stdout / stderr */
extern FILE *netman_in;                 /* answers to interactive prompts; NULL = stdin */
#ifdef __cplusplus
}
#endif
#endif /* NETMAN_DBUS_H */

#ifdef NETMAN_DBUS_IMPLEMENTATION
#ifndef NETMAN_DBUS_IMPLEMENTATION_DONE
#define NETMAN_DBUS_IMPLEMENTATION_DONE
#ifndef _GNU_SOURCE
#error "define _GNU_SOURCE before the first system include when using NETMAN_*_IMPLEMENTATION"
#endif
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

FILE *netman_out, *netman_err, *netman_in;
#define NETMAN_OUT (netman_out ? netman_out : stdout)
#define NETMAN_ERR (netman_err ? netman_err : stderr)
static void netman_dbus_puts(const char *s) { fputs(s, NETMAN_OUT); fputc('\n', NETMAN_OUT); }

/* ---- byte buffer with D-Bus alignment ---------------------------------------------------------------------------- */

typedef struct { unsigned char *b; size_t n, cap; } netman_dbus_Buf;

static void netman_dbus_put(netman_dbus_Buf *o, const void *d, size_t n)
{
	if (o->n + n > o->cap) { o->cap = (o->n + n) * 2 + 256; o->b = realloc(o->b, o->cap); if (!o->b) exit(1); }
	memcpy(o->b + o->n, d, n);
	o->n += n;
}
static void netman_dbus_align(netman_dbus_Buf *o, size_t a) { static const unsigned char z[8]; while (o->n % a) netman_dbus_put(o, z, 1); }
static void netman_dbus_put_u32(netman_dbus_Buf *o, uint32_t v) { netman_dbus_align(o, 4); netman_dbus_put(o, &v, 4); }
static void netman_dbus_put_str(netman_dbus_Buf *o, const char *s) { uint32_t l = strlen(s); netman_dbus_put_u32(o, l); netman_dbus_put(o, s, l + 1); }
static void netman_dbus_put_sig(netman_dbus_Buf *o, const char *s) { unsigned char l = strlen(s); netman_dbus_put(o, &l, 1); netman_dbus_put(o, s, l + 1); }

/* a header field: struct (BYTE code, VARIANT value) */
static void netman_dbus_field(netman_dbus_Buf *h, int code, char type, const char *sval, uint32_t uval)
{
	unsigned char c = code;
	char sig[2] = { type, 0 };
	netman_dbus_align(h, 8);
	netman_dbus_put(h, &c, 1);
	netman_dbus_put_sig(h, sig);
	if (type == 'u') netman_dbus_put_u32(h, uval); else if (type == 'g') netman_dbus_put_sig(h, sval); else netman_dbus_put_str(h, sval);
}

/* a whole message: type 1 call 2 return 3 error 4 signal */
static void netman_dbus_build(netman_dbus_Buf *out, int type, uint32_t serial, const char *dest, const char *path, const char *iface, const char *member,
                  uint32_t reply_to, const char *err, const char *sig, const netman_dbus_Buf *body)
{
	netman_dbus_Buf h = {0};
	unsigned char fix[12] = { 'l', (unsigned char)type, 0, 1 };
	uint32_t bl = body ? body->n : 0, fl;
	if (path) netman_dbus_field(&h, 1, 'o', path, 0);
	if (iface) netman_dbus_field(&h, 2, 's', iface, 0);
	if (member) netman_dbus_field(&h, 3, 's', member, 0);
	if (err) netman_dbus_field(&h, 4, 's', err, 0);
	if (reply_to) netman_dbus_field(&h, 5, 'u', NULL, reply_to);
	if (dest) netman_dbus_field(&h, 6, 's', dest, 0);
	if (sig && *sig) netman_dbus_field(&h, 8, 'g', sig, 0);
	fl = h.n;
	memcpy(fix + 4, &bl, 4);
	memcpy(fix + 8, &serial, 4);
	out->n = 0;
	netman_dbus_put(out, fix, 12);
	netman_dbus_put(out, &fl, 4);
	netman_dbus_put(out, h.b, h.n);
	netman_dbus_align(out, 8);
	if (body) netman_dbus_put(out, body->b, body->n);
	free(h.b);
}

/* ---- reading ----------------------------------------------------------------------------------------------------- */

typedef struct { const unsigned char *p; size_t pos, len; int bad; } netman_dbus_R;

static void netman_dbus_ralign(netman_dbus_R *r, size_t a) { while (r->pos % a) r->pos++; }
static uint32_t netman_dbus_r_u32(netman_dbus_R *r)
{
	uint32_t v = 0;
	netman_dbus_ralign(r, 4);
	if (r->pos + 4 > r->len) { r->bad = 1; return 0; }
	memcpy(&v, r->p + r->pos, 4);
	r->pos += 4;
	return v;
}
static const char *netman_dbus_r_str(netman_dbus_R *r)
{
	uint32_t l = netman_dbus_r_u32(r);
	const char *s;
	if (r->bad || r->pos + l + 1 > r->len) { r->bad = 1; return ""; }
	s = (const char *)r->p + r->pos;
	r->pos += l + 1;
	return s;
}
static int netman_dbus_r_byte(netman_dbus_R *r)
{
	if (r->pos >= r->len) { r->bad = 1; return 0; }
	return r->p[r->pos++];
}
static const char *netman_dbus_r_sig(netman_dbus_R *r)
{
	int l = netman_dbus_r_byte(r);
	const char *s = (const char *)r->p + r->pos;
	if (r->bad || r->pos + l + 1 > r->len) { r->bad = 1; return ""; }
	r->pos += l + 1;
	return s;
}

/* skip one value of the complete type at *sig, moving *sig past the type */
static void netman_dbus_skip_val(netman_dbus_R *r, const char **sig)
{
	char c = *(*sig)++;
	if (r->bad) return;
	switch (c) {
	case 'y': r->pos += 1; break;
	case 'n': case 'q': netman_dbus_ralign(r, 2); r->pos += 2; break;
	case 'b': case 'u': case 'i': case 'h': netman_dbus_ralign(r, 4); r->pos += 4; break;
	case 'x': case 't': case 'd': netman_dbus_ralign(r, 8); r->pos += 8; break;
	case 's': case 'o': netman_dbus_r_str(r); break;
	case 'g': netman_dbus_r_sig(r); break;
	case 'v': { const char *s = netman_dbus_r_sig(r); netman_dbus_skip_val(r, &s); break; }
	case 'a': {
		uint32_t len = netman_dbus_r_u32(r);
		const char *elem = *sig, *e = *sig;
		size_t al;
		const char *t = elem;
		int depth = 0;
		do {   /* find the end of the element type */
			char k = *t++;
			if (k == '(' || k == '{') depth++;
			else if (k == ')' || k == '}') depth--;
			else if (k == 'a') continue;
		} while (depth > 0 || t[-1] == 'a');
		*sig = t;
		al = (*e == 'y' || *e == 'g' || *e == 'v') ? 1 : (*e == 'n' || *e == 'q') ? 2 : (*e == '(' || *e == '{' || *e == 'x' || *e == 't' || *e == 'd') ? 8 : 4;
		netman_dbus_ralign(r, al);
		r->pos += len;
		break;
	}
	case '(': case '{': {
		netman_dbus_ralign(r, 8);
		while (**sig && **sig != ')' && **sig != '}') netman_dbus_skip_val(r, sig);
		if (**sig) (*sig)++;
		break;
	}
	default: r->bad = 1;
	}
	if (r->pos > r->len) r->bad = 1;
}

typedef struct {
	int type;
	uint32_t serial, blen, reply_serial;
	char path[128], iface[96], member[64], sender[64], dest[64], sig[32], err[96];
	const unsigned char *body;
} netman_dbus_Msg;

static unsigned char *netman_dbus_msgbuf;
static size_t netman_dbus_msgcap;

static int netman_dbus_read_full(int fd, void *dst, size_t n, int timeout_ms)
{
	size_t got = 0;
	while (got < n) {
		struct pollfd p = { fd, POLLIN, 0 };
		ssize_t r;
		if (timeout_ms >= 0 && poll(&p, 1, timeout_ms) <= 0) return -1;
		r = read(fd, (char *)dst + got, n - got);
		if (r <= 0) return -1;
		got += r;
	}
	return 0;
}

/* read one message from fd (blocking, with a per-read timeout when given); 0 on success */
static int netman_dbus_read_msg(int fd, netman_dbus_Msg *m, int timeout_ms)
{
	unsigned char fix[16];
	uint32_t fl;
	size_t hlen, total;
	netman_dbus_R r;
	memset(m, 0, sizeof *m);
	if (netman_dbus_read_full(fd, fix, 16, timeout_ms)) return -1;
	if (fix[0] != 'l') return -1;
	m->type = fix[1];
	memcpy(&m->blen, fix + 4, 4);
	memcpy(&m->serial, fix + 8, 4);
	memcpy(&fl, fix + 12, 4);
	if (fl > 65536 || m->blen > (1u << 20)) return -1;
	hlen = (16 + fl + 7) & ~(size_t)7;
	total = hlen + m->blen;
	if (total > netman_dbus_msgcap && !(netman_dbus_msgbuf = realloc(netman_dbus_msgbuf, netman_dbus_msgcap = total + 1024))) return -1;
	memcpy(netman_dbus_msgbuf, fix, 16);
	if (netman_dbus_read_full(fd, netman_dbus_msgbuf + 16, total - 16, timeout_ms)) return -1;
	r = (netman_dbus_R){ netman_dbus_msgbuf, 16, 16 + fl, 0 };
	while (r.pos < r.len && !r.bad) {
		int code;
		const char *sig;
		netman_dbus_ralign(&r, 8);
		if (r.pos >= r.len) break;
		code = netman_dbus_r_byte(&r);
		sig = netman_dbus_r_sig(&r);
		if (*sig == 's' || *sig == 'o') {
			const char *s = netman_dbus_r_str(&r);
			char *dst = code == 1 ? m->path : code == 2 ? m->iface : code == 3 ? m->member : code == 4 ? m->err : code == 7 ? m->sender : code == 6 ? m->dest : NULL;
			size_t cap = code == 1 ? sizeof m->path : code == 2 ? sizeof m->iface : code == 3 ? sizeof m->member : code == 4 ? sizeof m->err : code == 6 ? sizeof m->dest : sizeof m->sender;
			if (dst) snprintf(dst, cap, "%s", s);
		} else if (*sig == 'u') {
			uint32_t v = netman_dbus_r_u32(&r);
			if (code == 5) m->reply_serial = v;
		} else if (*sig == 'g') {
			const char *s = netman_dbus_r_sig(&r);
			if (code == 8) snprintf(m->sig, sizeof m->sig, "%s", s);
		} else { const char *s = sig; netman_dbus_skip_val(&r, &s); }
	}
	m->body = netman_dbus_msgbuf + hlen;
	return r.bad ? -1 : 0;
}

/* ---- connection -------------------------------------------------------------------------------------------------- */

static uint32_t netman_dbus_serial_no = 1;

static int netman_dbus_connect_bus(const char *addr)
{
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	char hex[64] = "", cred[32], line[256];
	size_t len;
	int fd, hl = 0;
	if (!strncmp(addr, "unix:path=", 10)) {
		len = strcspn(addr + 10, ",");
		if (len >= sizeof a.sun_path) return -1;
		memcpy(a.sun_path, addr + 10, len);
	} else if (!strncmp(addr, "unix:abstract=", 14)) {
		len = strcspn(addr + 14, ",");
		if (len + 1 >= sizeof a.sun_path) return -1;
		memcpy(a.sun_path + 1, addr + 14, len);
		len++;
	} else return -1;
	if ((fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0) return -1;
	if (connect(fd, (struct sockaddr *)&a, offsetof(struct sockaddr_un, sun_path) + (a.sun_path[0] ? strlen(a.sun_path) : len) + 1 - (a.sun_path[0] ? 0 : 0)) < 0) {
		close(fd);
		return -1;
	}
	snprintf(cred, sizeof cred, "%d", (int)getuid());
	for (char *c = cred; *c; c++) hl += snprintf(hex + hl, sizeof hex - hl, "%02x", (unsigned char)*c);
	if (write(fd, "\0", 1) != 1) { close(fd); return -1; }
	len = snprintf(line, sizeof line, "AUTH EXTERNAL %s\r\n", hex);
	if (write(fd, line, len) < 0) { close(fd); return -1; }
	len = 0;   /* read the reply line byte by byte so nothing of the message stream is consumed */
	while (len + 1 < sizeof line && read(fd, line + len, 1) == 1) { len++; if (line[len - 1] == '\n') break; }
	line[len] = 0;
	if (strncmp(line, "OK", 2)) { close(fd); return -1; }
	if (write(fd, "BEGIN\r\n", 7) < 0) { close(fd); return -1; }
	return fd;
}

static void netman_dbus_send_msg(int fd, const netman_dbus_Buf *m)
{
	size_t off = 0;
	while (off < m->n) {
		ssize_t w = write(fd, m->b + off, m->n - off);
		if (w <= 0) return;
		off += w;
	}
}

/* call a method and wait for its return/error. body (may be NULL) has the signature sig. returns 0 on success; the reply is in msgbuf/m */
static int netman_dbus_call_timeout = 3000;
static const char *netman_dbus_agent_pass, *netman_dbus_agent_user;   /* iwd credential agent: passphrase, and the identity for 802.1X */
static int (*netman_dbus_hook)(int fd, const netman_dbus_Msg *m);   /* set by btth.h while its pairing agent is registered; returns 1 when it consumed the call */
static void netman_dbus_reply_to_call(int fd, const netman_dbus_Msg *req, const char *sig, const netman_dbus_Buf *body);

static int netman_dbus_call(int fd, netman_dbus_Msg *m, const char *dest, const char *path, const char *iface, const char *member, const char *sig, const netman_dbus_Buf *body)
{
	netman_dbus_Buf out = {0};
	uint32_t s = netman_dbus_serial_no++;
	netman_dbus_build(&out, 1, s, dest, path, iface, member, 0, NULL, sig, body);
	netman_dbus_send_msg(fd, &out);
	free(out.b);
	for (int i = 0; i < 1024; i++) {
		if (netman_dbus_read_msg(fd, m, netman_dbus_call_timeout)) return -1;
		if (m->type == 1 && netman_dbus_hook && netman_dbus_hook(fd, m)) continue;
		if (m->type == 1 && netman_dbus_agent_pass) {   /* iwd asks our agent while a call is pending */
			netman_dbus_Buf b = {0};
			if (!strcmp(m->member, "RequestPassphrase") || !strcmp(m->member, "RequestPrivateKeyPassphrase") || !strcmp(m->member, "RequestUserPassword")) { netman_dbus_put_str(&b, netman_dbus_agent_pass); netman_dbus_reply_to_call(fd, m, "s", &b); }
			else if (!strcmp(m->member, "RequestUserNameAndPassword")) { netman_dbus_put_str(&b, netman_dbus_agent_user ? netman_dbus_agent_user : ""); netman_dbus_put_str(&b, netman_dbus_agent_pass); netman_dbus_reply_to_call(fd, m, "ss", &b); }
			else netman_dbus_reply_to_call(fd, m, NULL, NULL);
			free(b.b);
			continue;
		}
		if ((m->type == 2 || m->type == 3) && m->reply_serial == s) return m->type == 3;
	}
	return -1;
}

static int netman_dbus_bus_hello(int fd)
{
	netman_dbus_Msg m;
	return netman_dbus_call(fd, &m, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "Hello", NULL, NULL);
}

static void netman_dbus_reply_to_call(int fd, const netman_dbus_Msg *req, const char *sig, const netman_dbus_Buf *body)
{
	netman_dbus_Buf out = {0};
	netman_dbus_build(&out, 2, netman_dbus_serial_no++, req->sender, NULL, NULL, NULL, req->serial, NULL, sig, body);
	netman_dbus_send_msg(fd, &out);
	free(out.b);
}

/* ---- property store filled from ObjectManager.GetManagedObjects --------------------------------------------------- */

typedef struct { char path[160], iface[56], key[40], val[640]; } netman_dbus_Prop;
static netman_dbus_Prop *netman_dbus_props;
static int netman_dbus_nprops, netman_dbus_capprops;

static void netman_dbus_addprop(const char *path, const char *iface, const char *key, const char *val)
{
	netman_dbus_Prop *p;
	if (netman_dbus_nprops == netman_dbus_capprops && !(netman_dbus_props = realloc(netman_dbus_props, (netman_dbus_capprops = netman_dbus_capprops * 2 + 256) * sizeof *netman_dbus_props))) exit(1);
	p = &netman_dbus_props[netman_dbus_nprops++];
	snprintf(p->path, sizeof p->path, "%s", path); snprintf(p->iface, sizeof p->iface, "%s", iface);
	snprintf(p->key, sizeof p->key, "%s", key); snprintf(p->val, sizeof p->val, "%s", val);
}

/* read one variant and render the simple ones as text; always consumes the variant */
static void netman_dbus_variant_str(netman_dbus_R *r, char *out, size_t n)
{
	const char *sig = netman_dbus_r_sig(r), *t = sig;
	out[0] = 0;
	switch (*sig) {
	case 's': case 'o': snprintf(out, n, "%s", netman_dbus_r_str(r)); return;
	case 'b': snprintf(out, n, "%s", netman_dbus_r_u32(r) ? "true" : "false"); return;
	case 'y': snprintf(out, n, "%d", netman_dbus_r_byte(r)); return;
	case 'u': snprintf(out, n, "%u", netman_dbus_r_u32(r)); return;
	case 'i': snprintf(out, n, "%d", (int)netman_dbus_r_u32(r)); return;
	case 'n': { int16_t v = 0; netman_dbus_ralign(r, 2); if (r->pos + 2 <= r->len) memcpy(&v, r->p + r->pos, 2); r->pos += 2; snprintf(out, n, "%d", v); return; }
	case 'a':
		if (sig[1] == 's' || sig[1] == 'o') {
			uint32_t len = netman_dbus_r_u32(r);
			size_t end = r->pos + len, o = 0;
			while (!r->bad && r->pos < end) {
				const char *e = netman_dbus_r_str(r);
				o += snprintf(out + o, o < n ? n - o : 0, "%s%s", o ? "," : "", e);
			}
			return;
		}
	}
	netman_dbus_skip_val(r, &t);
}

static int netman_dbus_load_objects(int fd, const char *dest)
{
	netman_dbus_Msg m;
	netman_dbus_R r;
	uint32_t len;
	size_t end;
	netman_dbus_nprops = 0;
	if (netman_dbus_call(fd, &m, dest, "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", NULL, NULL)) return -1;
	r = (netman_dbus_R){ m.body, 0, m.blen, 0 };
	len = netman_dbus_r_u32(&r); netman_dbus_ralign(&r, 8); end = r.pos + len;
	while (!r.bad && r.pos < end) {
		const char *path;
		uint32_t ilen;
		size_t iend;
		netman_dbus_ralign(&r, 8);
		path = netman_dbus_r_str(&r);
		ilen = netman_dbus_r_u32(&r); netman_dbus_ralign(&r, 8); iend = r.pos + ilen;
		while (!r.bad && r.pos < iend) {
			const char *iface;
			uint32_t plen;
			size_t pend;
			netman_dbus_ralign(&r, 8);
			iface = netman_dbus_r_str(&r);
			plen = netman_dbus_r_u32(&r); netman_dbus_ralign(&r, 8); pend = r.pos + plen;
			netman_dbus_addprop(path, iface, "", "");   /* marks the interface as present */
			while (!r.bad && r.pos < pend) {
				const char *key;
				char val[640];
				netman_dbus_ralign(&r, 8);
				key = netman_dbus_r_str(&r);
				netman_dbus_variant_str(&r, val, sizeof val);
				netman_dbus_addprop(path, iface, key, val);
			}
		}
	}
	return r.bad ? -1 : 0;
}

static const char *netman_dbus_pget(const char *path, const char *iface, const char *key)
{
	for (int i = 0; i < netman_dbus_nprops; i++)
		if (!strcmp(netman_dbus_props[i].path, path) && !strcmp(netman_dbus_props[i].iface, iface) && !strcmp(netman_dbus_props[i].key, key)) return netman_dbus_props[i].val;
	return NULL;
}

static const char *netman_dbus_first_path(const char *iface)
{
	for (int i = 0; i < netman_dbus_nprops; i++) if (!strcmp(netman_dbus_props[i].iface, iface) && !*netman_dbus_props[i].key) return netman_dbus_props[i].path;
	return NULL;
}

static int netman_dbus_fail(const netman_dbus_Msg *m, const char *what)
{
	const char *detail = "";
	netman_dbus_R r = { m->body, 0, m->blen, 0 };
	if (m->type == 3 && m->sig[0] == 's') detail = netman_dbus_r_str(&r);
	fprintf(NETMAN_ERR, "%s: %s%s%s\n", what, m->err[0] ? m->err : "no answer", *detail ? ": " : "", detail);
	return 1;
}

static int netman_dbus_simple_call(int fd, const char *dest, const char *path, const char *iface, const char *member, const char *sig, const netman_dbus_Buf *body, const char *what)
{
	netman_dbus_Msg m;
	int rc = netman_dbus_call(fd, &m, dest, path, iface, member, sig, body);
	return rc ? netman_dbus_fail(&m, what) : 0;
}

static int netman_dbus_system_bus(void)
{
	const char *a = getenv("DBUS_SYSTEM_BUS_ADDRESS");
	int fd = netman_dbus_connect_bus(a && *a ? a : "unix:path=/run/dbus/system_bus_socket");
	if (fd < 0 || netman_dbus_bus_hello(fd)) { fputs("netman: cannot reach the system bus\n", NETMAN_ERR); return -1; }
	return fd;
}

/* Properties.Set with a bool (str == NULL) or string variant */
static int netman_dbus_set_var(int fd, const char *dest, const char *path, const char *iface, const char *key, int v, const char *str)
{
	netman_dbus_Buf b = {0};
	int rc;
	netman_dbus_put_str(&b, iface); netman_dbus_put_str(&b, key);
	if (str) { netman_dbus_put_sig(&b, "s"); netman_dbus_put_str(&b, str); } else { netman_dbus_put_sig(&b, "b"); netman_dbus_put_u32(&b, v); }
	rc = netman_dbus_simple_call(fd, dest, path, "org.freedesktop.DBus.Properties", "Set", "ssv", &b, key);
	free(b.b);
	return rc;
}

#endif /* NETMAN_DBUS_IMPLEMENTATION_DONE */
#endif /* NETMAN_DBUS_IMPLEMENTATION */
