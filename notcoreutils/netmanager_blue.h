/* netmanager_blue.h - stb-style single-header network + bluetooth manager. No libs: talks D-Bus itself to iwd and bluez,
 * wpa_supplicant's control socket, reads sysfs, execs nft for the firewall.
 *
 *   #define NMB_IMPLEMENTATION      in exactly ONE .c file before including this header
 *   #include "netmanager_blue.h"
 *
 * Every call returns 0 on success, 1 on failure (the reason goes to nmb_err), 2 when the request does not make sense for the
 * backend (e.g. forget under wpa_supplicant). Human-readable results go to nmb_out. Both default to stdout/stderr; set the
 * FILE * variables (NULL = default) to capture them. Not thread-safe: one call at a time (static buffers, one D-Bus socket).
 *
 * wifi backend: wpa_supplicant if /run/wpa_supplicant/$NMB_IFACE (default wlan0) exists or is unreadable (needs root or netdev),
 * otherwise iwd. Bluetooth: bluez. Not done: WPS, VPN, ethernet configuration. nmb_status lists ethernet links read-only.
 */
#ifndef NETMANAGER_BLUE_H
#define NETMANAGER_BLUE_H

#include <stdio.h>

#ifndef NMB_API
#define NMB_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

extern FILE *nmb_out, *nmb_err;   /* NULL = stdout / stderr */

NMB_API int nmb_status(void);                                     /* links (eth+wifi, IP, carrier), wifi state, bluetooth adapter */
NMB_API int nmb_wifi_backend_is_wpa(void);                        /* 1 when wpa_supplicant owns the interface, 0 for iwd */
NMB_API int nmb_wifi_list(void);                                  /* scan and list visible networks */
NMB_API int nmb_wifi_connect(const char *ssid, const char *pass); /* pass NULL = open or already known network */
NMB_API int nmb_wifi_disconnect(void);
NMB_API int nmb_wifi_forget(const char *ssid);                    /* iwd only; returns 2 under wpa_supplicant */
NMB_API int nmb_bt_list(void);
NMB_API int nmb_bt_scan(int seconds);                             /* discover for N seconds (<=0 means 5), then list */
NMB_API int nmb_bt_action(const char *action, const char *name_or_mac); /* connect|disconnect|pair|trust|remove; 2 for an unknown action */
NMB_API int nmb_bt_power(int on);
NMB_API int nmb_fw(void);                                         /* exec "nft list ruleset"; returns only if that fails */

#ifdef __cplusplus
}
#endif
#endif /* NETMANAGER_BLUE_H */

#ifdef NMB_IMPLEMENTATION
#ifndef NMB_IMPLEMENTATION_ONCE
#define NMB_IMPLEMENTATION_ONCE

#ifndef _GNU_SOURCE
#error "define _GNU_SOURCE before the first system include when using NMB_IMPLEMENTATION"
#endif
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <strings.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

FILE *nmb_out, *nmb_err;
#define NMB_OUT (nmb_out ? nmb_out : stdout)
#define NMB_ERR (nmb_err ? nmb_err : stderr)
static void nmb__puts(const char *s) { fputs(s, NMB_OUT); fputc('\n', NMB_OUT); }

/* ---- byte buffer with D-Bus alignment ---------------------------------------------------------------------------- */

typedef struct { unsigned char *b; size_t n, cap; } nmb__Buf;

static void nmb__put(nmb__Buf *o, const void *d, size_t n)
{
	if (o->n + n > o->cap) { o->cap = (o->n + n) * 2 + 256; o->b = realloc(o->b, o->cap); if (!o->b) exit(1); }
	memcpy(o->b + o->n, d, n);
	o->n += n;
}
static void nmb__align(nmb__Buf *o, size_t a) { static const unsigned char z[8]; while (o->n % a) nmb__put(o, z, 1); }
static void nmb__put_u32(nmb__Buf *o, uint32_t v) { nmb__align(o, 4); nmb__put(o, &v, 4); }
static void nmb__put_str(nmb__Buf *o, const char *s) { uint32_t l = strlen(s); nmb__put_u32(o, l); nmb__put(o, s, l + 1); }
static void nmb__put_sig(nmb__Buf *o, const char *s) { unsigned char l = strlen(s); nmb__put(o, &l, 1); nmb__put(o, s, l + 1); }

/* a header field: struct (BYTE code, VARIANT value) */
static void nmb__field(nmb__Buf *h, int code, char type, const char *sval, uint32_t uval)
{
	unsigned char c = code;
	char sig[2] = { type, 0 };
	nmb__align(h, 8);
	nmb__put(h, &c, 1);
	nmb__put_sig(h, sig);
	if (type == 'u') nmb__put_u32(h, uval); else if (type == 'g') nmb__put_sig(h, sval); else nmb__put_str(h, sval);
}

