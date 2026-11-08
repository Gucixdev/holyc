/* config.c - netman.conf loader/editor. See config.h for the format and the key table. Plain C, no module dependencies. */
#define _GNU_SOURCE
#include "config.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LINE_MAX_BYTES 1023

typedef struct { const char *sec, *name, *type, *def, *doc; } Key;
static const Key keys[NC_N] = {
#define X(id, sec, name, type, def, doc) { sec, name, type, def, doc },
	NC_KEYS(X)
#undef X
};
static const char *const sections[] = { "wifi", "bluetooth", "net", "roaming", "vpn", "firewall", "metrics", "tui", "privileges", NULL };

const char *netman_config_section(int k) { return keys[k].sec; }
const char *netman_config_name(int k) { return keys[k].name; }
const char *netman_config_type(int k) { return keys[k].type; }
const char *netman_config_default(int k) { return keys[k].def; }
const char *netman_config_doc(int k) { return keys[k].doc; }
void netman_config_fullname(int k, char *out, size_t n) { snprintf(out, n, "%s.%s", keys[k].sec, keys[k].name); }

static void seterr(char *err, size_t n, const char *fmt, ...)
{
	va_list a;
	if (!err || !n) return;
	va_start(a, fmt);
	vsnprintf(err, n, fmt, a);
	va_end(a);
}

const char *netman_config_str(const netman_config *c, int k) { return c->v[k]; }
int netman_config_bool(const netman_config *c, int k) { return !strcmp(c->v[k], "yes"); }
int netman_config_int(const netman_config *c, int k) { return atoi(c->v[k]); }

void netman_config_defaults(netman_config *c)
{
	memset(c, 0, sizeof *c);
	for (int k = 0; k < NC_N; k++) {
		snprintf(c->v[k], sizeof c->v[k], "%s", keys[k].def);
		snprintf(c->origin[k], sizeof c->origin[k], "default");
	}
}

int netman_config_find(const char *dotted)
{
	char full[96];
	for (int k = 0; k < NC_N; k++) {
		snprintf(full, sizeof full, "%s.%s", keys[k].sec, keys[k].name);
		if (!strcmp(full, dotted)) return k;
	}
	return -1;
}

static int lev(const char *a, const char *b)
{
	int la = (int)strlen(a), lb = (int)strlen(b), prev[130], cur[130];
	if (la > 120 || lb > 120) return 99;
	for (int j = 0; j <= lb; j++) prev[j] = j;
	for (int i = 1; i <= la; i++) {
		cur[0] = i;
		for (int j = 1; j <= lb; j++) {
			int d = prev[j - 1] + (a[i - 1] != b[j - 1]), x = prev[j] + 1, y = cur[j - 1] + 1;
			cur[j] = d < x ? (d < y ? d : y) : (x < y ? x : y);
		}
		memcpy(prev, cur, sizeof(int) * (size_t)(lb + 1));
	}
	return prev[lb];
}

/* closest "section.key" (or "section"); close = distance <= 2, or <= 3 for names of 8+ letters */
int netman_config_suggest(const char *name, char *out, size_t n)
{
	int best = 99;
	char full[96], cand[96] = "";
	for (int k = 0; k < NC_N; k++) {
		const char *tests[2];
		snprintf(full, sizeof full, "%s.%s", keys[k].sec, keys[k].name);
		tests[0] = full; tests[1] = keys[k].name;
		for (int t = 0; t < 2; t++) {
			int d = lev(name, tests[t]);
			if (d < best) { best = d; snprintf(cand, sizeof cand, "%s", t ? keys[k].name : full); }
		}
	}
	for (int i = 0; sections[i]; i++) { int d = lev(name, sections[i]); if (d < best) { best = d; snprintf(cand, sizeof cand, "%s", sections[i]); } }
	if (best <= (strlen(name) >= 8 ? 3 : 2) && cand[0]) { snprintf(out, n, "%s", cand); return 1; }
	out[0] = 0;
	return 0;
}

