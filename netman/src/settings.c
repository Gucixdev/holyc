/* settings.c - the settings schema and the `netman settings` operations. See settings.h. Plain C, depends only on config.h/config.c.
 *
 * What is added to NC_KEYS (config.h): the sections table below (tab, root) and one short exception list (restart). Add a key to
 * NC_KEYS and it shows up in every UI with its type, range, default, description and source; only a NEW SECTION needs a line here
 * (tests/settings_test.c fails until it has one). The "gate" of a section is its own `enabled` key: while it is `no` the other keys of
 * the section are hidden in the panel and in menus (listed with --all).
 */
#define _GNU_SOURCE
#include "settings.h"
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>

/* section -> tab title, needs root. Order = order of the tabs in the panel. */
static const struct { const char *sec, *tab; int root; } nset_secs[] = {
	{ "wifi", "Wi-Fi", 0 }, { "bluetooth", "Bluetooth", 0 }, { "net", "Network", 1 }, { "roaming", "Network", 1 },
	{ "vpn", "VPN", 1 }, { "wireguard", "VPN", 1 }, { "firewall", "Firewall", 1 },
	{ "enterprise", "Enterprise", 1 }, { "licence", "Enterprise", 1 }, { "tether", "Enterprise", 1 },
	{ "tor", "Tor", 1 }, { "ssh", "SSH", 0 }, { "mesh", "Mesh", 1 }, { "bonding", "Bond", 1 }, { "tunnel", "Tunnel", 1 },
	{ "metrics", "Metrics", 0 }, { "nmapi", "Daemon", 1 }, { "tui", "Interface", 0 }, { "privileges", "Interface", 0 },
};
#define NSET_NSECS ((int)(sizeof nset_secs / sizeof *nset_secs))

/* keys whose running consumer reads them only at start (a daemon, a tor process, a live ECMP loop) */
static const int nset_restart_keys[] = { NC_NMAPI_ENABLED, NC_NMAPI_REFRESH, NC_TOR_MODE, NC_TOR_SOCKS, NC_TOR_TRANS, NC_TOR_DNS, NC_TOR_CONTROL, NC_TOR_DIR,
	NC_TOR_USER, NC_TOR_UIDS, NC_TOR_IFACES, NC_TOR_OBFS4, NC_TOR_SNOW, NC_SSH_STATE, NC_NET_STATE_DIR, NC_ENT_PROBE_MS };

static int sec_of(int k)
{
	const char *s = netman_config_section(k);
	for (int i = 0; i < NSET_NSECS; i++) if (!strcmp(nset_secs[i].sec, s)) return i;
	return -1;
}

int nset_nsecs(void) { return NSET_NSECS; }
const char *nset_secname(int i) { return nset_secs[i].sec; }

int nset_kind(int k)
{
	const char *t = netman_config_type(k);
	if (!strcmp(t, "bool")) return NSET_BOOL;
	if (!strncmp(t, "int:", 4)) return NSET_INT;
	if (!strncmp(t, "enum:", 5)) return NSET_ENUM;
	if (!strcmp(t, "path")) return NSET_PATH;
	if (!strcmp(t, "cmd")) return NSET_CMD;
	return NSET_STRING;
}
const char *nset_kind_name(int k) { static const char *const n[] = { "bool", "int", "enum", "string", "path", "cmd" }; return n[nset_kind(k)]; }
const char *nset_format(int k) { const char *t = netman_config_type(k); return !strcmp(t, "url") ? "url" : !strcmp(t, "list") ? "list" : ""; }

int nset_range(int k, long *lo, long *hi)
{
	const char *t = netman_config_type(k);
	if (strncmp(t, "int:", 4)) return 0;
	*lo = strtol(t + 4, NULL, 10);
	*hi = strtol(strchr(t + 4, ':') + 1, NULL, 10);
	return 1;
}

int nset_choices(int k, char out[][24], int max)
{
	const char *t = netman_config_type(k), *p;
	int n = 0;
	if (nset_kind(k) == NSET_BOOL) { if (max > 1) { snprintf(out[0], 24, "yes"); snprintf(out[1], 24, "no"); } return 2; }
	if (strncmp(t, "enum:", 5)) return 0;
	for (p = t + 5; *p && n < max;) {
		const char *e = strchr(p, '|');
		size_t l = e ? (size_t)(e - p) : strlen(p);
		snprintf(out[n++], 24, "%.*s", (int)(l > 23 ? 23 : l), p);
		if (!e) break;
		p = e + 1;
	}
	return n;
}