/* a whole message: type 1 call 2 return 3 error 4 signal */
static void nmb__build(nmb__Buf *out, int type, uint32_t serial, const char *dest, const char *path, const char *iface, const char *member,
                  uint32_t reply_to, const char *err, const char *sig, const nmb__Buf *body)
{
	nmb__Buf h = {0};
	unsigned char fix[12] = { 'l', (unsigned char)type, 0, 1 };
	uint32_t bl = body ? body->n : 0, fl;
	if (path) nmb__field(&h, 1, 'o', path, 0);
	if (iface) nmb__field(&h, 2, 's', iface, 0);
	if (member) nmb__field(&h, 3, 's', member, 0);
	if (err) nmb__field(&h, 4, 's', err, 0);
	if (reply_to) nmb__field(&h, 5, 'u', NULL, reply_to);
	if (dest) nmb__field(&h, 6, 's', dest, 0);
	if (sig && *sig) nmb__field(&h, 8, 'g', sig, 0);
	fl = h.n;
	memcpy(fix + 4, &bl, 4);
	memcpy(fix + 8, &serial, 4);
	out->n = 0;
	nmb__put(out, fix, 12);
	nmb__put(out, &fl, 4);
	nmb__put(out, h.b, h.n);
	nmb__align(out, 8);
	if (body) nmb__put(out, body->b, body->n);
	free(h.b);
}

/* ---- reading ----------------------------------------------------------------------------------------------------- */

typedef struct { const unsigned char *p; size_t pos, len; int bad; } nmb__R;

static void nmb__ralign(nmb__R *r, size_t a) { while (r->pos % a) r->pos++; }
static uint32_t nmb__r_u32(nmb__R *r)
{
	uint32_t v = 0;
	nmb__ralign(r, 4);
	if (r->pos + 4 > r->len) { r->bad = 1; return 0; }
	memcpy(&v, r->p + r->pos, 4);
	r->pos += 4;
	return v;
}
static const char *nmb__r_str(nmb__R *r)
{
	uint32_t l = nmb__r_u32(r);
	const char *s;
	if (r->bad || r->pos + l + 1 > r->len) { r->bad = 1; return ""; }
	s = (const char *)r->p + r->pos;
	r->pos += l + 1;
	return s;
}
static int nmb__r_byte(nmb__R *r)
{
	if (r->pos >= r->len) { r->bad = 1; return 0; }
	return r->p[r->pos++];
}
static const char *nmb__r_sig(nmb__R *r)
{
	int l = nmb__r_byte(r);
	const char *s = (const char *)r->p + r->pos;
	if (r->bad || r->pos + l + 1 > r->len) { r->bad = 1; return ""; }
	r->pos += l + 1;
	return s;
}

