/* netmanager_blue - small network + bluetooth manager in one C file. No libs: talks D-Bus itself to iwd and bluez, reads sysfs.
 *
 *   netmanager_blue                 status of ethernet, wifi and bluetooth
 *   netmanager_blue wifi list       scan and list visible networks (signal, security, known, connected)
 *   netmanager_blue wifi connect SSID [PASSPHRASE]
 *   netmanager_blue wifi disconnect
 *   netmanager_blue wifi forget SSID
 *   netmanager_blue bt list         known and discovered devices
 *   netmanager_blue bt scan [SEC]   discover for SEC seconds (default 5)
 *   netmanager_blue bt connect|disconnect|pair|trust|remove NAME_OR_MAC
 *   netmanager_blue bt power on|off
 *   netmanager_blue fw              firewall ruleset (nft)
 * wifi needs iwd (the backend that does WPA), bluetooth needs bluez; both are the normal Linux daemons.
 * Not done yet: WPS, VPN, ethernet configuration, the TUI.
 */
#define _GNU_SOURCE
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ---- byte buffer with D-Bus alignment ---------------------------------------------------------------------------- */

typedef struct { unsigned char *b; size_t n, cap; } Buf;

static void put(Buf *o, const void *d, size_t n)
{
	if (o->n + n > o->cap) { o->cap = (o->n + n) * 2 + 256; o->b = realloc(o->b, o->cap); if (!o->b) exit(1); }
	memcpy(o->b + o->n, d, n);
	o->n += n;
}
static void align(Buf *o, size_t a) { static const unsigned char z[8]; while (o->n % a) put(o, z, 1); }
static void put_u32(Buf *o, uint32_t v) { align(o, 4); put(o, &v, 4); }
static void put_str(Buf *o, const char *s) { uint32_t l = strlen(s); put_u32(o, l); put(o, s, l + 1); }
static void put_sig(Buf *o, const char *s) { unsigned char l = strlen(s); put(o, &l, 1); put(o, s, l + 1); }

/* a header field: struct (BYTE code, VARIANT value) */
static void field(Buf *h, int code, char type, const char *sval, uint32_t uval)
{
	unsigned char c = code;
	char sig[2] = { type, 0 };
	align(h, 8);
	put(h, &c, 1);
	put_sig(h, sig);
	if (type == 'u') put_u32(h, uval); else if (type == 'g') put_sig(h, sval); else put_str(h, sval);
}

/* a whole message: type 1 call 2 return 3 error 4 signal */
static void build(Buf *out, int type, uint32_t serial, const char *dest, const char *path, const char *iface, const char *member,
                  uint32_t reply_to, const char *err, const char *sig, const Buf *body)
{
	Buf h = {0};
	unsigned char fix[12] = { 'l', (unsigned char)type, 0, 1 };
	uint32_t bl = body ? body->n : 0, fl;
	if (path) field(&h, 1, 'o', path, 0);
	if (iface) field(&h, 2, 's', iface, 0);
	if (member) field(&h, 3, 's', member, 0);
	if (err) field(&h, 4, 's', err, 0);
	if (reply_to) field(&h, 5, 'u', NULL, reply_to);
	if (dest) field(&h, 6, 's', dest, 0);
	if (sig && *sig) field(&h, 8, 'g', sig, 0);
	fl = h.n;
	memcpy(fix + 4, &bl, 4);
	memcpy(fix + 8, &serial, 4);
	out->n = 0;
	put(out, fix, 12);
	put(out, &fl, 4);
	put(out, h.b, h.n);
	align(out, 8);
	if (body) put(out, body->b, body->n);
	free(h.b);
}

/* ---- reading ----------------------------------------------------------------------------------------------------- */

typedef struct { const unsigned char *p; size_t pos, len; int bad; } R;