const char *nset_tab(int k) { int s = sec_of(k); return s < 0 ? "Other" : nset_secs[s].tab; }
static int tab_dup(int i) { for (int j = 0; j < i; j++) if (!strcmp(nset_secs[i].tab, nset_secs[j].tab)) return 1; return 0; }
int nset_ntabs(void) { int n = 0; for (int i = 0; i < NSET_NSECS; i++) if (!tab_dup(i)) n++; return n; }
const char *nset_tabname(int idx)
{
	int n = 0;
	for (int i = 0; i < NSET_NSECS; i++) if (!tab_dup(i) && n++ == idx) return nset_secs[i].tab;
	return "";
}
int nset_tab_index(int k)
{
	const char *t = nset_tab(k);
	for (int i = 0, n = nset_ntabs(); i < n; i++) if (!strcmp(nset_tabname(i), t)) return i;
	return 0;
}
int nset_needs_root(int k) { int s = sec_of(k); return s >= 0 && nset_secs[s].root; }
int nset_restart(int k)
{
	for (unsigned i = 0; i < sizeof nset_restart_keys / sizeof *nset_restart_keys; i++) if (nset_restart_keys[i] == k) return 1;
	return 0;
}
const char *nset_apply(int k) { return nset_restart(k) ? "restart" : "live"; }
int nset_is_gate(int k) { return !strcmp(netman_config_name(k), "enabled"); }
int nset_gate_key(int k)
{
	char full[96];
	snprintf(full, sizeof full, "%s.enabled", netman_config_section(k));
	return netman_config_find(full);
}
int nset_hidden(const netman_config *c, int k)
{
	int g;
	if (nset_is_gate(k)) return 0;
	g = nset_gate_key(k);
	return g >= 0 && !netman_config_bool(c, g);
}

void nset_short(int k, char *out, size_t n)
{
	const char *d = netman_config_doc(k), *e = d;
	size_t l;
	while (*e) {   /* up to the end of the first sentence, else the whole text */
		if ((e[0] == '.' || e[0] == ';') && (e[1] == ' ' || !e[1])) break;
		e++;
	}
	l = (size_t)(e - d);
	if (l > 70) {   /* too long for a menu line: cut at the last clause break (: , ( ;) before 70, else at a space */
		size_t b = 70;
		while (b > 25 && d[b] != ',' && d[b] != ':' && d[b] != '(' && d[b] != ';') b--;
		if (b > 25) l = b;
		else { l = 70; while (l > 25 && d[l] != ' ') l--; }
		while (l && d[l - 1] == ' ') l--;
	}
	if (l >= n) l = n - 1;
	memcpy(out, d, l);
	out[l] = 0;
}

int nset_find(const char *key) { return netman_config_find(key); }
int nset_is_default(const netman_config *c, int k) { return !strcmp(netman_config_str(c, k), netman_config_default(k)); }

const char *nset_source(const netman_config *c, int k)
{
	const char *o = c->origin[k];
	if (!strcmp(o, "default")) return "default";
	if (!strncmp(o, "env ", 4)) return "env";
	if (!strncmp(o, "/etc/", 5)) return "system";
	return "user";
}

/* ---- helpers ------------------------------------------------------------------------------------------------------- */

static void seterr(char *err, size_t n, const char *fmt, ...)
{
	va_list a;
	if (!err || !n) return;
	va_start(a, fmt);
	vsnprintf(err, n, fmt, a);
	va_end(a);
}

static void jstr(FILE *o, const char *s)
{
	fputc('"', o);
	for (; *s; s++) {
		unsigned char ch = (unsigned char)*s;
		if (ch == '"' || ch == '\\') { fputc('\\', o); fputc(ch, o); }
		else if (ch == '\n') fputs("\\n", o);
		else if (ch == '\t') fputs("\\t", o);
		else if (ch < 0x20 || ch == 0x7f) fprintf(o, "\\u%04x", ch);
		else fputc(ch, o);
	}
	fputc('"', o);
}

/* the value as shown in lists and menus: empty strings as "" */
static const char *shown(const netman_config *c, int k) { const char *v = netman_config_str(c, k); return *v ? v : "\"\""; }

static int match_section(int k, const char *section)
{
	if (!section || !*section) return 1;
	return !strcmp(netman_config_section(k), section) || !strcasecmp(nset_tab(k), section);
}