static const char *home_dir(void)
{
	const char *h = getenv("HOME"), *su = getenv("SUDO_USER");
	if (getuid() == 0 && su && *su && strcmp(su, "root")) {   /* sudo: the invoking user's config, not root's */
		struct passwd *p = getpwnam(su);
		if (p && p->pw_dir && *p->pw_dir) return p->pw_dir;
	}
	return h && *h ? h : NULL;
}

/* http://HOST[:PORT]/PATH: plain http only (the probe is a bare GET); HOST is a name or an IPv4 literal */
int netman_config_split_url(const char *url, char *host, size_t hn, int *port, char *path, size_t pn)
{
	const char *p, *he, *colon;
	char *end;
	size_t l;
	if (strncmp(url, "http://", 7)) return -1;
	p = url + 7;
	he = p;
	while (*he && *he != '/' && *he != ':') he++;
	if (he == p) return -1;
	l = (size_t)(he - p);
	if (l >= hn || l > 253) return -1;
	for (size_t i = 0; i < l; i++) if (!(isalnum((unsigned char)p[i]) || p[i] == '.' || p[i] == '-')) return -1;
	memcpy(host, p, l);
	host[l] = 0;
	*port = 80;
	if (*he == ':') {
		long v;
		colon = he + 1;
		v = strtol(colon, &end, 10);
		if (end == colon || *end != '/' || v < 1 || v > 65535) return -1;
		*port = (int)v;
		he = end;
	}
	if (*he != '/') return -1;
	if (strlen(he) >= pn) return -1;
	for (const char *q = he; *q; q++) if ((unsigned char)*q <= ' ' || *q == 0x7f) return -1;
	snprintf(path, pn, "%s", he);
	return 0;
}

int netman_config_check_value(int k, const char *v, char *norm, size_t nn, char *msg, size_t mn)
{
	const char *t = keys[k].type;
	char full[96];
	netman_config_fullname(k, full, sizeof full);
	if (!strcmp(t, "bool")) {
		static const char *const yes[] = { "yes", "true", "on", "1", NULL }, *const no[] = { "no", "false", "off", "0", NULL };
		for (int i = 0; yes[i]; i++) if (!strcasecmp(v, yes[i])) { snprintf(norm, nn, "yes"); return 0; }
		for (int i = 0; no[i]; i++) if (!strcasecmp(v, no[i])) { snprintf(norm, nn, "no"); return 0; }
		seterr(msg, mn, "%s must be yes or no (got '%s')", full, v);
		return -1;
	}
	if (!strncmp(t, "enum:", 5)) {
		const char *p = t + 5;
		size_t vl = strlen(v);
		while (*p) {
			const char *e = strchr(p, '|');
			size_t l = e ? (size_t)(e - p) : strlen(p);
			if (l == vl && !strncmp(p, v, l)) { snprintf(norm, nn, "%s", v); return 0; }
			p += l + (e ? 1 : 0);
			if (!e) break;
		}
		seterr(msg, mn, "%s must be one of %s (got '%s')", full, t + 5, v);
		return -1;
	}
	if (!strncmp(t, "int:", 4)) {
		long lo = strtol(t + 4, NULL, 10), hi = strtol(strchr(t + 4, ':') + 1, NULL, 10), n;
		char *end;
		errno = 0;
		n = strtol(v, &end, 10);
		if (!*v || *end || errno || n < lo || n > hi) { seterr(msg, mn, "%s must be a whole number from %ld to %ld (got '%s')", full, lo, hi, v); return -1; }
		snprintf(norm, nn, "%ld", n);
		return 0;
	}
	if (!strcmp(t, "path")) {
		char tmp[600];
		const char *h;
		if (!*v) { seterr(msg, mn, "%s must not be empty", full); return -1; }
		if (v[0] == '~' && (v[1] == '/' || !v[1])) {
			if (!(h = home_dir())) { seterr(msg, mn, "%s uses ~ but HOME is not set", full); return -1; }
			snprintf(tmp, sizeof tmp, "%s%s", h, v + 1);
			v = tmp;
		}
		if (v[0] != '/') { seterr(msg, mn, "%s must be an absolute path (got '%s')", full, v); return -1; }
		if (strlen(v) >= 255 || strlen(v) >= nn) { seterr(msg, mn, "%s is longer than 254 bytes", full); return -1; }
		if (strstr(v, "/../") || (strlen(v) >= 3 && !strcmp(v + strlen(v) - 3, "/.."))) { seterr(msg, mn, "%s must not contain '..'", full); return -1; }
		snprintf(norm, nn, "%s", v);
		return 0;
	}
	if (!strcmp(t, "url")) {
		char host[128], path[128];
		int port;
		if (*v && (strlen(v) >= 255 || netman_config_split_url(v, host, sizeof host, &port, path, sizeof path))) {
			seterr(msg, mn, "%s must look like http://HOST[:PORT]/PATH, or be empty (got '%.60s')", full, v);
			return -1;
		}
		snprintf(norm, nn, "%s", v);
		return 0;
	}
	seterr(msg, mn, "internal: unknown type %s", t);
	return -1;
}

