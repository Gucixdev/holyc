/* notts - notification daemon and sender in one C file. Speaks the D-Bus wire protocol itself: no libdbus, no libsystemd, no GLib.
 *
 *   notts                       run the daemon (org.freedesktop.Notifications on the session bus)
 *   notts send [-u LEVEL] [-t MS] [-a APP] SUMMARY [BODY]
 *                               send one notification (LEVEL low|normal|critical); works with any daemon
 *   notts history [N]           the last N notifications (default 20), newest last
 *   notts dnd on|off|status     do-not-disturb: still logged, hooks and forwards are silent (critical ones always pass)
 *   notts last                  print the newest notification (what the bar shows)
 *
 * Every notification is appended to $XDG_STATE_HOME/notts/history (tab separated: time id urgency app summary body) and the
 * newest one is written to $XDG_RUNTIME_DIR/notts.last for a status bar. Then, per ~/.config/notts/config (one "key value" per line):
 *   hook CMD                 run CMD (sh -c) with NOTTS_ID NOTTS_APP NOTTS_SUMMARY NOTTS_BODY NOTTS_URGENCY (0 low 1 normal 2 critical) set
 *   discord WEBHOOK_URL      post "app: summary - body" to a Discord webhook (needs curl)
 *   telegram TOKEN CHAT_ID   send it to a Telegram chat through the Bot API (needs curl)
 *   ignore APP               drop notifications from this app (can be given many times)
 * Only little-endian D-Bus peers are understood (every Linux machine). Actions and images are accepted and ignored.
 */
#define _GNU_SOURCE
#include <fcntl.h>
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
	char path[128], iface[96], member[64], sender[64], sig[32];
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
			char *dst = code == 1 ? m->path : code == 2 ? m->iface : code == 3 ? m->member : code == 7 ? m->sender : NULL;
			size_t cap = code == 1 ? sizeof m->path : code == 2 ? sizeof m->iface : code == 3 ? sizeof m->member : sizeof m->sender;
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