static int resolve(const char *key, char *err, size_t en)
{
	int k = netman_config_find(key);
	char sug[96];
	if (k >= 0) return k;
	if (netman_config_suggest(key, sug, sizeof sug)) seterr(err, en, "unknown setting '%s' (did you mean '%s'?)", key, sug);
	else seterr(err, en, "unknown setting '%s' (netman settings list)", key);
	return -1;
}

/* ---- CLI operations ------------------------------------------------------------------------------------------------ */

int nset_list(const netman_config *c, FILE *o, const char *section, int all)
{
	int n = 0;
	for (int k = 0; k < NC_N; k++) {
		char full[96], sh[100];
		if (!match_section(k, section) || (!all && nset_hidden(c, k))) continue;
		netman_config_fullname(k, full, sizeof full);
		nset_short(k, sh, sizeof sh);
		fprintf(o, "%-26s %-24s %-8s %s%s%s\n", full, shown(c, k), nset_source(c, k), sh, nset_restart(k) ? " [restart]" : "", nset_needs_root(k) ? " [root]" : "");
		n++;
	}
	return n ? 0 : 1;
}

int nset_get(const netman_config *c, const char *key, FILE *o, char *err, size_t en)
{
	int k = resolve(key, err, en);
	if (k < 0) return 1;
	fprintf(o, "%s\n", netman_config_str(c, k));
	return 0;
}

static void after_write(const char *key, const char *path, const char *what, FILE *o)
{
	netman_config c;
	char err[300];
	int k = netman_config_find(key);
	fprintf(o, "%s  (%s)\n", what, path);
	if (k < 0) return;
	if (nset_restart(k)) fprintf(o, "note: %s is read at start: restart needed\n", key);
	if (!netman_config_load(&c, err, sizeof err) && !strcmp(nset_source(&c, k), "env")) fprintf(o, "note: %s is overridden by %s\n", key, c.origin[k] + 4);
}

int nset_set(const char *path, const char *key, const char *value, FILE *o, char *err, size_t en)
{
	char what[400];
	if (resolve(key, err, en) < 0) return 1;
	if (netman_config_set_file(path, key, value, err, en)) return 1;
	snprintf(what, sizeof what, "%s = %s", key, value);
	after_write(key, path, what, o);
	return 0;
}

int nset_toggle(const char *path, const char *key, FILE *o, char *err, size_t en)
{
	int k = resolve(key, err, en);
	char what[400];
	if (k < 0) return 1;
	if (nset_kind(k) != NSET_BOOL) { seterr(err, en, "%s is not a yes/no switch (%s); use: netman settings set %s VALUE", key, nset_kind_name(k), key); return 1; }
	if (netman_config_toggle_file(path, key, err, en)) return 1;
	snprintf(what, sizeof what, "%s toggled", key);
	after_write(key, path, what, o);
	return 0;
}

int nset_reset(const char *path, const char *key, FILE *o, char *err, size_t en)
{
	int k = resolve(key, err, en), rc;
	char what[400];
	if (k < 0) return 1;
	rc = netman_config_unset_file(path, key, err, en);
	if (rc < 0) return 1;
	snprintf(what, sizeof what, rc ? "%s was not set in the file (default %s)" : "%s reset to the default (%s)", key, *netman_config_default(k) ? netman_config_default(k) : "\"\"");
	after_write(key, path, what, o);
	return 0;
}

int nset_check(FILE *o, int json)
{
	netman_config c;
	char err[512];
	int rc = netman_config_load(&c, err, sizeof err), nenv = 0, nfile = 0;
	if (rc) {
		if (json) { fputs("{\"ok\":false,\"error\":", o); jstr(o, err); fputs("}\n", o); } else fprintf(o, "%s\n", err);
		return 1;
	}
	for (int k = 0; k < NC_N; k++) { const char *s = nset_source(&c, k); if (!strcmp(s, "env")) nenv++; else if (strcmp(s, "default")) nfile++; }
	if (json) {
		int first = 1;
		fprintf(o, "{\"ok\":true,\"keys\":%d,\"set_in_files\":%d,\"from_env\":%d,\"env_overrides\":[", NC_N, nfile, nenv);
		for (int k = 0; k < NC_N; k++) {
			char full[96];
			if (strcmp(nset_source(&c, k), "env")) continue;
			netman_config_fullname(k, full, sizeof full);
			fprintf(o, "%s{\"key\":", first ? "" : ",");
			jstr(o, full); fputs(",\"origin\":", o); jstr(o, c.origin[k]); fputc('}', o);
			first = 0;
		}
		fputs("]}\n", o);
	} else {
		fprintf(o, "settings ok (%d keys, %d set in files, %d from the environment)\n", NC_N, nfile, nenv);
		for (int k = 0; k < NC_N; k++) {
			char full[96];
			if (strcmp(nset_source(&c, k), "env")) continue;
			netman_config_fullname(k, full, sizeof full);
			fprintf(o, "note: %s comes from %s\n", full, c.origin[k] + 4);
		}
	}
	return 0;
}