static void ralign(R *r, size_t a) { while (r->pos % a) r->pos++; }
static uint32_t r_u32(R *r)
{
	uint32_t v = 0;
	ralign(r, 4);
	if (r->pos + 4 > r->len) { r->bad = 1; return 0; }
	memcpy(&v, r->p + r->pos, 4);
	r->pos += 4;
	return v;
}
static const char *r_str(R *r)
{
	uint32_t l = r_u32(r);
	const char *s;
	if (r->bad || r->pos + l + 1 > r->len) { r->bad = 1; return ""; }
	s = (const char *)r->p + r->pos;
	r->pos += l + 1;
	return s;
}
static int r_byte(R *r)
{
	if (r->pos >= r->len) { r->bad = 1; return 0; }
	return r->p[r->pos++];
}
static const char *r_sig(R *r)
{
	int l = r_byte(r);
	const char *s = (const char *)r->p + r->pos;
	if (r->bad || r->pos + l + 1 > r->len) { r->bad = 1; return ""; }
	r->pos += l + 1;
	return s;
}

/* skip one value of the complete type at *sig, moving *sig past the type */
static void skip_val(R *r, const char **sig)
{
	char c = *(*sig)++;
	if (r->bad) return;
	switch (c) {
	case 'y': r->pos += 1; break;
	case 'n': case 'q': ralign(r, 2); r->pos += 2; break;
	case 'b': case 'u': case 'i': case 'h': ralign(r, 4); r->pos += 4; break;
	case 'x': case 't': case 'd': ralign(r, 8); r->pos += 8; break;
	case 's': case 'o': r_str(r); break;
	case 'g': r_sig(r); break;
	case 'v': { const char *s = r_sig(r); skip_val(r, &s); break; }
	case 'a': {
		uint32_t len = r_u32(r);
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
		ralign(r, al);
		r->pos += len;
		break;
	}
	case '(': case '{': {
		ralign(r, 8);
		while (**sig && **sig != ')' && **sig != '}') skip_val(r, sig);
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
} Msg;

static unsigned char *msgbuf;
static size_t msgcap;

static int read_full(int fd, void *dst, size_t n, int timeout_ms)
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
static int read_msg(int fd, Msg *m, int timeout_ms)
{
	unsigned char fix[16];
	uint32_t fl;
	size_t hlen, total;
	R r;
	memset(m, 0, sizeof *m);
	if (read_full(fd, fix, 16, timeout_ms)) return -1;
	if (fix[0] != 'l') return -1;
	m->type = fix[1];
	memcpy(&m->blen, fix + 4, 4);
	memcpy(&m->serial, fix + 8, 4);
	memcpy(&fl, fix + 12, 4);
	if (fl > 65536 || m->blen > (1u << 20)) return -1;
	hlen = (16 + fl + 7) & ~(size_t)7;
	total = hlen + m->blen;
	if (total > msgcap && !(msgbuf = realloc(msgbuf, msgcap = total + 1024))) return -1;
	memcpy(msgbuf, fix, 16);
	if (read_full(fd, msgbuf + 16, total - 16, timeout_ms)) return -1;
	r = (R){ msgbuf, 16, 16 + fl, 0 };
	while (r.pos < r.len && !r.bad) {
		int code;
		const char *sig;
		ralign(&r, 8);
		if (r.pos >= r.len) break;
		code = r_byte(&r);
		sig = r_sig(&r);
		if (*sig == 's' || *sig == 'o') {
			const char *s = r_str(&r);
			char *dst = code == 1 ? m->path : code == 2 ? m->iface : code == 3 ? m->member : code == 4 ? m->err : code == 7 ? m->sender : NULL;
			size_t cap = code == 1 ? sizeof m->path : code == 2 ? sizeof m->iface : code == 3 ? sizeof m->member : code == 4 ? sizeof m->err : sizeof m->sender;
			if (dst) snprintf(dst, cap, "%s", s);
		} else if (*sig == 'u') {
			uint32_t v = r_u32(&r);
			if (code == 5) m->reply_serial = v;
		} else if (*sig == 'g') {
			const char *s = r_sig(&r);
			if (code == 8) snprintf(m->sig, sizeof m->sig, "%s", s);
		} else { const char *s = sig; skip_val(&r, &s); }
	}
	m->body = msgbuf + hlen;
	return r.bad ? -1 : 0;
}

/* ---- connection -------------------------------------------------------------------------------------------------- */

static uint32_t serial_no = 1;

static int connect_bus(const char *addr)
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

static void send_msg(int fd, const Buf *m)
{
	size_t off = 0;
	while (off < m->n) {
		ssize_t w = write(fd, m->b + off, m->n - off);
		if (w <= 0) return;
		off += w;
	}
}

/* call a method and wait for its return/error. body (may be NULL) has the signature sig. returns 0 on success; the reply is in msgbuf/m */
static int call_timeout = 3000;
static const char *agent_pass;
static void reply_to_call(int fd, const Msg *req, const char *sig, const Buf *body);

static int call(int fd, Msg *m, const char *dest, const char *path, const char *iface, const char *member, const char *sig, const Buf *body)
{
	Buf out = {0};
	uint32_t s = serial_no++;
	build(&out, 1, s, dest, path, iface, member, 0, NULL, sig, body);
	send_msg(fd, &out);
	free(out.b);
	for (int i = 0; i < 64; i++) {
		if (read_msg(fd, m, call_timeout)) return -1;
		if (m->type == 1 && agent_pass) {   /* iwd asks our agent while a call is pending */
			Buf b = {0};
			if (!strcmp(m->member, "RequestPassphrase")) { put_str(&b, agent_pass); reply_to_call(fd, m, "s", &b); }
			else reply_to_call(fd, m, NULL, NULL);
			free(b.b);
			continue;
		}
		if ((m->type == 2 || m->type == 3) && m->reply_serial == s) return m->type == 3;
	}
	return -1;
}

static int bus_hello(int fd)
{
	Msg m;
	return call(fd, &m, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "Hello", NULL, NULL);
}

static void reply_to_call(int fd, const Msg *req, const char *sig, const Buf *body)
{
	Buf out = {0};
	build(&out, 2, serial_no++, req->sender, NULL, NULL, NULL, req->serial, NULL, sig, body);
	send_msg(fd, &out);
	free(out.b);
}

/* ---- property store filled from ObjectManager.GetManagedObjects --------------------------------------------------- */

typedef struct { char path[160], iface[56], key[40], val[200]; } Prop;
static Prop *props;
static int nprops, capprops;

static void addprop(const char *path, const char *iface, const char *key, const char *val)
{
	Prop *p;
	if (nprops == capprops && !(props = realloc(props, (capprops = capprops * 2 + 256) * sizeof *props))) exit(1);
	p = &props[nprops++];
	snprintf(p->path, sizeof p->path, "%s", path); snprintf(p->iface, sizeof p->iface, "%s", iface);
	snprintf(p->key, sizeof p->key, "%s", key); snprintf(p->val, sizeof p->val, "%s", val);
}

/* read one variant and render the simple ones as text; always consumes the variant */
static void variant_str(R *r, char *out, size_t n)
{
	const char *sig = r_sig(r), *t = sig;
	out[0] = 0;
	switch (*sig) {
	case 's': case 'o': snprintf(out, n, "%s", r_str(r)); return;
	case 'b': snprintf(out, n, "%s", r_u32(r) ? "true" : "false"); return;
	case 'y': snprintf(out, n, "%d", r_byte(r)); return;
	case 'u': snprintf(out, n, "%u", r_u32(r)); return;
	case 'i': snprintf(out, n, "%d", (int)r_u32(r)); return;
	case 'n': { int16_t v = 0; ralign(r, 2); if (r->pos + 2 <= r->len) memcpy(&v, r->p + r->pos, 2); r->pos += 2; snprintf(out, n, "%d", v); return; }
	case 'a':
		if (sig[1] == 's') {
			uint32_t len = r_u32(r);
			size_t end = r->pos + len, o = 0;
			while (!r->bad && r->pos < end) {
				const char *e = r_str(r);
				o += snprintf(out + o, o < n ? n - o : 0, "%s%s", o ? "," : "", e);
			}
			return;
		}
	}
	skip_val(r, &t);
}

static int load_objects(int fd, const char *dest)
{
	Msg m;
	R r;
	uint32_t len;
	size_t end;
	nprops = 0;
	if (call(fd, &m, dest, "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", NULL, NULL)) return -1;
	r = (R){ m.body, 0, m.blen, 0 };
	len = r_u32(&r); ralign(&r, 8); end = r.pos + len;
	while (!r.bad && r.pos < end) {
		const char *path;
		uint32_t ilen;
		size_t iend;
		ralign(&r, 8);
		path = r_str(&r);
		ilen = r_u32(&r); ralign(&r, 8); iend = r.pos + ilen;
		while (!r.bad && r.pos < iend) {
			const char *iface;
			uint32_t plen;
			size_t pend;
			ralign(&r, 8);
			iface = r_str(&r);
			plen = r_u32(&r); ralign(&r, 8); pend = r.pos + plen;
			addprop(path, iface, "", "");   /* marks the interface as present */
			while (!r.bad && r.pos < pend) {
				const char *key;
				char val[200];
				ralign(&r, 8);
				key = r_str(&r);
				variant_str(&r, val, sizeof val);
				addprop(path, iface, key, val);
			}
		}
	}
	return r.bad ? -1 : 0;
}

static const char *pget(const char *path, const char *iface, const char *key)
{
	for (int i = 0; i < nprops; i++)
		if (!strcmp(props[i].path, path) && !strcmp(props[i].iface, iface) && !strcmp(props[i].key, key)) return props[i].val;
	return NULL;
}

static const char *first_path(const char *iface)
{
	for (int i = 0; i < nprops; i++) if (!strcmp(props[i].iface, iface) && !*props[i].key) return props[i].path;
	return NULL;
}

static int fail(const Msg *m, const char *what)
{
	const char *detail = "";
	R r = { m->body, 0, m->blen, 0 };
	if (m->type == 3 && m->sig[0] == 's') detail = r_str(&r);
	fprintf(stderr, "%s: %s%s%s\n", what, m->err[0] ? m->err : "no answer", *detail ? ": " : "", detail);
	return 1;
}

static int simple_call(int fd, const char *dest, const char *path, const char *iface, const char *member, const char *sig, const Buf *body, const char *what)
{
	Msg m;
	int rc = call(fd, &m, dest, path, iface, member, sig, body);
	return rc ? fail(&m, what) : 0;
}

static int system_bus(void)
{
	const char *a = getenv("DBUS_SYSTEM_BUS_ADDRESS");
	int fd = connect_bus(a && *a ? a : "unix:path=/run/dbus/system_bus_socket");
	if (fd < 0 || bus_hello(fd)) { fputs("netmanager_blue: cannot reach the system bus\n", stderr); return -1; }
	return fd;
}

/* ---- wifi through wpa_supplicant's control socket (when it owns the interface instead of iwd) ------------------------ */

static char wpa_dir[] = "/run/wpa_supplicant";

static int wpa_cmd(const char *ifname, const char *cmd, char *out, size_t n)
{
	struct sockaddr_un to = { .sun_family = AF_UNIX }, me = { .sun_family = AF_UNIX };
	int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	struct pollfd p = { fd, POLLIN, 0 };
	ssize_t r;
	if (fd < 0) return -1;
	snprintf(to.sun_path, sizeof to.sun_path, "%s/%s", wpa_dir, ifname);
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

static int wpa_present(const char *ifname)
{
	char p[300];
	snprintf(p, sizeof p, "%s/%s", wpa_dir, ifname);
	return access(p, F_OK) == 0 || errno == EACCES;
}

static int wpa_main(const char *ifname, int argc, char **argv)
{
	const char *sub = argc > 2 ? argv[2] : "list";
	static char out[16384];
	if (!strcmp(sub, "list")) {
		char *l;
		wpa_cmd(ifname, "SCAN", out, sizeof out);
		sleep(4);
		if (wpa_cmd(ifname, "SCAN_RESULTS", out, sizeof out)) { fputs("wifi: cannot talk to wpa_supplicant (needs root or the netdev group)\n", stderr); return 1; }
		for (l = strtok(out, "\n"); l; l = strtok(NULL, "\n")) {   /* bssid \t freq \t signal \t flags \t ssid */
			char *f[5] = {0}, *x = l;
			int k = 0;
			for (; k < 5; k++) { f[k] = x; if (!(x = strchr(x, '\t'))) break; *x++ = 0; }
			if (k < 4 || !strcmp(f[0], "bssid / frequency / signal level / flags / ssid")) continue;
			printf("  %-32s %-4s %4s dBm\n", f[4] ?: "", strstr(f[3], "WPA") || strstr(f[3], "RSN") ? "psk" : "open", f[2]);
		}
		return 0;
	}
	if (!strcmp(sub, "disconnect")) { wpa_cmd(ifname, "DISCONNECT", out, sizeof out); return 0; }
	if (!strcmp(sub, "connect") && argc > 3) {
		char cmd[300], id[16];
		if (wpa_cmd(ifname, "ADD_NETWORK", out, sizeof out)) { fputs("wifi: cannot talk to wpa_supplicant\n", stderr); return 1; }
		snprintf(id, sizeof id, "%d", atoi(out));
		snprintf(cmd, sizeof cmd, "SET_NETWORK %s ssid \"%s\"", id, argv[3]); wpa_cmd(ifname, cmd, out, sizeof out);
		if (argc > 4) snprintf(cmd, sizeof cmd, "SET_NETWORK %s psk \"%s\"", id, argv[4]);
		else snprintf(cmd, sizeof cmd, "SET_NETWORK %s key_mgmt NONE", id);
		wpa_cmd(ifname, cmd, out, sizeof out);
		if (strncmp(out, "OK", 2)) { fprintf(stderr, "wifi: wpa_supplicant refused it (%s) - passphrase must be 8-63 chars\n", out); return 1; }
		snprintf(cmd, sizeof cmd, "SELECT_NETWORK %s", id); wpa_cmd(ifname, cmd, out, sizeof out);
		wpa_cmd(ifname, "SAVE_CONFIG", out, sizeof out);
		printf("selected %s (check: netmanager_blue status)\n", argv[3]);
		return 0;
	}
	fputs("usage: wifi list | connect SSID [PASSPHRASE] | disconnect\n", stderr);
	return 2;
}

/* ---- wifi (iwd) -------------------------------------------------------------------------------------------------- */

#define IWD "net.connman.iwd"

static int iwd_load(int fd)
{
	if (load_objects(fd, IWD)) { fputs("wifi: iwd is not running (systemctl start iwd)\n", stderr); return -1; }
	return 0;
}

static int wifi_scan(int fd, const char *station)
{
	if (simple_call(fd, IWD, station, IWD ".Station", "Scan", NULL, NULL, "scan")) return -1;
	for (int i = 0; i < 20; i++) {
		struct timespec ts = { 0, 500000000 };
		nanosleep(&ts, NULL);
		if (iwd_load(fd)) return -1;
		if (strcmp(pget(station, IWD ".Station", "Scanning") ?: "false", "true")) break;
	}
	return 0;
}

static const char *net_by_name(const char *name)
{
	for (int i = 0; i < nprops; i++)
		if (!strcmp(props[i].iface, IWD ".Network") && !strcmp(props[i].key, "Name") && !strcmp(props[i].val, name)) return props[i].path;
	return NULL;
}

static int wifi_main(int fd, int argc, char **argv)
{
	const char *sub = argc > 2 ? argv[2] : "list", *st;
	Msg m;
	if (iwd_load(fd)) return 1;
	if (!(st = first_path(IWD ".Station"))) { fputs("wifi: no wifi device in station mode\n", stderr); return 1; }
	if (!strcmp(sub, "list")) {
		R r;
		uint32_t len;
		size_t end;
		const char *conn = pget(st, IWD ".Station", "ConnectedNetwork");
		if (wifi_scan(fd, st)) return 1;
		conn = pget(st, IWD ".Station", "ConnectedNetwork");
		if (call(fd, &m, IWD, st, IWD ".Station", "GetOrderedNetworks", NULL, NULL)) return fail(&m, "list");
		r = (R){ m.body, 0, m.blen, 0 };
		len = r_u32(&r); ralign(&r, 8); end = r.pos + len;
		while (!r.bad && r.pos < end) {
			char path[160];
			int16_t sig = 0;
			const char *name, *type;
			ralign(&r, 8);
			snprintf(path, sizeof path, "%s", r_str(&r));
			ralign(&r, 2);
			if (r.pos + 2 <= r.len) memcpy(&sig, r.p + r.pos, 2);
			r.pos += 2;
			name = pget(path, IWD ".Network", "Name");
			type = pget(path, IWD ".Network", "Type");
			printf("%c %-32s %-6s %4d dBm%s\n", conn && !strcmp(conn, path) ? '*' : ' ', name ?: "?", type ?: "?", sig / 100,
			       pget(path, IWD ".Network", "KnownNetwork") ? "  (known)" : "");
		}
		return 0;
	}
	if (!strcmp(sub, "disconnect")) return simple_call(fd, IWD, st, IWD ".Station", "Disconnect", NULL, NULL, "disconnect");
	if ((!strcmp(sub, "connect") || !strcmp(sub, "forget")) && argc > 3) {
		const char *net = net_by_name(argv[3]);
		if (!net && !strcmp(sub, "connect")) { if (wifi_scan(fd, st)) return 1; net = net_by_name(argv[3]); }
		if (!strcmp(sub, "forget")) {
			const char *k = net ? pget(net, IWD ".Network", "KnownNetwork") : NULL;
			if (!k) { fputs("wifi: that network is not known\n", stderr); return 1; }
			return simple_call(fd, IWD, k, IWD ".KnownNetwork", "Forget", NULL, NULL, "forget");
		}
		if (!net) { fprintf(stderr, "wifi: no network called %s in range\n", argv[3]); return 1; }
		if (argc > 4) {
			Buf b = {0};
			int rc;
			agent_pass = argv[4];
			put_str(&b, "/nmb/agent");
			rc = simple_call(fd, IWD, "/net/connman/iwd", "net.connman.iwd.AgentManager", "RegisterAgent", "o", &b, "agent");
			free(b.b);
			if (rc) return 1;
		}
		call_timeout = 40000;
		{
			int rc = simple_call(fd, IWD, net, IWD ".Network", "Connect", NULL, NULL, "connect");
			if (!rc) printf("connected to %s\n", argv[3]);
			return rc;
		}
	}
	fputs("usage: wifi list | connect SSID [PASSPHRASE] | disconnect | forget SSID\n", stderr);
	return 2;
}

/* ---- bluetooth (bluez) ------------------------------------------------------------------------------------------- */

#define BZ "org.bluez"
#define DEV BZ ".Device1"
#define ADP BZ ".Adapter1"

static int bz_load(int fd)
{
	if (load_objects(fd, BZ) || !first_path(ADP)) { fputs("bluetooth: bluez is not running or there is no adapter\n", stderr); return -1; }
	return 0;
}

static void bt_print(void)
{
	for (int i = 0; i < nprops; i++) {
		const char *path = props[i].path;
		if (strcmp(props[i].iface, DEV) || *props[i].key) continue;
		printf("%c%c%c %s  %s  %s dBm\n", !strcmp(pget(path, DEV, "Connected") ?: "", "true") ? 'C' : '-', !strcmp(pget(path, DEV, "Paired") ?: "", "true") ? 'P' : '-',
		       !strcmp(pget(path, DEV, "Trusted") ?: "", "true") ? 'T' : '-', pget(path, DEV, "Address") ?: "?", pget(path, DEV, "Alias") ?: "?",
		       pget(path, DEV, "RSSI") ?: "  -");
	}
}

static const char *bt_find(const char *what)
{
	for (int i = 0; i < nprops; i++) {
		const char *a, *n;
		if (strcmp(props[i].iface, DEV) || *props[i].key) continue;
		a = pget(props[i].path, DEV, "Address"); n = pget(props[i].path, DEV, "Alias");
		if ((a && !strcasecmp(a, what)) || (n && strcasestr(n, what))) return props[i].path;
	}
	return NULL;
}

static int set_bool(int fd, const char *path, const char *iface, const char *key, int v)
{
	Buf b = {0};
	int rc;
	put_str(&b, iface); put_str(&b, key); put_sig(&b, "b"); put_u32(&b, v);
	rc = simple_call(fd, BZ, path, "org.freedesktop.DBus.Properties", "Set", "ssv", &b, key);
	free(b.b);
	return rc;
}

static int bt_main(int fd, int argc, char **argv)
{
	const char *sub = argc > 2 ? argv[2] : "list", *ad, *dev;
	if (bz_load(fd)) return 1;
	ad = first_path(ADP);
	if (!strcmp(sub, "list")) { bt_print(); return 0; }
	if (!strcmp(sub, "power") && argc > 3) return set_bool(fd, ad, ADP, "Powered", !strcmp(argv[3], "on"));
	if (!strcmp(sub, "scan")) {
		int secs = argc > 3 ? atoi(argv[3]) : 5;
		if (simple_call(fd, BZ, ad, ADP, "StartDiscovery", NULL, NULL, "scan")) return 1;
		sleep(secs > 0 ? secs : 5);
		simple_call(fd, BZ, ad, ADP, "StopDiscovery", NULL, NULL, "scan");
		if (bz_load(fd)) return 1;
		bt_print();
		return 0;
	}
	if (argc > 3 && (dev = bt_find(argv[3]))) {
		call_timeout = 30000;
		if (!strcmp(sub, "connect")) return simple_call(fd, BZ, dev, DEV, "Connect", NULL, NULL, "connect");
		if (!strcmp(sub, "disconnect")) return simple_call(fd, BZ, dev, DEV, "Disconnect", NULL, NULL, "disconnect");
		if (!strcmp(sub, "pair")) return simple_call(fd, BZ, dev, DEV, "Pair", NULL, NULL, "pair");
		if (!strcmp(sub, "trust")) return set_bool(fd, dev, DEV, "Trusted", 1);
		if (!strcmp(sub, "remove")) {
			Buf b = {0};
			int rc;
			put_str(&b, dev);
			rc = simple_call(fd, BZ, ad, ADP, "RemoveDevice", "o", &b, "remove");
			free(b.b);
			return rc;
		}
	} else if (argc > 3) { fprintf(stderr, "bluetooth: no device matching %s (try: bt scan)\n", argv[3]); return 1; }
	fputs("usage: bt list | scan [SEC] | connect|disconnect|pair|trust|remove NAME_OR_MAC | power on|off\n", stderr);
	return 2;
}

/* ---- status and main --------------------------------------------------------------------------------------------- */

static void slurp(const char *path, char *out, size_t n)
{
	FILE *f = fopen(path, "r");
	out[0] = 0;
	if (f) { if (fgets(out, n, f)) out[strcspn(out, "\n")] = 0; fclose(f); }
}

static void status(void)
{
	DIR *d = opendir("/sys/class/net");
	struct dirent *e;
	struct ifaddrs *ifa, *a;
	int fd;
	if (d) while ((e = readdir(d))) {
		char p[300], state[32], mac[32], carrier[8], ip[INET_ADDRSTRLEN] = "";
		if (e->d_name[0] == '.' || !strcmp(e->d_name, "lo")) continue;
		snprintf(p, sizeof p, "/sys/class/net/%s/operstate", e->d_name); slurp(p, state, sizeof state);
		snprintf(p, sizeof p, "/sys/class/net/%s/address", e->d_name); slurp(p, mac, sizeof mac);
		snprintf(p, sizeof p, "/sys/class/net/%s/carrier", e->d_name); slurp(p, carrier, sizeof carrier);
		if (!getifaddrs(&ifa)) {
			for (a = ifa; a; a = a->ifa_next)
				if (a->ifa_addr && a->ifa_addr->sa_family == AF_INET && !strcmp(a->ifa_name, e->d_name))
					inet_ntop(AF_INET, &((struct sockaddr_in *)a->ifa_addr)->sin_addr, ip, sizeof ip);
			freeifaddrs(ifa);
		}
		snprintf(p, sizeof p, "/sys/class/net/%s/wireless", e->d_name);
		printf("%-8s %-5s %-10s %-17s %s%s\n", e->d_name, access(p, F_OK) ? "eth" : "wifi", state, mac, ip, !strcmp(carrier, "0") ? "  no cable/link" : "");
	}
	if (d) closedir(d);
	if ((fd = system_bus()) < 0) return;
	if (!load_objects(fd, IWD)) {
		const char *st = first_path(IWD ".Station"), *c = st ? pget(st, IWD ".Station", "ConnectedNetwork") : NULL;
		if (st) printf("wifi     %s%s%s\n", pget(st, IWD ".Station", "State") ?: "?", c ? " to " : "", c ? pget(c, IWD ".Network", "Name") ?: "" : "");
	} else puts("wifi     iwd not running");
	if (wpa_present("wlan0")) puts("wifi     wlan0 is driven by wpa_supplicant (not iwd)");
	if (!load_objects(fd, BZ) && first_path(ADP)) {
		int n = 0;
		const char *ad = first_path(ADP);
		for (int i = 0; i < nprops; i++)
			if (!strcmp(props[i].iface, DEV) && !*props[i].key && !strcmp(pget(props[i].path, DEV, "Connected") ?: "", "true")) {
				printf("bt       connected: %s\n", pget(props[i].path, DEV, "Alias") ?: "?");
				n++;
			}
		printf("bt       adapter %s%s\n", !strcmp(pget(ad, ADP, "Powered") ?: "", "true") ? "on" : "off", n ? "" : ", nothing connected");
	} else puts("bt       bluez not running or no adapter");
	close(fd);
}

int main(int argc, char **argv)
{
	const char *c = argc > 1 ? argv[1] : "";
	int fd, rc;
	if (!*c || !strcmp(c, "status")) { status(); return 0; }
	if (!strcmp(c, "fw")) { execlp("nft", "nft", "list", "ruleset", (char *)NULL); perror("nft"); return 1; }
	if (strcmp(c, "wifi") && strcmp(c, "bt")) {
		fputs("usage: netmanager_blue [status] | wifi ... | bt ... | fw\n", stderr);
		return 2;
	}
	if (!strcmp(c, "wifi")) {
		const char *ifn = getenv("NMB_IFACE") ?: "wlan0";
		if (wpa_present(ifn)) return wpa_main(ifn, argc, argv);
	}
	if ((fd = system_bus()) < 0) return 1;
	signal(SIGPIPE, SIG_IGN);
	rc = !strcmp(c, "wifi") ? wifi_main(fd, argc, argv) : bt_main(fd, argc, argv);
	close(fd);
	return rc;
}
