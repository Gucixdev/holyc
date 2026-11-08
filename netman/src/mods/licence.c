/* licence.c - see licence.h. */
#define _GNU_SOURCE
#include "licence.h"
#include "entcore.h"
#include "enterprise.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/random.h>
#include <sys/stat.h>

const char *netman_lic_dir = "/etc/netman/licences";
int netman_lic_warn_days = 30;
const char *netman_lic_renew_hook = "";
const char *netman_lic_apply_hook = "";
static time_t lic__now(void) { return time(NULL); }
time_t (*netman_lic_now)(void) = lic__now;

#define LF(...) ent_fail(-EINVAL, __VA_ARGS__)

static int lic__date(const char *s, time_t *out)
{
	struct tm t;
	int y, m, d;
	char x;
	if (!strcmp(s, "never")) return *out = 0, 0;
	if (sscanf(s, "%4d-%2d-%2d%c", &y, &m, &d, &x) != 3 || strlen(s) != 10 || m < 1 || m > 12 || d < 1 || d > 31 || y < 1970) return -EINVAL;
	memset(&t, 0, sizeof t);
	t.tm_year = y - 1900, t.tm_mon = m - 1, t.tm_mday = d + 1; /* valid through the whole day */
	*out = timegm(&t);
	return *out == (time_t)-1 ? -EINVAL : 0;
}
static void lic__state(netman_lic *l)
{
	time_t now = netman_lic_now();
	if (!l->expires) { l->days = 99999, l->state = 0; return; }
	l->days = (int)((l->expires - now) / 86400);
	if (now >= l->expires) l->days = (int)(-((now - l->expires) / 86400) - 1), l->state = 2;
	else l->state = l->days < netman_lic_warn_days ? 1 : 0;
}
int netman_lic_parse(const char *text, netman_lic *l)
{
	char *b = strdup(text), *s, *save, *ln;
	int have_exp = 0, rc = 0;
	if (!b) return -ENOMEM;
	memset(l, 0, sizeof *l);
	for (ln = strtok_r(b, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
		char *k = ent_trim(ln), *v;
		if (!*k || *k == '#' || !(v = strchr(k, '='))) continue;
		*v++ = 0;
		k = ent_trim(k), v = ent_trim(v);
		if (!strcmp(k, "name")) ent_cpy(l->name, sizeof l->name, v);
		else if (!strcmp(k, "holder")) ent_cpy(l->holder, sizeof l->holder, v);
		else if (!strcmp(k, "kind")) ent_cpy(l->kind, sizeof l->kind, v);
		else if (!strcmp(k, "expires")) {
			if (lic__date(v, &l->expires) < 0) { rc = LF("bad expires '%s' (want YYYY-MM-DD or never)", v); break; }
			have_exp = 1;
		}
	}
	s = l->name;
	free(b);
	if (rc) return rc;
	if (!ent_safe_name(s, 64)) return LF("licence has no valid name=");
	if (!have_exp) return LF("licence has no expires=");
	lic__state(l);
	return 0;
}
static int lic__path(char *p, size_t n, const char *name, const char *ext)
{
	if (!ent_safe_name(name, 64)) return LF("bad licence name");
	snprintf(p, n, "%s/%s.%s", netman_lic_dir, name, ext);
	return 0;
}
int netman_lic_load(const char *name, netman_lic *l)
{
	char p[600], buf[4096];
	int r = lic__path(p, sizeof p, name, "lic");
	if (r < 0) return r;
	if ((r = ent_slurp(p, buf, sizeof buf)) < 0) return ent_fail(r, "cannot read %s: %s", p, strerror(-r));
	return netman_lic_parse(buf, l);
}
static int lic__cmp(const void *a, const void *b) { return strcmp(((const netman_lic *)a)->name, ((const netman_lic *)b)->name); }
int netman_lic_list(netman_lic *out, int max)
{
	DIR *d = opendir(netman_lic_dir);
	struct dirent *e;
	int c = 0;
	if (!d) return errno == ENOENT ? 0 : -errno;
	while ((e = readdir(d)) && c < max) {
		size_t l = strlen(e->d_name);
		char nm[64];
		if (l < 5 || l - 4 >= sizeof nm || strcmp(e->d_name + l - 4, ".lic")) continue;
		memcpy(nm, e->d_name, l - 4), nm[l - 4] = 0;
		if (netman_lic_load(nm, &out[c]) == 0) c++;
	}
	closedir(d);
	qsort(out, c, sizeof *out, lic__cmp);
	return c;
}
int netman_lic_status(FILE *f)
{
	static netman_lic v[64];
	int n = netman_lic_list(v, 64), i, bad = 0;
	if (n < 0) return n;
	if (!n) fprintf(f, "no licences in %s\n", netman_lic_dir);
	for (i = 0; i < n; i++) {
		char when[16] = "never";
		if (v[i].expires) {
			time_t last = v[i].expires - 86400;
			strftime(when, sizeof when, "%Y-%m-%d", gmtime(&last));
		}
		fprintf(f, "%-20s %-8s %-12s expires %-10s ", v[i].name, v[i].state == 2 ? "EXPIRED" : v[i].state == 1 ? "EXPIRING" : "valid", v[i].kind[0] ? v[i].kind : "-", when);
		if (v[i].expires) fprintf(f, "(%d day%s %s)", v[i].days < 0 ? -v[i].days : v[i].days, (v[i].days == 1 || v[i].days == -1) ? "" : "s", v[i].days < 0 ? "ago" : "left");
		fputc('\n', f);
		if (v[i].state) bad++;
	}
	return bad;
}

static int lic__run_hook(const char *hook, const char *a, const char *b, const char *c)
{
	char buf[512], *av[16], err[256];
	int n = 0, r;
	char *t, *s;
	ent_cpy(buf, sizeof buf, hook);
	for (t = strtok_r(buf, " \t", &s); t && n < 12; t = strtok_r(NULL, " \t", &s)) av[n++] = t;
	if (!n) return ent_fail(-ENOENT, "no hook configured");
	if (a) av[n++] = (char *)a;
	if (b) av[n++] = (char *)b;
	if (c) av[n++] = (char *)c;
	av[n] = NULL;
	if ((r = ent_exec(av, NULL, err, sizeof err)) < 0) return ent_fail(r, "hook %s failed: %s", av[0], r == -ENOENT ? "not found" : err);
	return 0;
}
int netman_lic_renew(const char *name)
{
	char cur[600], nw[600], prev[600], buf[4096];
	netman_lic old, now;
	int r;
	if (!netman_lic_renew_hook || !*netman_lic_renew_hook) return ent_fail(-ENOENT, "no renew_hook configured");
	if ((r = netman_lic_load(name, &old)) < 0) return r;
	lic__path(cur, sizeof cur, name, "lic");
	lic__path(nw, sizeof nw, name, "lic.new");
	lic__path(prev, sizeof prev, name, "lic.prev");
	unlink(nw);
	if ((r = lic__run_hook(netman_lic_renew_hook, name, cur, nw)) < 0) { unlink(nw); return r; }
	if ((r = ent_slurp(nw, buf, sizeof buf)) < 0) return ent_fail(r, "renew hook wrote no %s", nw);
	if ((r = netman_lic_parse(buf, &now)) < 0) { unlink(nw); return r; }
	if (strcmp(now.name, old.name)) { unlink(nw); return LF("renewed licence is for '%s', not '%s'", now.name, old.name); }
	if (old.expires && now.expires && now.expires <= old.expires) { unlink(nw); return LF("renewed licence does not expire later than the current one"); }
	if (old.expires == 0 && now.expires) { unlink(nw); return LF("renewal would turn a perpetual licence into a dated one"); }
	chmod(nw, 0600);
	unlink(prev);
	if (link(cur, prev) < 0) { r = -errno; unlink(nw); return ent_fail(r, "cannot keep %s: %s", prev, strerror(-r)); }
	if (rename(nw, cur) < 0) { r = -errno; unlink(nw); return ent_fail(r, "cannot install renewed licence: %s", strerror(-r)); }
	return 0;
}

/* ------------------------------------------------------------------ Curve25519 (TweetNaCl scalarmult, public domain algorithm) */
typedef int64_t gf[16];
static const gf lic__121665 = {0xDB41, 1};
static void car(gf o)
{
	int i;
	int64_t c;
	for (i = 0; i < 16; i++) {
		o[i] += (1LL << 16);
		c = o[i] >> 16;
		o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
		o[i] -= c << 16;
	}
}
static void sel(gf p, gf q, int b)
{
	int64_t t, c = ~((int64_t)b - 1);
	int i;
	for (i = 0; i < 16; i++) t = c & (p[i] ^ q[i]), p[i] ^= t, q[i] ^= t;
}
static void pack(uint8_t *o, const gf n)
{
	int i, j, b;
	gf m, t;
	for (i = 0; i < 16; i++) t[i] = n[i];
	car(t), car(t), car(t);
	for (j = 0; j < 2; j++) {
		m[0] = t[0] - 0xffed;
		for (i = 1; i < 15; i++) {
			m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
			m[i - 1] &= 0xffff;
		}
		m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
		b = (m[15] >> 16) & 1;
		m[14] &= 0xffff;
		sel(t, m, 1 - b);
	}
	for (i = 0; i < 16; i++) o[2 * i] = t[i] & 0xff, o[2 * i + 1] = t[i] >> 8;
}
static void unpack(gf o, const uint8_t *n)
{
	int i;
	for (i = 0; i < 16; i++) o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
	o[15] &= 0x7fff;
}
static void ga(gf o, const gf a, const gf b) { int i; for (i = 0; i < 16; i++) o[i] = a[i] + b[i]; }
static void gz(gf o, const gf a, const gf b) { int i; for (i = 0; i < 16; i++) o[i] = a[i] - b[i]; }
static void gm(gf o, const gf a, const gf b)
{
	int64_t i, j, t[31];
	for (i = 0; i < 31; i++) t[i] = 0;
	for (i = 0; i < 16; i++)
		for (j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
	for (i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
	for (i = 0; i < 16; i++) o[i] = t[i];
	car(o), car(o);
}
static void gs(gf o, const gf a) { gm(o, a, a); }
static void inv(gf o, const gf i)
{
	gf c;
	int a;
	for (a = 0; a < 16; a++) c[a] = i[a];
	for (a = 253; a >= 0; a--) {
		gs(c, c);
		if (a != 2 && a != 4) gm(c, c, i);
	}
	for (a = 0; a < 16; a++) o[a] = c[a];
}
static void scalarmult(uint8_t *q, const uint8_t *n, const uint8_t *p)
{
	uint8_t z[32];
	int64_t x[80], r, i;
	gf a, b, c, d, e, f;
	for (i = 0; i < 31; i++) z[i] = n[i];
	z[31] = (n[31] & 127) | 64;
	z[0] &= 248;
	unpack(x, p);
	for (i = 0; i < 16; i++) b[i] = x[i], d[i] = a[i] = c[i] = 0;
	a[0] = d[0] = 1;
	for (i = 254; i >= 0; --i) {
		r = (z[i >> 3] >> (i & 7)) & 1;
		sel(a, b, r), sel(c, d, r);
		ga(e, a, c), gz(a, a, c), ga(c, b, d), gz(b, b, d);
		gs(d, e), gs(f, a), gm(a, c, a), gm(c, b, e);
		ga(e, a, c), gz(a, a, c), gs(b, a), gz(c, d, f);
		gm(a, c, lic__121665), ga(a, a, d), gm(c, c, a), gm(a, d, f), gm(d, b, x), gs(b, e);
		sel(a, b, r), sel(c, d, r);
	}
	for (i = 0; i < 16; i++) x[i + 16] = a[i], x[i + 32] = c[i], x[i + 48] = b[i], x[i + 64] = d[i];
	inv(x + 32, x + 32);
	gm(x + 16, x + 16, x + 32);
	pack(q, x + 16);
}

static const char lic__b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static void b64enc(const uint8_t *in, char *out) /* exactly 32 bytes -> 44 chars + NUL */
{
	int i, o = 0;
	for (i = 0; i < 30; i += 3) {
		uint32_t v = in[i] << 16 | in[i + 1] << 8 | in[i + 2];
		out[o++] = lic__b64[v >> 18], out[o++] = lic__b64[(v >> 12) & 63], out[o++] = lic__b64[(v >> 6) & 63], out[o++] = lic__b64[v & 63];
	}
	{
		uint32_t v = in[30] << 16 | in[31] << 8;
		out[o++] = lic__b64[v >> 18], out[o++] = lic__b64[(v >> 12) & 63], out[o++] = lic__b64[(v >> 6) & 63], out[o++] = '=';
	}
	out[o] = 0;
}
static int b64dec(const char *s, uint8_t *out) /* 44 chars -> 32 bytes */
{
	int i, j, o = 0;
	if (strlen(s) != 44 || s[43] != '=') return -EINVAL;
	for (i = 0; i < 44; i += 4) {
		uint32_t v = 0;
		for (j = 0; j < 4; j++) {
			const char *p = i + j == 43 ? lic__b64 : strchr(lic__b64, s[i + j]);
			if (!p || !s[i + j]) return -EINVAL;
			v = v << 6 | (uint32_t)(p - lic__b64);
		}
		out[o++] = v >> 16;
		if (o < 32) out[o++] = v >> 8;
		if (o < 32) out[o++] = v;
	}
	return 0;
}
int netman_key_generate(char priv[45])
{
	uint8_t k[32];
	if (getrandom(k, 32, 0) != 32) return ent_fail(-EIO, "no random bytes: %s", strerror(errno));
	k[0] &= 248, k[31] = (k[31] & 127) | 64;
	b64enc(k, priv);
	memset(k, 0, sizeof k);
	return 0;
}
int netman_key_pub(const char *priv, char pub[45])
{
	uint8_t k[32], p[32], base[32] = {9};
	if (b64dec(priv, k) < 0) return LF("not a base64 32-byte key");
	scalarmult(p, k, base);
	b64enc(p, pub);
	memset(k, 0, sizeof k);
	return 0;
}
static int key__read(const char *path, char priv[45])
{
	char b[128];
	int r = ent_slurp(path, b, sizeof b);
	if (r < 0) return ent_fail(r, "cannot read %s: %s", path, strerror(-r));
	ent_cpy(priv, 45, ent_trim(b));
	return 0;
}
int netman_key_show(const char *name, char pub[45])
{
	char p[600], k[45];
	int r = lic__path(p, sizeof p, name, "key");
	if (r < 0 || (r = key__read(p, k)) < 0) return r;
	r = netman_key_pub(k, pub);
	memset(k, 0, sizeof k);
	return r;
}
int netman_key_rotate(const char *name, char pub[45])
{
	char cur[600], nw[600], prev[600], k[45], line[64];
	int r, had;
	if ((r = lic__path(cur, sizeof cur, name, "key")) < 0) return r;
	lic__path(nw, sizeof nw, name, "key.new");
	lic__path(prev, sizeof prev, name, "key.prev");
	if ((r = ent_mkdirs(netman_lic_dir)) < 0) return ent_fail(r, "cannot create %s", netman_lic_dir);
	if ((r = netman_key_generate(k)) < 0 || (r = netman_key_pub(k, pub)) < 0) return r;
	snprintf(line, sizeof line, "%s\n", k);
	memset(k, 0, sizeof k);
	if ((r = ent_write_atomic(nw, line, strlen(line), 0600)) < 0) return ent_fail(r, "cannot write %s: %s", nw, strerror(-r));
	memset(line, 0, sizeof line);
	had = access(cur, F_OK) == 0;
	if (had) {
		unlink(prev);
		if (link(cur, prev) < 0) { r = -errno; unlink(nw); return ent_fail(r, "cannot keep previous key: %s", strerror(-r)); }
	}
	if (rename(nw, cur) < 0) { r = -errno; unlink(nw); return ent_fail(r, "cannot install new key: %s", strerror(-r)); }
	if (netman_lic_apply_hook && *netman_lic_apply_hook && (r = lic__run_hook(netman_lic_apply_hook, name, pub, NULL)) < 0) {
		if (had) rename(prev, cur); /* hook refused: back to the old key */
		else unlink(cur);
		return r;
	}
	return 0;
}