void nset_dump_api(const netman_config *c, const char *path, FILE *o)
{
	fputs("{\"api\":1,\"program\":\"netman\",\"config_file\":", o);
	jstr(o, path ? path : "");
	fputs(",\"env_prefix\":\"NETMAN_\",\"apply\":[\"live\",\"restart\"],\"sources\":[\"default\",\"system\",\"user\",\"env\"],\"tabs\":[", o);
	for (int i = 0, n = nset_ntabs(); i < n; i++) {
		int first = 1;
		fprintf(o, "%s{\"id\":%d,\"name\":", i ? "," : "", i);
		jstr(o, nset_tabname(i));
		fputs(",\"sections\":[", o);
		for (int s = 0; s < NSET_NSECS; s++) if (!strcmp(nset_secs[s].tab, nset_tabname(i))) { fputs(first ? "" : ",", o); jstr(o, nset_secs[s].sec); first = 0; }
		fputs("]}", o);
	}
	fputs("],\"settings\":[", o);
	for (int k = 0; k < NC_N; k++) {
		char full[96], sh[100], ch[40][24];
		long lo, hi;
		int nch = nset_choices(k, ch, 40), g = nset_gate_key(k);
		netman_config_fullname(k, full, sizeof full);
		nset_short(k, sh, sizeof sh);
		fprintf(o, "%s\n{\"key\":", k ? "," : "");
		jstr(o, full);
		fputs(",\"section\":", o); jstr(o, netman_config_section(k));
		fputs(",\"name\":", o); jstr(o, netman_config_name(k));
		fputs(",\"tab\":", o); jstr(o, nset_tab(k));
		fputs(",\"type\":", o); jstr(o, nset_kind_name(k));
		if (*nset_format(k)) { fputs(",\"format\":", o); jstr(o, nset_format(k)); }
		if (nset_range(k, &lo, &hi)) fprintf(o, ",\"min\":%ld,\"max\":%ld", lo, hi);
		if (nch) {
			fputs(",\"choices\":[", o);
			for (int i = 0; i < nch; i++) { fputs(i ? "," : "", o); jstr(o, ch[i]); }
			fputc(']', o);
		}
		fputs(",\"default\":", o); jstr(o, netman_config_default(k));
		fputs(",\"value\":", o); jstr(o, netman_config_str(c, k));
		fputs(",\"source\":", o); jstr(o, nset_source(c, k));
		fputs(",\"origin\":", o); jstr(o, c->origin[k]);
		fputs(",\"description\":", o); jstr(o, sh);
		fputs(",\"doc\":", o); jstr(o, netman_config_doc(k));
		fprintf(o, ",\"needs_root\":%s,\"restart\":%s,\"apply\":\"%s\"", nset_needs_root(k) ? "true" : "false", nset_restart(k) ? "true" : "false", nset_apply(k));
		fputs(",\"gate\":", o);
		if (nset_is_gate(k) || g < 0) fputs("null", o);
		else { char gf[96]; netman_config_fullname(g, gf, sizeof gf); jstr(o, gf); }
		fprintf(o, ",\"hidden\":%s}", nset_hidden(c, k) ? "true" : "false");
	}
	fputs("\n]}\n", o);
}

/* desktop's settings.schema: key|type|extra|apply|default|label (types bool number text choice) */
void nset_schema(FILE *o)
{
	for (int k = 0; k < NC_N; k++) {
		char full[96], sh[100], ch[40][24];
		long lo, hi;
		int nch;
		netman_config_fullname(k, full, sizeof full);
		nset_short(k, sh, sizeof sh);
		fprintf(o, "%s|", full);
		switch (nset_kind(k)) {
		case NSET_BOOL: fprintf(o, "bool||%s|%s|", nset_apply(k), netman_config_default(k)); break;
		case NSET_INT: nset_range(k, &lo, &hi); fprintf(o, "number|%ld:%ld|%s|%s|", lo, hi, nset_apply(k), netman_config_default(k)); break;
		case NSET_ENUM:
			nch = nset_choices(k, ch, 40);
			fputs("choice|", o);
			for (int i = 0; i < nch; i++) fprintf(o, "%s%s", i ? "," : "", ch[i]);
			fprintf(o, "|%s|%s|", nset_apply(k), netman_config_default(k));
			break;
		default: fprintf(o, "text||%s|%s|", nset_apply(k), netman_config_default(k)); break;
		}
		fprintf(o, "%s\n", sh);
	}
}