static int rssi_order(const netman_config *c, char *msg, size_t mn)
{
	int g = netman_config_int(c, NC_WIFI_RSSI_GOOD), o = netman_config_int(c, NC_WIFI_RSSI_OK), w = netman_config_int(c, NC_WIFI_RSSI_WEAK);
	if (g <= o) { seterr(msg, mn, "wifi.rssi_good (%d) must be higher than wifi.rssi_ok (%d)", g, o); return -1; }
	if (o <= w) { seterr(msg, mn, "wifi.rssi_ok (%d) must be higher than wifi.rssi_weak (%d)", o, w); return -1; }
	return 0;
}

static int valid_key_char(int ch) { return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.'; }

int netman_config_parse(netman_config *c, const char *text, size_t len, const char *fname, char *err, size_t errsz)
{
	char sec[32] = "", line[LINE_MAX_BYTES + 2], msg[400], sug[96];
	unsigned char seen[NC_N] = { 0 };
	int last_rssi_line = 0, lineno = 0, any_rssi = 0;
	size_t pos = 0;
	if (memchr(text, 0, len)) {
		int ln = 1;
		for (size_t i = 0; i < len && text[i]; i++) if (text[i] == '\n') ln++;
		seterr(err, errsz, "%s:%d: NUL byte in the file (not a text config)", fname, ln);
		return -1;
	}
	while (pos < len) {
		size_t e = pos, n;
		char *p, *eq, *key, *val, *kend, full[1100];
		int k;
		while (e < len && text[e] != '\n') e++;
		n = e - pos;
		lineno++;
		if (n > LINE_MAX_BYTES) { seterr(err, errsz, "%s:%d: line longer than %d bytes", fname, lineno, LINE_MAX_BYTES); return -1; }
		memcpy(line, text + pos, n);
		line[n] = 0;
		pos = e < len ? e + 1 : e;
		if (n && line[n - 1] == '\r') line[--n] = 0;
		p = line;
		while (*p == ' ' || *p == '\t') p++;
		if (!*p || *p == '#' || *p == ';') continue;
		if (*p == '[') {
			char *r = strchr(p, ']'), *q;
			int ok = 0;
			if (!r) { seterr(err, errsz, "%s:%d: section header without ']'", fname, lineno); return -1; }
			for (q = r + 1; *q == ' ' || *q == '\t'; q++) ;
			if (*q && *q != '#' && *q != ';') { seterr(err, errsz, "%s:%d: junk after the section header", fname, lineno); return -1; }
			*r = 0;
			p++;
			while (*p == ' ' || *p == '\t') p++;
			for (q = p + strlen(p); q > p && (q[-1] == ' ' || q[-1] == '\t'); ) *--q = 0;
			for (int i = 0; sections[i]; i++) if (!strcmp(p, sections[i])) ok = 1;
			if (!ok) {
				if (netman_config_suggest(p, sug, sizeof sug)) seterr(err, errsz, "%s:%d: unknown section [%.40s] (did you mean [%s]?)", fname, lineno, p, sug);
				else seterr(err, errsz, "%s:%d: unknown section [%.40s]", fname, lineno, p);
				return -1;
			}
			snprintf(sec, sizeof sec, "%s", p);
			continue;
		}
		eq = strchr(p, '=');
		if (!eq) { seterr(err, errsz, "%s:%d: expected key = value, got '%.40s'", fname, lineno, p); return -1; }
		key = p;
		kend = eq;
		while (kend > key && (kend[-1] == ' ' || kend[-1] == '\t')) kend--;
		*kend = 0;
		if (!*key) { seterr(err, errsz, "%s:%d: empty key", fname, lineno); return -1; }
		for (char *q = key; *q; q++) if (!valid_key_char((unsigned char)*q)) { seterr(err, errsz, "%s:%d: bad character in key '%.40s'", fname, lineno, key); return -1; }
		val = eq + 1;
		while (*val == ' ' || *val == '\t') val++;
		if (*val == '"') {   /* quoted: keeps '#' and trailing spaces; only a comment may follow the closing quote */
			char *q = strchr(val + 1, '"'), *t;
			if (!q) { seterr(err, errsz, "%s:%d: unterminated quote", fname, lineno); return -1; }
			for (t = q + 1; *t == ' ' || *t == '\t'; t++) ;
			if (*t && *t != '#' && *t != ';') { seterr(err, errsz, "%s:%d: junk after the closing quote", fname, lineno); return -1; }
			*q = 0;
			val++;
		} else {
			for (char *q = val; *q; q++) if ((*q == '#' || *q == ';') && (q == val || q[-1] == ' ' || q[-1] == '\t')) { *q = 0; break; }
			for (char *q = val + strlen(val); q > val && (q[-1] == ' ' || q[-1] == '\t'); ) *--q = 0;
		}
		if (strchr(key, '.')) snprintf(full, sizeof full, "%s", key);          /* top-level dotted form: wifi.backend */
		else snprintf(full, sizeof full, "%s.%s", sec, key);
		if (!*sec && !strchr(key, '.')) { seterr(err, errsz, "%s:%d: '%s' is outside any [section]; write [section] first or use section.%s", fname, lineno, key, key); return -1; }
		if ((k = netman_config_find(full)) < 0) {
			char want[1100];
			snprintf(want, sizeof want, "%s", strchr(key, '.') || !*sec ? key : key);
			if (netman_config_suggest(strchr(key, '.') ? key : key, sug, sizeof sug))
				seterr(err, errsz, "%s:%d: unknown key '%.60s'%s%s (did you mean '%s'?)", fname, lineno, key, *sec && !strchr(key, '.') ? " in section " : "", *sec && !strchr(key, '.') ? sec : "", sug);
			else seterr(err, errsz, "%s:%d: unknown key '%.60s'%s%s", fname, lineno, key, *sec && !strchr(key, '.') ? " in section " : "", *sec && !strchr(key, '.') ? sec : "");
			return -1;
		}
		if (seen[k]) { seterr(err, errsz, "%s:%d: %s.%s is set twice in this file", fname, lineno, keys[k].sec, keys[k].name); return -1; }
		{
			char norm[256];
			if (netman_config_check_value(k, val, norm, sizeof norm, msg, sizeof msg)) { seterr(err, errsz, "%s:%d: %s", fname, lineno, msg); return -1; }
			snprintf(c->v[k], sizeof c->v[k], "%s", norm);
		}
		seen[k] = 1;
		snprintf(c->origin[k], sizeof c->origin[k], "%.100s:%d", fname, lineno);
		if (k == NC_WIFI_RSSI_GOOD || k == NC_WIFI_RSSI_OK || k == NC_WIFI_RSSI_WEAK) { any_rssi = 1; last_rssi_line = lineno; }
	}
	if (any_rssi && rssi_order(c, msg, sizeof msg)) { seterr(err, errsz, "%s:%d: %s", fname, last_rssi_line, msg); return -1; }
	return 0;
}

int netman_config_load_file(netman_config *c, const char *path, int must_exist, char *err, size_t errsz)
{
	FILE *f = fopen(path, "r");
	char *buf = NULL;
	size_t n = 0, cap = 0;
	int rc;
	if (!f) {
		if (errno == ENOENT && !must_exist) return 0;
		seterr(err, errsz, "%s: %s", path, strerror(errno));
		return -1;
	}
	for (;;) {
		char *nb;
		size_t r;
		if (n + 4096 + 1 > cap) {
			cap = cap ? cap * 2 : 8192;
			if (cap > (1u << 20)) { fclose(f); free(buf); seterr(err, errsz, "%s: larger than 1 MiB, not a config file", path); return -1; }
			if (!(nb = realloc(buf, cap))) { fclose(f); free(buf); seterr(err, errsz, "out of memory"); return -1; }
			buf = nb;
		}
		r = fread(buf + n, 1, cap - n - 1, f);
		if (r == 0) break;
		n += r;
	}
	if (ferror(f)) { seterr(err, errsz, "%s: read error", path); fclose(f); free(buf); return -1; }
	fclose(f);
	rc = netman_config_parse(c, buf ? buf : "", n, path, err, errsz);
	free(buf);
	return rc;
}

static void envname(int k, char *out, size_t n)
{
	snprintf(out, n, "NETMAN_%s_%s", keys[k].sec, keys[k].name);
	for (char *p = out; *p; p++) *p = (char)toupper((unsigned char)*p);
}

int netman_config_env(netman_config *c, char *err, size_t errsz)
{
	char name[96], msg[400], norm[256];
	int changed = 0;
	for (int k = 0; k < NC_N; k++) {
		const char *v;
		envname(k, name, sizeof name);
		v = getenv(name);
		if ((!v || !*v) && k == NC_PRIV_SUDO) {
			if ((v = getenv("NETMAN_SUDO")) && *v) snprintf(name, sizeof name, "NETMAN_SUDO");
			else if ((v = getenv("NM_SUDO")) && *v) snprintf(name, sizeof name, "NM_SUDO");
		}
		if (!v || !*v) continue;
		if (netman_config_check_value(k, v, norm, sizeof norm, msg, sizeof msg)) { seterr(err, errsz, "environment %s: %s", name, msg); return -1; }
		snprintf(c->v[k], sizeof c->v[k], "%s", norm);
		snprintf(c->origin[k], sizeof c->origin[k], "env %s", name);
		changed = 1;
	}
	if (changed && rssi_order(c, msg, sizeof msg)) { seterr(err, errsz, "environment: %s", msg); return -1; }
	return 0;
}

const char *netman_config_user_path(char *buf, size_t n)
{
	const char *one = getenv("NETMAN_CONFIG"), *xdg = getenv("XDG_CONFIG_HOME"), *h;
	if (one && *one) snprintf(buf, n, "%s", one);
	else if (xdg && *xdg == '/') snprintf(buf, n, "%s/netman/netman.conf", xdg);
	else if ((h = home_dir())) snprintf(buf, n, "%s/.config/netman/netman.conf", h);
	else return NULL;
	return buf;
}

int netman_config_load(netman_config *c, char *err, size_t errsz)
{
	const char *one = getenv("NETMAN_CONFIG");
	char path[600];
	netman_config_defaults(c);
	if (one && *one) {
		if (netman_config_load_file(c, one, 1, err, errsz)) return -1;
	} else {
		if (netman_config_load_file(c, "/etc/netman/netman.conf", 0, err, errsz)) return -1;
		if (netman_config_user_path(path, sizeof path) && netman_config_load_file(c, path, 0, err, errsz)) return -1;
	}
	return netman_config_env(c, err, errsz);
}

void netman_config_show(const netman_config *c, FILE *o)
{
	const char *cur = "";
	for (int k = 0; k < NC_N; k++) {
		char val[300];
		if (strcmp(cur, keys[k].sec)) { fprintf(o, "%s[%s]\n", *cur ? "\n" : "", keys[k].sec); cur = keys[k].sec; }
		if (!strcmp(keys[k].type, "url") || !strcmp(keys[k].type, "path")) snprintf(val, sizeof val, "\"%s\"", c->v[k]);
		else snprintf(val, sizeof val, "%s", c->v[k]);
		fprintf(o, "%-16s = %-48s # %s\n", keys[k].name, val, c->origin[k]);
	}
}

void netman_config_example(FILE *o)
{
	const char *cur = "";
	fputs("# netman.conf - the settings file of netman. Copy to /etc/netman/netman.conf (system) or ~/.config/netman/netman.conf (you; wins).\n"
	      "# Every key is shown with its default, commented out. Uncomment and change what you want, or use:\n"
	      "#     netman config set wifi.backend iwd        netman config toggle vpn.killswitch        netman config show        netman config check\n"
	      "# Environment variables override the files: NETMAN_<SECTION>_<KEY>, e.g. NETMAN_WIFI_BACKEND=wpa.\n"
	      "# Booleans: yes|no (true/false/on/off/1/0 are accepted). A value with '#' in it must be \"quoted\".\n", o);
	for (int k = 0; k < NC_N; k++) {
		const char *t = keys[k].type;
		if (strcmp(cur, keys[k].sec)) { fprintf(o, "\n[%s]\n", keys[k].sec); cur = keys[k].sec; }
		fprintf(o, "# %s", keys[k].doc);
		if (!strncmp(t, "enum:", 5)) fprintf(o, " (%s)", t + 5);
		else if (!strncmp(t, "int:", 4)) { long lo = strtol(t + 4, NULL, 10), hi = strtol(strchr(t + 4, ':') + 1, NULL, 10); fprintf(o, " (%ld..%ld)", lo, hi); }
		fputc('\n', o);
		fprintf(o, "# %s = %s\n", keys[k].name, !strcmp(t, "url") || !strcmp(t, "path") ? (!*keys[k].def ? "\"\"" : keys[k].def) : keys[k].def);
	}
}

/* ---- editing ------------------------------------------------------------------------------------------------------ */

typedef struct { char *b; size_t n, cap; } Sb;
static void sb_add(Sb *s, const char *p, size_t n)
{
	if (s->n + n + 1 > s->cap) { s->cap = (s->n + n + 1) * 2; if (!(s->b = realloc(s->b, s->cap))) abort(); }
	memcpy(s->b + s->n, p, n);
	s->n += n;
	s->b[s->n] = 0;
}
static void sb_str(Sb *s, const char *p) { sb_add(s, p, strlen(p)); }

static void quote_if_needed(const char *v, char *out, size_t n)
{
	int q = !*v || strchr(v, '#') || strchr(v, ';') || *v == ' ' || v[strlen(v) - 1] == ' ' || *v == '"';
	if (q) snprintf(out, n, "\"%s\"", v); else snprintf(out, n, "%s", v);
}

static int write_atomic(const char *path, const char *data, size_t n, char *err, size_t errsz)
{
	char dir[600], tmp[640], base[600];
	struct stat st;
	int fd;
	mode_t mode = 0644;
	snprintf(base, sizeof base, "%s", path);
	snprintf(dir, sizeof dir, "%s", dirname(base));
	if (stat(dir, &st) && errno == ENOENT) {   /* ~/.config/netman may not exist yet */
		char parent[600], b2[600];
		snprintf(b2, sizeof b2, "%s", dir);
		snprintf(parent, sizeof parent, "%s", dirname(b2));
		if (stat(parent, &st) && errno == ENOENT) mkdir(parent, 0755);
		if (mkdir(dir, 0755) && errno != EEXIST) { seterr(err, errsz, "%s: %s", dir, strerror(errno)); return -1; }
	}
	if (!stat(path, &st)) mode = st.st_mode & 0777;
	snprintf(tmp, sizeof tmp, "%s/.netman.conf.XXXXXX", dir);
	if ((fd = mkstemp(tmp)) < 0) { seterr(err, errsz, "%s: %s (system file? run with sudo and set --system, or edit by hand)", dir, strerror(errno)); return -1; }
	if (fchmod(fd, mode) || write(fd, data, n) != (ssize_t)n || fsync(fd) || close(fd)) {
		seterr(err, errsz, "%s: %s", tmp, strerror(errno));
		unlink(tmp);
		return -1;
	}
	if (rename(tmp, path)) { seterr(err, errsz, "%s: %s", path, strerror(errno)); unlink(tmp); return -1; }
	return 0;
}

int netman_config_set_file(const char *path, const char *dotted, const char *value, char *err, size_t errsz)
{
	int k = netman_config_find(dotted);
	char norm[256], msg[400], sug[96], newval[300], *text = NULL;
	size_t len = 0;
	FILE *f;
	Sb out = { 0 };
	const char *p, *end;
	char cur[32] = "";
	int done = 0;
	size_t insert_at = (size_t)-1;   /* byte offset in out after the last line of the target section */
	if (k < 0) {
		if (netman_config_suggest(dotted, sug, sizeof sug)) seterr(err, errsz, "unknown key '%s' (did you mean '%s'?)", dotted, sug);
		else seterr(err, errsz, "unknown key '%s' (see: netman config example)", dotted);
		return -1;
	}
	if (netman_config_check_value(k, value, norm, sizeof norm, msg, sizeof msg)) { seterr(err, errsz, "%s", msg); return -1; }
	quote_if_needed(norm, newval, sizeof newval);
	if ((f = fopen(path, "r"))) {
		size_t cap = 0, r;
		while (1) {
			if (len + 4097 > cap) { cap = cap ? cap * 2 : 8192; if (cap > (1u << 20)) { fclose(f); free(text); seterr(err, errsz, "%s: too large", path); return -1; } text = realloc(text, cap); if (!text) abort(); }
			r = fread(text + len, 1, cap - len - 1, f);
			if (!r) break;
			len += r;
		}
		fclose(f);
		text[len] = 0;
	} else if (errno != ENOENT) { seterr(err, errsz, "%s: %s", path, strerror(errno)); return -1; }
	if (!text) { text = calloc(1, 1); len = 0; }
	p = text; end = text + len;
	while (p < end) {
		const char *e = memchr(p, '\n', (size_t)(end - p)), *q = p;
		size_t ll = e ? (size_t)(e - p) : (size_t)(end - p);
		char line[1100];
		int replaced = 0;
		if (ll > 1023) ll = 1023;   /* longer lines are rejected by the re-parse below */
		memcpy(line, p, ll); line[ll] = 0;
		while (*q == ' ' || *q == '\t') q++;
		if (*q == '[') {
			char *r = strchr(line, ']');
			if (r) { char *s = strchr(line, '[') + 1; *r = 0; while (*s == ' ') s++; snprintf(cur, sizeof cur, "%s", s); for (char *t = cur + strlen(cur); t > cur && t[-1] == ' '; ) *--t = 0; }
		} else if (*q && *q != '#' && *q != ';') {
			char *eq = strchr(line, '='), *ks = line;
			if (eq) {
				char kname[200], full[240];
				size_t kl;
				while (*ks == ' ' || *ks == '\t') ks++;
				kl = (size_t)(eq - ks);
				while (kl && (ks[kl - 1] == ' ' || ks[kl - 1] == '\t')) kl--;
				snprintf(kname, sizeof kname, "%.*s", (int)(kl > 199 ? 199 : kl), ks);
				if (strchr(kname, '.')) snprintf(full, sizeof full, "%s", kname); else snprintf(full, sizeof full, "%s.%s", cur, kname);
				if (!strcmp(full, dotted) && !done) {
					const char *vs = eq + 1, *cm = NULL;
					char *a;
					while (*vs == ' ' || *vs == '\t') vs++;
					if (*vs == '"') { const char *cq = strchr(vs + 1, '"'); a = cq ? (char *)cq + 1 : (char *)vs + strlen(vs); } else a = (char *)vs;
					for (const char *t = a; *t; t++) if ((*t == '#' || *t == ';') && (t == vs || t[-1] == ' ' || t[-1] == '\t')) { cm = t; break; }
					sb_add(&out, line, (size_t)(vs - line));
					sb_str(&out, newval);
					if (cm) { sb_str(&out, "   "); sb_str(&out, cm); }
					replaced = done = 1;
				} else if (!strcmp(cur, keys[k].sec) || (strchr(kname, '.') && !strncmp(kname, keys[k].sec, strlen(keys[k].sec)))) {
					/* a key of the target section: new keys go after the last one */
				}
			}
		}
		if (!replaced) sb_add(&out, p, ll);
		if (e) sb_add(&out, "\n", 1);
		if (!replaced && !strcmp(cur, keys[k].sec) && *q && *q != '#' && *q != ';' && *q != '[') insert_at = out.n;
		else if (!strcmp(cur, keys[k].sec) && *q == '[' && insert_at == (size_t)-1) insert_at = out.n;   /* just after the header of an empty section */
		p = e ? e + 1 : end;
	}
	if (!done) {
		Sb ins = { 0 };
		char l2[400];
		snprintf(l2, sizeof l2, "%s = %s\n", keys[k].name, newval);
		if (insert_at != (size_t)-1) {
			sb_add(&ins, out.b, insert_at);
			if (insert_at && out.b[insert_at - 1] != '\n') sb_add(&ins, "\n", 1);
			sb_str(&ins, l2);
			sb_add(&ins, out.b + insert_at, out.n - insert_at);
		} else {
			sb_add(&ins, out.b ? out.b : "", out.n);
			if (out.n && out.b[out.n - 1] != '\n') sb_add(&ins, "\n", 1);
			if (out.n) sb_add(&ins, "\n", 1);
			snprintf(msg, sizeof msg, "[%s]\n", keys[k].sec);
			sb_str(&ins, msg);
			sb_str(&ins, l2);
		}
		free(out.b);
		out = ins;
	}
	{
		netman_config chk;
		char perr[500];
		netman_config_defaults(&chk);
		if (netman_config_parse(&chk, out.b ? out.b : "", out.n, path, perr, sizeof perr)) {
			seterr(err, errsz, "refusing to write: the result would not load: %s", perr);
			free(out.b); free(text);
			return -1;
		}
	}
	{
		int rc = write_atomic(path, out.b ? out.b : "", out.n, err, errsz);
		free(out.b); free(text);
		return rc;
	}
}

int netman_config_toggle_file(const char *path, const char *dotted, char *err, size_t errsz)
{
	int k = netman_config_find(dotted);
	netman_config c;
	char sug[96], e2[300];
	if (k < 0) {
		if (netman_config_suggest(dotted, sug, sizeof sug)) seterr(err, errsz, "unknown key '%s' (did you mean '%s'?)", dotted, sug);
		else seterr(err, errsz, "unknown key '%s'", dotted);
		return -1;
	}
	if (strcmp(keys[k].type, "bool")) { seterr(err, errsz, "%s is not a yes/no switch (type %s); use: netman config set %s VALUE", dotted, keys[k].type, dotted); return -1; }
	if (netman_config_load(&c, e2, sizeof e2)) { seterr(err, errsz, "%s", e2); return -1; }
	return netman_config_set_file(path, dotted, netman_config_bool(&c, k) ? "no" : "yes", err, errsz);
}