static int connect_bus(void)
{
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	const char *addr = getenv("DBUS_SESSION_BUS_ADDRESS");
	char def[128], hex[64] = "", cred[32], line[256];
	size_t len;
	int fd, hl = 0;
	if (!addr || !*addr) { snprintf(def, sizeof def, "unix:path=/run/user/%d/bus", (int)getuid()); addr = def; }
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
static int call(int fd, Msg *m, const char *dest, const char *path, const char *iface, const char *member, const char *sig, const Buf *body)
{
	Buf out = {0};
	uint32_t s = serial_no++;
	build(&out, 1, s, dest, path, iface, member, 0, NULL, sig, body);
	send_msg(fd, &out);
	free(out.b);
	for (int i = 0; i < 64; i++) {
		if (read_msg(fd, m, 3000)) return -1;
		if ((m->type == 2 || m->type == 3) && m->reply_serial == s) return m->type == 3;
	}
	return -1;
}

static int bus_hello(int fd)
{
	Msg m;
	return call(fd, &m, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "Hello", NULL, NULL);
}

/* ---- storage and configuration ----------------------------------------------------------------------------------- */

static char hist_path[400], last_path[400], dnd_path[400], conf_path[400];

static void paths(void)
{
	const char *st = getenv("XDG_STATE_HOME"), *rt = getenv("XDG_RUNTIME_DIR"), *cf = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
	char dir[400], parent[400];
	if (st && *st) snprintf(dir, sizeof dir, "%s/notts", st);
	else snprintf(dir, sizeof dir, "%s/.local/state/notts", h ? h : ".");
	snprintf(parent, sizeof parent, "%s", dir);
	for (char *s = parent + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(parent, 0700); *s = '/'; }
	mkdir(dir, 0700);
	snprintf(hist_path, sizeof hist_path, "%s/history", dir);
	snprintf(dnd_path, sizeof dnd_path, "%s/dnd", dir);
	snprintf(last_path, sizeof last_path, "%s/notts.last", rt ? rt : "/tmp");
	if (cf && *cf) snprintf(conf_path, sizeof conf_path, "%s/notts/config", cf);
	else snprintf(conf_path, sizeof conf_path, "%s/.config/notts/config", h ? h : ".");
}

static char *hook, *discord, *tg_token, *tg_chat, *ignored[32];
static int nignored;

static void load_config(void)
{
	FILE *f = fopen(conf_path, "r");
	char line[1024];
	if (!f) return;
	while (fgets(line, sizeof line, f)) {
		char *k = line, *v;
		line[strcspn(line, "\n")] = 0;
		if (*k == '#' || !*k) continue;
		v = strchr(k, ' ');
		if (!v) continue;
		*v++ = 0;
		while (*v == ' ') v++;
		if (!strcmp(k, "hook")) hook = strdup(v);
		else if (!strcmp(k, "discord")) discord = strdup(v);
		else if (!strcmp(k, "telegram")) {
			char *sp = strchr(v, ' ');
			if (sp) { *sp++ = 0; tg_token = strdup(v); tg_chat = strdup(sp); }
		} else if (!strcmp(k, "ignore") && nignored < 32) ignored[nignored++] = strdup(v);
	}
	fclose(f);
}

static void clean(char *s) { for (; *s; s++) if (*s == '\t' || *s == '\n' || *s == '\r') *s = ' '; }

/* JSON string escape into dst */
static void jesc(char *dst, size_t n, const char *s)
{
	size_t o = 0;
	for (; *s && o + 7 < n; s++) {
		unsigned char c = *s;
		if (c == '"' || c == '\\') { dst[o++] = '\\'; dst[o++] = c; }
		else if (c == '\n') { dst[o++] = '\\'; dst[o++] = 'n'; }
		else if (c < 0x20) o += snprintf(dst + o, n - o, "\\u%04x", c);
		else dst[o++] = c;
	}
	dst[o] = 0;
}

static void spawn(char *const argv[])
{
	pid_t p = fork();
	if (p == 0) {
		int nul = open("/dev/null", O_RDWR);
		if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
		setsid();
		execvp(argv[0], argv);
		_exit(127);
	}
}

static void deliver(uint32_t id, int urgency, const char *app, const char *summary, const char *body)
{
	char a[256], s[512], b[2048], idb[16], ub[4], text[3000];
	FILE *f;
	int dnd = access(dnd_path, F_OK) == 0;
	snprintf(a, sizeof a, "%s", app); snprintf(s, sizeof s, "%s", summary); snprintf(b, sizeof b, "%s", body);
	clean(a); clean(s); clean(b);
	for (int i = 0; i < nignored; i++) if (!strcmp(ignored[i], a)) return;
	if ((f = fopen(hist_path, "a"))) {
		fprintf(f, "%ld\t%u\t%d\t%s\t%s\t%s\n", (long)time(NULL), id, urgency, a, s, b);
		fclose(f);
	}
	if ((f = fopen(last_path, "w"))) {
		fprintf(f, "%ld\t%d\t%s\t%s\t%s\n", (long)time(NULL), urgency, a, s, b);
		fclose(f);
	}
	if (dnd && urgency < 2) return;
	snprintf(idb, sizeof idb, "%u", id);
	snprintf(ub, sizeof ub, "%d", urgency);
	if (hook) {
		char *argv[] = { "sh", "-c", hook, NULL };
		setenv("NOTTS_ID", idb, 1); setenv("NOTTS_APP", a, 1); setenv("NOTTS_SUMMARY", s, 1);
		setenv("NOTTS_BODY", b, 1); setenv("NOTTS_URGENCY", ub, 1);
		spawn(argv);
	}
	snprintf(text, sizeof text, "%s: %s%s%s", a, s, *b ? " - " : "", b);
	if (discord) {
		char esc[3500], json[3600];
		jesc(esc, sizeof esc, text);
		snprintf(json, sizeof json, "{\"content\":\"%s\"}", esc);
		char *argv[] = { "curl", "-s", "-m", "10", "-H", "Content-Type: application/json", "-d", json, discord, NULL };
		spawn(argv);
	}
	if (tg_token) {
		char url[400], chat[100];
		snprintf(url, sizeof url, "https://api.telegram.org/bot%s/sendMessage", tg_token);
		snprintf(chat, sizeof chat, "chat_id=%s", tg_chat);
		char t[3100];
		snprintf(t, sizeof t, "text=%s", text);
		char *argv[] = { "curl", "-s", "-m", "10", "--data-urlencode", chat, "--data-urlencode", t, url, NULL };
		spawn(argv);
	}
}

/* ---- the daemon -------------------------------------------------------------------------------------------------- */

#define MAXACT 64
static struct { uint32_t id; time_t expires; } active[MAXACT];
static int nactive;
static uint32_t next_id = 1;

static void emit_closed(int fd, uint32_t id, uint32_t reason)
{
	Buf body = {0}, out = {0};
	put_u32(&body, id);
	put_u32(&body, reason);
	build(&out, 4, serial_no++, NULL, "/org/freedesktop/Notifications", "org.freedesktop.Notifications", "NotificationClosed", 0, NULL, "uu", &body);
	send_msg(fd, &out);
	free(body.b); free(out.b);
}

static void remove_active(uint32_t id)
{
	for (int i = 0; i < nactive; i++) if (active[i].id == id) { active[i] = active[--nactive]; return; }
}

static void reply(int fd, const Msg *req, const char *sig, const Buf *body)
{
	Buf out = {0};
	build(&out, 2, serial_no++, req->sender, NULL, NULL, NULL, req->serial, NULL, sig, body);
	send_msg(fd, &out);
	free(out.b);
}

static void reply_error(int fd, const Msg *req, const char *name)
{
	Buf out = {0};
	build(&out, 3, serial_no++, req->sender, NULL, NULL, NULL, req->serial, name, NULL, NULL);
	send_msg(fd, &out);
	free(out.b);
}

static void handle(int fd, const Msg *m)
{
	Buf body = {0};
	if (m->type != 1) return;
	if (!strcmp(m->iface, "org.freedesktop.DBus.Peer") && !strcmp(m->member, "Ping")) { reply(fd, m, NULL, NULL); return; }
	if (strcmp(m->iface, "org.freedesktop.Notifications")) { reply_error(fd, m, "org.freedesktop.DBus.Error.UnknownMethod"); return; }
	if (!strcmp(m->member, "GetCapabilities")) {
		Buf arr = {0};
		put_str(&arr, "body");
		put_str(&arr, "persistence");
		put_u32(&body, arr.n);
		put(&body, arr.b, arr.n);
		free(arr.b);
		reply(fd, m, "as", &body);
	} else if (!strcmp(m->member, "GetServerInformation")) {
		put_str(&body, "notts"); put_str(&body, "Gucixdev"); put_str(&body, "0.1"); put_str(&body, "1.2");
		reply(fd, m, "ssss", &body);
	} else if (!strcmp(m->member, "CloseNotification")) {
		R r = { m->body, 0, m->blen, 0 };
		uint32_t id = r_u32(&r);
		remove_active(id);
		reply(fd, m, NULL, NULL);
		emit_closed(fd, id, 3);
	} else if (!strcmp(m->member, "Notify") && !strcmp(m->sig, "susssasa{sv}i")) {
		R r = { m->body, 0, m->blen, 0 };
		const char *app = r_str(&r), *icon, *summary, *text;
		uint32_t replaces = r_u32(&r), id;
		int urgency = 1, timeout;
		icon = r_str(&r); (void)icon;
		summary = r_str(&r);
		text = r_str(&r);
		{ const char *s = "as"; skip_val(&r, &s); }
		{   /* hints: a{sv} */
			uint32_t len = r_u32(&r);
			size_t end;
			ralign(&r, 8);
			end = r.pos + len;
			while (!r.bad && r.pos < end) {
				const char *key, *sig;
				ralign(&r, 8);
				key = r_str(&r);
				sig = r_sig(&r);
				if (!strcmp(key, "urgency") && !strcmp(sig, "y")) urgency = r_byte(&r);
				else skip_val(&r, &sig);
			}
		}
		timeout = (int)r_u32(&r);
		if (r.bad) { reply_error(fd, m, "org.freedesktop.DBus.Error.InvalidArgs"); return; }
		id = replaces ? replaces : next_id++;
		if (urgency < 0 || urgency > 2) urgency = 1;
		remove_active(id);
		if (nactive < MAXACT) {
			active[nactive].id = id;
			active[nactive].expires = timeout == 0 ? 0 : time(NULL) + (timeout < 0 ? (urgency == 2 ? 0 : 5) : (timeout + 999) / 1000);
			nactive++;
		}
		deliver(id, urgency, app, summary, text);
		put_u32(&body, id);
		reply(fd, m, "u", &body);
	} else reply_error(fd, m, "org.freedesktop.DBus.Error.UnknownMethod");
	free(body.b);
}

static int daemon_main(void)
{
	int fd = connect_bus();
	Msg m;
	Buf body = {0};
	if (fd < 0) { fputs("notts: cannot reach the session bus\n", stderr); return 1; }
	if (bus_hello(fd)) { fputs("notts: Hello failed\n", stderr); return 1; }
	put_str(&body, "org.freedesktop.Notifications");
	put_u32(&body, 4);   /* DBUS_NAME_FLAG_DO_NOT_QUEUE */
	if (call(fd, &m, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName", "su", &body) ||
	    r_u32(&(R){ m.body, 0, m.blen, 0 }) != 1) {
		fputs("notts: another notification daemon owns the name (stop dunst/mako first)\n", stderr);
		return 1;
	}
	free(body.b);
	signal(SIGCHLD, SIG_IGN);
	load_config();
	for (;;) {
		struct pollfd p = { fd, POLLIN, 0 };
		time_t now = time(NULL);
		for (int i = 0; i < nactive;)
			if (active[i].expires && active[i].expires <= now) { uint32_t id = active[i].id; active[i] = active[--nactive]; emit_closed(fd, id, 1); }
			else i++;
		if (poll(&p, 1, nactive ? 1000 : -1) > 0) {
			if (read_msg(fd, &m, 2000)) { fputs("notts: bus connection lost\n", stderr); return 1; }
			handle(fd, &m);
		}
	}
}

/* ---- client side ------------------------------------------------------------------------------------------------- */

static int send_main(int argc, char **argv)
{
	const char *app = "notts", *summary = NULL, *text = "";
	int urgency = 1, timeout = -1, fd, i = 2;
	Buf body = {0}, hints = {0};
	Msg m;
	for (; i < argc; i++) {
		if (!strcmp(argv[i], "-u") && i + 1 < argc) {
			i++;
			urgency = !strcmp(argv[i], "low") ? 0 : !strcmp(argv[i], "critical") ? 2 : 1;
		} else if (!strcmp(argv[i], "-t") && i + 1 < argc) timeout = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-a") && i + 1 < argc) app = argv[++i];
		else break;
	}
	if (i >= argc) { fputs("usage: notts send [-u low|normal|critical] [-t MS] [-a APP] SUMMARY [BODY]\n", stderr); return 2; }
	summary = argv[i++];
	if (i < argc) text = argv[i];
	if ((fd = connect_bus()) < 0 || bus_hello(fd)) { fputs("notts: cannot reach the session bus\n", stderr); return 1; }
	put_str(&body, app);
	put_u32(&body, 0);
	put_str(&body, "");
	put_str(&body, summary);
	put_str(&body, text);
	put_u32(&body, 0);   /* actions: empty array */
	/* hints: one entry, urgency (byte) */
	align(&hints, 8);
	put_str(&hints, "urgency");
	put_sig(&hints, "y");
	{ unsigned char u = urgency; put(&hints, &u, 1); }
	put_u32(&body, hints.n);
	align(&body, 8);
	put(&body, hints.b, hints.n);
	{ uint32_t t = timeout; put_u32(&body, t); }
	if (call(fd, &m, "org.freedesktop.Notifications", "/org/freedesktop/Notifications", "org.freedesktop.Notifications", "Notify", "susssasa{sv}i", &body)) {
		fputs("notts: no notification daemon answered\n", stderr);
		return 1;
	}
	printf("%u\n", r_u32(&(R){ m.body, 0, m.blen, 0 }));
	return 0;
}

static int history_main(int n)
{
	FILE *f = fopen(hist_path, "r");
	static char lines[1000][2600];
	int count = 0;
	if (!f) return 0;
	while (fgets(lines[count % 1000], sizeof lines[0], f)) count++;
	fclose(f);
	for (int i = count > n ? count - n : 0; i < count; i++) {
		long t; unsigned id; int u; char a[256], s[512], b[2048];
		char *l = lines[i % 1000], *p;
		char *tok[6] = {0};
		int k = 0;
		l[strcspn(l, "\n")] = 0;
		for (p = l; k < 6; k++) { tok[k] = p; if (!(p = strchr(p, '\t'))) break; *p++ = 0; }
		if (k < 5) continue;
		t = atol(tok[0]); id = atoi(tok[1]); u = atoi(tok[2]);
		snprintf(a, sizeof a, "%s", tok[3]); snprintf(s, sizeof s, "%s", tok[4]); snprintf(b, sizeof b, "%s", tok[5] ? tok[5] : "");
		{ char ts[32]; strftime(ts, sizeof ts, "%m-%d %H:%M:%S", localtime(&t)); printf("%s #%u %s%s: %s%s%s\n", ts, id, u == 2 ? "! " : "", a, s, *b ? " - " : "", b); }
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *c = argc > 1 ? argv[1] : "";
	paths();
	if (!*c) return daemon_main();
	if (!strcmp(c, "send")) return send_main(argc, argv);
	if (!strcmp(c, "history")) return history_main(argc > 2 ? atoi(argv[2]) : 20);
	if (!strcmp(c, "last")) {
		FILE *f = fopen(last_path, "r");
		char line[3000];
		if (f && fgets(line, sizeof line, f)) fputs(line, stdout);
		if (f) fclose(f);
		return 0;
	}
	if (!strcmp(c, "dnd") && argc > 2) {
		if (!strcmp(argv[2], "on")) { int fd = open(dnd_path, O_CREAT | O_WRONLY, 0600); if (fd >= 0) close(fd); return 0; }
		if (!strcmp(argv[2], "off")) { unlink(dnd_path); return 0; }
		if (!strcmp(argv[2], "status")) { puts(access(dnd_path, F_OK) == 0 ? "on" : "off"); return 0; }
	}
	fputs("usage: notts | send [-u LEVEL] [-t MS] [-a APP] SUMMARY [BODY] | history [N] | last | dnd on|off|status\n", stderr);
	return 2;
}