void nset_menu(const netman_config *c, FILE *o, const char *section, int all)
{
	for (int k = 0; k < NC_N; k++) {
		char full[96], sh[100];
		if (!match_section(k, section) || (!all && nset_hidden(c, k))) continue;
		netman_config_fullname(k, full, sizeof full);
		nset_short(k, sh, sizeof sh);
		fprintf(o, "%s = %s  # %s\n", full, shown(c, k), sh);
	}
}

/* ---- pick: the chosen menu line -> value menu -> set ---------------------------------------------------------------- */

/* run the menu command with ITEMS (newline separated) on stdin; the chosen line goes to out. 0 chosen, 1 nothing chosen, -1 cannot run */
static int run_menu(const char *prompt, const char *items, char *out, size_t n, char *err, size_t en)
{
	int in[2], ou[2], st;
	pid_t pid;
	const char *cmd = getenv("NETMAN_MENU");
	ssize_t r;
	size_t got = 0;
	if (!cmd || !*cmd) cmd = "dmenu -p \"$NETMAN_MENU_PROMPT\"";
	if (pipe(in) || pipe(ou)) { seterr(err, en, "pipe: %s", strerror(errno)); return -1; }
	signal(SIGPIPE, SIG_IGN);
	if ((pid = fork()) < 0) { seterr(err, en, "fork: %s", strerror(errno)); return -1; }
	if (!pid) {
		dup2(in[0], 0); dup2(ou[1], 1);
		close(in[0]); close(in[1]); close(ou[0]); close(ou[1]);
		setenv("NETMAN_MENU_PROMPT", prompt, 1);
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
	close(in[0]); close(ou[1]);
	if (write(in[1], items, strlen(items)) < 0) {}
	close(in[1]);
	while (got + 1 < n && (r = read(ou[0], out + got, n - 1 - got)) > 0) got += (size_t)r;
	close(ou[0]);
	waitpid(pid, &st, 0);
	out[got] = 0;
	while (got && (out[got - 1] == '\n' || out[got - 1] == '\r')) out[--got] = 0;
	if (WIFEXITED(st) && WEXITSTATUS(st) == 127) { seterr(err, en, "cannot run the menu command (NETMAN_MENU, default dmenu); use: netman settings pick --choices, or --value VALUE"); return -1; }
	return got ? 0 : 1;
}

int nset_pick(const netman_config *c, const char *path, FILE *in, FILE *o, int choices_only, const char *value, char *err, size_t en)
{
	char line[600], key[96], items[1500] = "", chosen[300], ch[40][24];
	char *p;
	int k, nch, rc;
	if (!fgets(line, sizeof line, in)) { seterr(err, en, "pick: no line on standard input (use: netman settings menu | dmenu | netman settings pick)"); return 1; }
	line[strcspn(line, "\r\n")] = 0;
	if ((p = strstr(line, " = "))) snprintf(key, sizeof key, "%.*s", (int)(p - line > 95 ? 95 : p - line), line);
	else snprintf(key, sizeof key, "%s", line);   /* a bare key works too */
	if (netman_config_find(key) < 0) { seterr(err, en, "pick: not a settings line: '%.60s'", line); return 1; }
	if ((k = resolve(key, err, en)) < 0) return 1;
	nch = nset_choices(k, ch, 40);
	if (choices_only) {
		for (int i = 0; i < nch; i++) fprintf(o, "%s\n", ch[i]);
		if (!nch) fprintf(o, "%s\n", netman_config_str(c, k));
		return 0;
	}
	if (value) snprintf(chosen, sizeof chosen, "%s", value);
	else {
		if (nch) for (int i = 0; i < nch; i++) { strncat(items, ch[i], sizeof items - strlen(items) - 2); strcat(items, "\n"); }
		else snprintf(items, sizeof items, "%s\n", netman_config_str(c, k));   /* free text: the menu shows the current value, you type the new one */
		if ((rc = run_menu(key, items, chosen, sizeof chosen, err, en)) != 0) return rc < 0 ? 1 : 0;   /* nothing chosen = cancelled, not an error */
	}
	return nset_set(path, key, chosen, o, err, en);
}