/* skip one value of the complete type at *sig, moving *sig past the type */
static void nmb__skip_val(nmb__R *r, const char **sig)
{
	char c = *(*sig)++;
	if (r->bad) return;
	switch (c) {
	case 'y': r->pos += 1; break;
	case 'n': case 'q': nmb__ralign(r, 2); r->pos += 2; break;
	case 'b': case 'u': case 'i': case 'h': nmb__ralign(r, 4); r->pos += 4; break;
	case 'x': case 't': case 'd': nmb__ralign(r, 8); r->pos += 8; break;
	case 's': case 'o': nmb__r_str(r); break;
	case 'g': nmb__r_sig(r); break;
	case 'v': { const char *s = nmb__r_sig(r); nmb__skip_val(r, &s); break; }
	case 'a': {
		uint32_t len = nmb__r_u32(r);
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
		nmb__ralign(r, al);
		r->pos += len;
		break;
	}
	case '(': case '{': {
		nmb__ralign(r, 8);
		while (**sig && **sig != ')' && **sig != '}') nmb__skip_val(r, sig);
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
	char path[128], iface[96], member[64], sender[64], sig[32], err[96];
	const unsigned char *body;
} nmb__Msg;

static unsigned char *nmb__msgbuf;
static size_t nmb__msgcap;

static int nmb__read_full(int fd, void *dst, size_t n, int timeout_ms)
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
static int nmb__read_msg(int fd, nmb__Msg *m, int timeout_ms)
{
	unsigned char fix[16];
	uint32_t fl;
	size_t hlen, total;
	nmb__R r;
	memset(m, 0, sizeof *m);
	if (nmb__read_full(fd, fix, 16, timeout_ms)) return -1;
	if (fix[0] != 'l') return -1;
	m->type = fix[1];
	memcpy(&m->blen, fix + 4, 4);
	memcpy(&m->serial, fix + 8, 4);
	memcpy(&fl, fix + 12, 4);
	if (fl > 65536 || m->blen > (1u << 20)) return -1;
	hlen = (16 + fl + 7) & ~(size_t)7;
	total = hlen + m->blen;
	if (total > nmb__msgcap && !(nmb__msgbuf = realloc(nmb__msgbuf, nmb__msgcap = total + 1024))) return -1;
	memcpy(nmb__msgbuf, fix, 16);
	if (nmb__read_full(fd, nmb__msgbuf + 16, total - 16, timeout_ms)) return -1;
	r = (nmb__R){ nmb__msgbuf, 16, 16 + fl, 0 };
	while (r.pos < r.len && !r.bad) {
		int code;
		const char *sig;
		nmb__ralign(&r, 8);
		if (r.pos >= r.len) break;
		code = nmb__r_byte(&r);
		sig = nmb__r_sig(&r);
		if (*sig == 's' || *sig == 'o') {
			const char *s = nmb__r_str(&r);
			char *dst = code == 1 ? m->path : code == 2 ? m->iface : code == 3 ? m->member : code == 4 ? m->err : code == 7 ? m->sender : NULL;
			size_t cap = code == 1 ? sizeof m->path : code == 2 ? sizeof m->iface : code == 3 ? sizeof m->member : code == 4 ? sizeof m->err : sizeof m->sender;
			if (dst) snprintf(dst, cap, "%s", s);
		} else if (*sig == 'u') {
			uint32_t v = nmb__r_u32(&r);
			if (code == 5) m->reply_serial = v;
		} else if (*sig == 'g') {
			const char *s = nmb__r_sig(&r);
			if (code == 8) snprintf(m->sig, sizeof m->sig, "%s", s);
		} else { const char *s = sig; nmb__skip_val(&r, &s); }
	}
	m->body = nmb__msgbuf + hlen;
	return r.bad ? -1 : 0;
}

/* ---- connection -------------------------------------------------------------------------------------------------- */

static uint32_t nmb__serial_no = 1;

static int nmb__connect_bus(const char *addr)
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

static void nmb__send_msg(int fd, const nmb__Buf *m)
{
	size_t off = 0;
	while (off < m->n) {
		ssize_t w = write(fd, m->b + off, m->n - off);
		if (w <= 0) return;
		off += w;
	}
}

/* call a method and wait for its return/error. body (may be NULL) has the signature sig. returns 0 on success; the reply is in msgbuf/m */
static int nmb__call_timeout = 3000;
static const char *nmb__agent_pass;
static void nmb__reply_to_call(int fd, const nmb__Msg *req, const char *sig, const nmb__Buf *body);

static int nmb__call(int fd, nmb__Msg *m, const char *dest, const char *path, const char *iface, const char *member, const char *sig, const nmb__Buf *body)
{
	nmb__Buf out = {0};
	uint32_t s = nmb__serial_no++;
	nmb__build(&out, 1, s, dest, path, iface, member, 0, NULL, sig, body);
	nmb__send_msg(fd, &out);
	free(out.b);
	for (int i = 0; i < 64; i++) {
		if (nmb__read_msg(fd, m, nmb__call_timeout)) return -1;
		if (m->type == 1 && nmb__agent_pass) {   /* iwd asks our agent while a call is pending */
			nmb__Buf b = {0};
			if (!strcmp(m->member, "RequestPassphrase")) { nmb__put_str(&b, nmb__agent_pass); nmb__reply_to_call(fd, m, "s", &b); }
			else nmb__reply_to_call(fd, m, NULL, NULL);
			free(b.b);
			continue;
		}
		if ((m->type == 2 || m->type == 3) && m->reply_serial == s) return m->type == 3;
	}
	return -1;
}

static int nmb__bus_hello(int fd)
{
	nmb__Msg m;
	return nmb__call(fd, &m, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "Hello", NULL, NULL);
}

static void nmb__reply_to_call(int fd, const nmb__Msg *req, const char *sig, const nmb__Buf *body)
{
	nmb__Buf out = {0};
	nmb__build(&out, 2, nmb__serial_no++, req->sender, NULL, NULL, NULL, req->serial, NULL, sig, body);
	nmb__send_msg(fd, &out);
	free(out.b);
}

/* ---- property store filled from ObjectManager.GetManagedObjects --------------------------------------------------- */

typedef struct { char path[160], iface[56], key[40], val[200]; } nmb__Prop;
static nmb__Prop *nmb__props;
static int nmb__nprops, nmb__capprops;

static void nmb__addprop(const char *path, const char *iface, const char *key, const char *val)
{
	nmb__Prop *p;
	if (nmb__nprops == nmb__capprops && !(nmb__props = realloc(nmb__props, (nmb__capprops = nmb__capprops * 2 + 256) * sizeof *nmb__props))) exit(1);
	p = &nmb__props[nmb__nprops++];
	snprintf(p->path, sizeof p->path, "%s", path); snprintf(p->iface, sizeof p->iface, "%s", iface);
	snprintf(p->key, sizeof p->key, "%s", key); snprintf(p->val, sizeof p->val, "%s", val);
}

/* read one variant and render the simple ones as text; always consumes the variant */
static void nmb__variant_str(nmb__R *r, char *out, size_t n)
{
	const char *sig = nmb__r_sig(r), *t = sig;
	out[0] = 0;
	switch (*sig) {
	case 's': case 'o': snprintf(out, n, "%s", nmb__r_str(r)); return;
	case 'b': snprintf(out, n, "%s", nmb__r_u32(r) ? "true" : "false"); return;
	case 'y': snprintf(out, n, "%d", nmb__r_byte(r)); return;
	case 'u': snprintf(out, n, "%u", nmb__r_u32(r)); return;
	case 'i': snprintf(out, n, "%d", (int)nmb__r_u32(r)); return;
	case 'n': { int16_t v = 0; nmb__ralign(r, 2); if (r->pos + 2 <= r->len) memcpy(&v, r->p + r->pos, 2); r->pos += 2; snprintf(out, n, "%d", v); return; }
	case 'a':
		if (sig[1] == 's') {
			uint32_t len = nmb__r_u32(r);
			size_t end = r->pos + len, o = 0;
			while (!r->bad && r->pos < end) {
				const char *e = nmb__r_str(r);
				o += snprintf(out + o, o < n ? n - o : 0, "%s%s", o ? "," : "", e);
			}
			return;
		}
	}
	nmb__skip_val(r, &t);
}

static int nmb__load_objects(int fd, const char *dest)
{
	nmb__Msg m;
	nmb__R r;
	uint32_t len;
	size_t end;
	nmb__nprops = 0;
	if (nmb__call(fd, &m, dest, "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", NULL, NULL)) return -1;
	r = (nmb__R){ m.body, 0, m.blen, 0 };
	len = nmb__r_u32(&r); nmb__ralign(&r, 8); end = r.pos + len;
	while (!r.bad && r.pos < end) {
		const char *path;
		uint32_t ilen;
		size_t iend;
		nmb__ralign(&r, 8);
		path = nmb__r_str(&r);
		ilen = nmb__r_u32(&r); nmb__ralign(&r, 8); iend = r.pos + ilen;
		while (!r.bad && r.pos < iend) {
			const char *iface;
			uint32_t plen;
			size_t pend;
			nmb__ralign(&r, 8);
			iface = nmb__r_str(&r);
			plen = nmb__r_u32(&r); nmb__ralign(&r, 8); pend = r.pos + plen;
			nmb__addprop(path, iface, "", "");   /* marks the interface as present */
			while (!r.bad && r.pos < pend) {
				const char *key;
				char val[200];
				nmb__ralign(&r, 8);
				key = nmb__r_str(&r);
				nmb__variant_str(&r, val, sizeof val);
				nmb__addprop(path, iface, key, val);
			}
		}
	}
	return r.bad ? -1 : 0;
}

static const char *nmb__pget(const char *path, const char *iface, const char *key)
{
	for (int i = 0; i < nmb__nprops; i++)
		if (!strcmp(nmb__props[i].path, path) && !strcmp(nmb__props[i].iface, iface) && !strcmp(nmb__props[i].key, key)) return nmb__props[i].val;
	return NULL;
}

static const char *nmb__first_path(const char *iface)
{
	for (int i = 0; i < nmb__nprops; i++) if (!strcmp(nmb__props[i].iface, iface) && !*nmb__props[i].key) return nmb__props[i].path;
	return NULL;
}

static int nmb__fail(const nmb__Msg *m, const char *what)
{
	const char *detail = "";
	nmb__R r = { m->body, 0, m->blen, 0 };
	if (m->type == 3 && m->sig[0] == 's') detail = nmb__r_str(&r);
	fprintf(NMB_ERR, "%s: %s%s%s\n", what, m->err[0] ? m->err : "no answer", *detail ? ": " : "", detail);
	return 1;
}

static int nmb__simple_call(int fd, const char *dest, const char *path, const char *iface, const char *member, const char *sig, const nmb__Buf *body, const char *what)
{
	nmb__Msg m;
	int rc = nmb__call(fd, &m, dest, path, iface, member, sig, body);
	return rc ? nmb__fail(&m, what) : 0;
}

static int nmb__system_bus(void)
{
	const char *a = getenv("DBUS_SYSTEM_BUS_ADDRESS");
	int fd = nmb__connect_bus(a && *a ? a : "unix:path=/run/dbus/system_bus_socket");
	if (fd < 0 || nmb__bus_hello(fd)) { fputs("netmanager_blue: cannot reach the system bus\n", NMB_ERR); return -1; }
	return fd;
}

/* ---- wifi through wpa_supplicant's control socket (when it owns the interface instead of iwd) ------------------------ */

/* ---- wifi through wpa_supplicant's control socket (when it owns the interface instead of iwd) ------------------------ */

static char nmb__wpa_dir[] = "/run/wpa_supplicant";

static int nmb__wpa_cmd(const char *ifname, const char *cmd, char *out, size_t n)
{
	struct sockaddr_un to = { .sun_family = AF_UNIX }, me = { .sun_family = AF_UNIX };
	int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	struct pollfd p = { fd, POLLIN, 0 };
	ssize_t r;
	if (fd < 0) return -1;
	snprintf(to.sun_path, sizeof to.sun_path, "%s/%s", nmb__wpa_dir, ifname);
	snprintf(me.sun_path, sizeof me.sun_path, "/tmp/nmb-%d", (int)getpid());
	unlink(me.sun_path);
	if (bind(fd, (struct sockaddr *)&me, sizeof me) < 0 || connect(fd, (struct sockaddr *)&to, sizeof to) < 0 || send(fd, cmd, strlen(cmd), 0) < 0) {
		close(fd); unlink(me.sun_path);
		return -1;
	}
	r = poll(&p, 1, 10000) > 0 ? recv(fd, out, n - 1, 0) : -1;
	close(fd); unlink(me.sun_path);
	if (r < 0) return -1;
	out[r] = 0;
	return 0;
}

static int nmb__wpa_present(const char *ifname)
{
	char p[300];
	snprintf(p, sizeof p, "%s/%s", nmb__wpa_dir, ifname);
	return access(p, F_OK) == 0 || errno == EACCES;
}

static const char *nmb__wpa_iface(void) { return getenv("NMB_IFACE") ?: "wlan0"; }

static int nmb__wpa_list(const char *ifname)
{
	static char out[16384];
	char *l;
	nmb__wpa_cmd(ifname, "SCAN", out, sizeof out);
	sleep(4);
	if (nmb__wpa_cmd(ifname, "SCAN_RESULTS", out, sizeof out)) { fputs("wifi: cannot talk to wpa_supplicant (needs root or the netdev group)\n", NMB_ERR); return 1; }
	for (l = strtok(out, "\n"); l; l = strtok(NULL, "\n")) {   /* bssid \t freq \t signal \t flags \t ssid */
		char *f[5] = {0}, *x = l;
		int k = 0;
		for (; k < 5; k++) { f[k] = x; if (!(x = strchr(x, '\t'))) break; *x++ = 0; }
		if (k < 4 || !strcmp(f[0], "bssid / frequency / signal level / flags / ssid")) continue;
		fprintf(NMB_OUT, "  %-32s %-4s %4s dBm\n", f[4] ?: "", strstr(f[3], "WPA") || strstr(f[3], "RSN") ? "psk" : "open", f[2]);
	}
	return 0;
}

static int nmb__wpa_connect(const char *ifname, const char *ssid, const char *pass)
{
	static char out[16384];
	char cmd[300], id[16];
	if (nmb__wpa_cmd(ifname, "ADD_NETWORK", out, sizeof out)) { fputs("wifi: cannot talk to wpa_supplicant\n", NMB_ERR); return 1; }
	snprintf(id, sizeof id, "%d", atoi(out));
	snprintf(cmd, sizeof cmd, "SET_NETWORK %s ssid \"%s\"", id, ssid); nmb__wpa_cmd(ifname, cmd, out, sizeof out);
	if (pass) snprintf(cmd, sizeof cmd, "SET_NETWORK %s psk \"%s\"", id, pass);
	else snprintf(cmd, sizeof cmd, "SET_NETWORK %s key_mgmt NONE", id);
	nmb__wpa_cmd(ifname, cmd, out, sizeof out);
	if (strncmp(out, "OK", 2)) { fprintf(NMB_ERR, "wifi: wpa_supplicant refused it (%s) - passphrase must be 8-63 chars\n", out); return 1; }
	snprintf(cmd, sizeof cmd, "SELECT_NETWORK %s", id); nmb__wpa_cmd(ifname, cmd, out, sizeof out);
	nmb__wpa_cmd(ifname, "SAVE_CONFIG", out, sizeof out);
	fprintf(NMB_OUT, "selected %s (check: netmanager_blue status)\n", ssid);
	return 0;
}

/* ---- wifi (iwd) -------------------------------------------------------------------------------------------------- */

#define nmb__IWD "net.connman.iwd"

static int nmb__iwd_load(int fd)
{
	if (nmb__load_objects(fd, nmb__IWD)) { fputs("wifi: iwd is not running (systemctl start iwd)\n", NMB_ERR); return -1; }
	return 0;
}

static int nmb__wifi_scan(int fd, const char *station)
{
	if (nmb__simple_call(fd, nmb__IWD, station, nmb__IWD ".Station", "Scan", NULL, NULL, "scan")) return -1;
	for (int i = 0; i < 20; i++) {
		struct timespec ts = { 0, 500000000 };
		nanosleep(&ts, NULL);
		if (nmb__iwd_load(fd)) return -1;
		if (strcmp(nmb__pget(station, nmb__IWD ".Station", "Scanning") ?: "false", "true")) break;
	}
	return 0;
}

static const char *nmb__net_by_name(const char *name)
{
	for (int i = 0; i < nmb__nprops; i++)
		if (!strcmp(nmb__props[i].iface, nmb__IWD ".Network") && !strcmp(nmb__props[i].key, "Name") && !strcmp(nmb__props[i].val, name)) return nmb__props[i].path;
	return NULL;
}

/* connect to the system bus and load iwd's objects; *st = the station path. Returns the fd or -1 (message printed). */
static int nmb__iwd_open(const char **st)
{
	int fd = nmb__system_bus();
	if (fd < 0) return -1;
	signal(SIGPIPE, SIG_IGN);
	if (nmb__iwd_load(fd)) { close(fd); return -1; }
	if (!(*st = nmb__first_path(nmb__IWD ".Station"))) { fputs("wifi: no wifi device in station mode\n", NMB_ERR); close(fd); return -1; }
	return fd;
}

static int nmb__iwd_list(int fd, const char *st)
{
	nmb__Msg m;
	nmb__R r;
	uint32_t len;
	size_t end;
	const char *conn;
	if (nmb__wifi_scan(fd, st)) return 1;
	conn = nmb__pget(st, nmb__IWD ".Station", "ConnectedNetwork");
	if (nmb__call(fd, &m, nmb__IWD, st, nmb__IWD ".Station", "GetOrderedNetworks", NULL, NULL)) return nmb__fail(&m, "list");
	r = (nmb__R){ m.body, 0, m.blen, 0 };
	len = nmb__r_u32(&r); nmb__ralign(&r, 8); end = r.pos + len;
	while (!r.bad && r.pos < end) {
		char path[160];
		int16_t sig = 0;
		const char *name, *type;
		nmb__ralign(&r, 8);
		snprintf(path, sizeof path, "%s", nmb__r_str(&r));
		nmb__ralign(&r, 2);
		if (r.pos + 2 <= r.len) memcpy(&sig, r.p + r.pos, 2);
		r.pos += 2;
		name = nmb__pget(path, nmb__IWD ".Network", "Name");
		type = nmb__pget(path, nmb__IWD ".Network", "Type");
		fprintf(NMB_OUT, "%c %-32s %-6s %4d dBm%s\n", conn && !strcmp(conn, path) ? '*' : ' ', name ?: "?", type ?: "?", sig / 100,
		       nmb__pget(path, nmb__IWD ".Network", "KnownNetwork") ? "  (known)" : "");
	}
	return 0;
}

static int nmb__iwd_connect(int fd, const char *st, const char *ssid, const char *pass)
{
	const char *net = nmb__net_by_name(ssid);
	int rc;
	if (!net) { if (nmb__wifi_scan(fd, st)) return 1; net = nmb__net_by_name(ssid); }
	if (!net) { fprintf(NMB_ERR, "wifi: no network called %s in range\n", ssid); return 1; }
	if (pass) {
		nmb__Buf b = {0};
		nmb__agent_pass = pass;
		nmb__put_str(&b, "/nmb/agent");
		rc = nmb__simple_call(fd, nmb__IWD, "/net/connman/iwd", "net.connman.iwd.AgentManager", "RegisterAgent", "o", &b, "agent");
		free(b.b);
		if (rc) return 1;
	}
	nmb__call_timeout = 40000;
	rc = nmb__simple_call(fd, nmb__IWD, net, nmb__IWD ".Network", "Connect", NULL, NULL, "connect");
	if (!rc) fprintf(NMB_OUT, "connected to %s\n", ssid);
	return rc;
}

static int nmb__iwd_forget(int fd, const char *ssid)
{
	const char *net = nmb__net_by_name(ssid), *k = net ? nmb__pget(net, nmb__IWD ".Network", "KnownNetwork") : NULL;
	if (!k) { fputs("wifi: that network is not known\n", NMB_ERR); return 1; }
	return nmb__simple_call(fd, nmb__IWD, k, nmb__IWD ".KnownNetwork", "Forget", NULL, NULL, "forget");
}

/* ---- bluetooth (bluez) ------------------------------------------------------------------------------------------- */

#define nmb__BZ "org.bluez"
#define nmb__DEV nmb__BZ ".Device1"
#define nmb__ADP nmb__BZ ".Adapter1"

static int nmb__bz_load(int fd)
{
	if (nmb__load_objects(fd, nmb__BZ) || !nmb__first_path(nmb__ADP)) { fputs("bluetooth: bluez is not running or there is no adapter\n", NMB_ERR); return -1; }
	return 0;
}

static void nmb__bt_print(void)
{
	for (int i = 0; i < nmb__nprops; i++) {
		const char *path = nmb__props[i].path;
		if (strcmp(nmb__props[i].iface, nmb__DEV) || *nmb__props[i].key) continue;
		fprintf(NMB_OUT, "%c%c%c %s  %s  %s dBm\n", !strcmp(nmb__pget(path, nmb__DEV, "Connected") ?: "", "true") ? 'C' : '-', !strcmp(nmb__pget(path, nmb__DEV, "Paired") ?: "", "true") ? 'P' : '-',
		       !strcmp(nmb__pget(path, nmb__DEV, "Trusted") ?: "", "true") ? 'T' : '-', nmb__pget(path, nmb__DEV, "Address") ?: "?", nmb__pget(path, nmb__DEV, "Alias") ?: "?",
		       nmb__pget(path, nmb__DEV, "RSSI") ?: "  -");
	}
}

static const char *nmb__bt_find(const char *what)
{
	for (int i = 0; i < nmb__nprops; i++) {
		const char *a, *n;
		if (strcmp(nmb__props[i].iface, nmb__DEV) || *nmb__props[i].key) continue;
		a = nmb__pget(nmb__props[i].path, nmb__DEV, "Address"); n = nmb__pget(nmb__props[i].path, nmb__DEV, "Alias");
		if ((a && !strcasecmp(a, what)) || (n && strcasestr(n, what))) return nmb__props[i].path;
	}
	return NULL;
}

static int nmb__set_bool(int fd, const char *path, const char *iface, const char *key, int v)
{
	nmb__Buf b = {0};
	int rc;
	nmb__put_str(&b, iface); nmb__put_str(&b, key); nmb__put_sig(&b, "b"); nmb__put_u32(&b, v);
	rc = nmb__simple_call(fd, nmb__BZ, path, "org.freedesktop.DBus.Properties", "Set", "ssv", &b, key);
	free(b.b);
	return rc;
}

/* system bus + bluez objects; *ad = adapter path. Returns the fd or -1 (message printed). */
static int nmb__bz_open(const char **ad)
{
	int fd = nmb__system_bus();
	if (fd < 0) return -1;
	signal(SIGPIPE, SIG_IGN);
	if (nmb__bz_load(fd)) { close(fd); return -1; }
	*ad = nmb__first_path(nmb__ADP);
	return fd;
}

/* ---- status ------------------------------------------------------------------------------------------------------ */

static void nmb__slurp(const char *path, char *out, size_t n)
{
	FILE *f = fopen(path, "r");
	out[0] = 0;
	if (f) { if (fgets(out, n, f)) out[strcspn(out, "\n")] = 0; fclose(f); }
}

/* ---- public API -------------------------------------------------------------------------------------------------- */

int nmb_wifi_backend_is_wpa(void) { return nmb__wpa_present(nmb__wpa_iface()); }

int nmb_wifi_list(void)
{
	const char *st;
	int fd, rc;
	if (nmb__wpa_present(nmb__wpa_iface())) return nmb__wpa_list(nmb__wpa_iface());
	if ((fd = nmb__iwd_open(&st)) < 0) return 1;
	rc = nmb__iwd_list(fd, st);
	close(fd);
	return rc;
}

int nmb_wifi_connect(const char *ssid, const char *pass)
{
	const char *st;
	int fd, rc;
	if (nmb__wpa_present(nmb__wpa_iface())) return nmb__wpa_connect(nmb__wpa_iface(), ssid, pass);
	if ((fd = nmb__iwd_open(&st)) < 0) return 1;
	rc = nmb__iwd_connect(fd, st, ssid, pass);
	close(fd);
	return rc;
}

int nmb_wifi_disconnect(void)
{
	const char *st;
	int fd, rc;
	if (nmb__wpa_present(nmb__wpa_iface())) { char out[256]; nmb__wpa_cmd(nmb__wpa_iface(), "DISCONNECT", out, sizeof out); return 0; }
	if ((fd = nmb__iwd_open(&st)) < 0) return 1;
	rc = nmb__simple_call(fd, nmb__IWD, st, nmb__IWD ".Station", "Disconnect", NULL, NULL, "disconnect");
	close(fd);
	return rc;
}

int nmb_wifi_forget(const char *ssid)
{
	const char *st;
	int fd, rc;
	if (nmb__wpa_present(nmb__wpa_iface())) return 2;   /* wpa_supplicant backend has no forget */
	if ((fd = nmb__iwd_open(&st)) < 0) return 1;
	rc = nmb__iwd_forget(fd, ssid);
	close(fd);
	return rc;
}

int nmb_bt_list(void)
{
	const char *ad;
	int fd = nmb__bz_open(&ad);
	if (fd < 0) return 1;
	nmb__bt_print();
	close(fd);
	return 0;
}

int nmb_bt_scan(int secs)
{
	const char *ad;
	int fd = nmb__bz_open(&ad);
	if (fd < 0) return 1;
	if (nmb__simple_call(fd, nmb__BZ, ad, nmb__ADP, "StartDiscovery", NULL, NULL, "scan")) { close(fd); return 1; }
	sleep(secs > 0 ? secs : 5);
	nmb__simple_call(fd, nmb__BZ, ad, nmb__ADP, "StopDiscovery", NULL, NULL, "scan");
	if (nmb__bz_load(fd)) { close(fd); return 1; }
	nmb__bt_print();
	close(fd);
	return 0;
}

int nmb_bt_power(int on)
{
	const char *ad;
	int fd = nmb__bz_open(&ad), rc;
	if (fd < 0) return 1;
	rc = nmb__set_bool(fd, ad, nmb__ADP, "Powered", on);
	close(fd);
	return rc;
}

int nmb_bt_action(const char *action, const char *what)
{
	const char *ad, *dev;
	int fd = nmb__bz_open(&ad), rc = 2;
	if (fd < 0) return 1;
	if (!(dev = nmb__bt_find(what))) { fprintf(NMB_ERR, "bluetooth: no device matching %s (try: bt scan)\n", what); close(fd); return 1; }
	nmb__call_timeout = 30000;
	if (!strcmp(action, "connect")) rc = nmb__simple_call(fd, nmb__BZ, dev, nmb__DEV, "Connect", NULL, NULL, "connect");
	else if (!strcmp(action, "disconnect")) rc = nmb__simple_call(fd, nmb__BZ, dev, nmb__DEV, "Disconnect", NULL, NULL, "disconnect");
	else if (!strcmp(action, "pair")) rc = nmb__simple_call(fd, nmb__BZ, dev, nmb__DEV, "Pair", NULL, NULL, "pair");
	else if (!strcmp(action, "trust")) rc = nmb__set_bool(fd, dev, nmb__DEV, "Trusted", 1);
	else if (!strcmp(action, "remove")) {
		nmb__Buf b = {0};
		nmb__put_str(&b, dev);
		rc = nmb__simple_call(fd, nmb__BZ, ad, nmb__ADP, "RemoveDevice", "o", &b, "remove");
		free(b.b);
	}
	close(fd);
	return rc;
}

int nmb_status(void)
{
	DIR *d = opendir("/sys/class/net");
	struct dirent *e;
	struct ifaddrs *ifa, *a;
	int fd;
	if (d) while ((e = readdir(d))) {
		char p[300], state[32], mac[32], carrier[8], ip[INET_ADDRSTRLEN] = "";
		if (e->d_name[0] == '.' || !strcmp(e->d_name, "lo")) continue;
		snprintf(p, sizeof p, "/sys/class/net/%s/operstate", e->d_name); nmb__slurp(p, state, sizeof state);
		snprintf(p, sizeof p, "/sys/class/net/%s/address", e->d_name); nmb__slurp(p, mac, sizeof mac);
		snprintf(p, sizeof p, "/sys/class/net/%s/carrier", e->d_name); nmb__slurp(p, carrier, sizeof carrier);
		if (!getifaddrs(&ifa)) {
			for (a = ifa; a; a = a->ifa_next)
				if (a->ifa_addr && a->ifa_addr->sa_family == AF_INET && !strcmp(a->ifa_name, e->d_name))
					inet_ntop(AF_INET, &((struct sockaddr_in *)a->ifa_addr)->sin_addr, ip, sizeof ip);
			freeifaddrs(ifa);
		}
		snprintf(p, sizeof p, "/sys/class/net/%s/wireless", e->d_name);
		fprintf(NMB_OUT, "%-8s %-5s %-10s %-17s %s%s\n", e->d_name, access(p, F_OK) ? "eth" : "wifi", state, mac, ip, !strcmp(carrier, "0") ? "  no cable/link" : "");
	}
	if (d) closedir(d);
	if ((fd = nmb__system_bus()) < 0) return 0;
	if (!nmb__load_objects(fd, nmb__IWD)) {
		const char *st = nmb__first_path(nmb__IWD ".Station"), *c = st ? nmb__pget(st, nmb__IWD ".Station", "ConnectedNetwork") : NULL;
		if (st) fprintf(NMB_OUT, "wifi     %s%s%s\n", nmb__pget(st, nmb__IWD ".Station", "State") ?: "?", c ? " to " : "", c ? nmb__pget(c, nmb__IWD ".Network", "Name") ?: "" : "");
	} else nmb__puts("wifi     iwd not running");
	if (nmb__wpa_present("wlan0")) nmb__puts("wifi     wlan0 is driven by wpa_supplicant (not iwd)");
	if (!nmb__load_objects(fd, nmb__BZ) && nmb__first_path(nmb__ADP)) {
		int n = 0;
		const char *ad = nmb__first_path(nmb__ADP);
		for (int i = 0; i < nmb__nprops; i++)
			if (!strcmp(nmb__props[i].iface, nmb__DEV) && !*nmb__props[i].key && !strcmp(nmb__pget(nmb__props[i].path, nmb__DEV, "Connected") ?: "", "true")) {
				fprintf(NMB_OUT, "bt       connected: %s\n", nmb__pget(nmb__props[i].path, nmb__DEV, "Alias") ?: "?");
				n++;
			}
		fprintf(NMB_OUT, "bt       adapter %s%s\n", !strcmp(nmb__pget(ad, nmb__ADP, "Powered") ?: "", "true") ? "on" : "off", n ? "" : ", nothing connected");
	} else nmb__puts("bt       bluez not running or no adapter");
	close(fd);
	return 0;
}

int nmb_fw(void)
{
	execlp("nft", "nft", "list", "ruleset", (char *)NULL);
	perror("nft");
	return 1;
}

#endif /* NMB_IMPLEMENTATION_ONCE */
#endif /* NMB_IMPLEMENTATION */
