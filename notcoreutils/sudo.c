/* sudo - drop-in replacement for sudo(8): same command line, same /etc/sudoers rules, PAM login, per-tty timestamps.
 * What it leaves out is the pain: no plugin system, no LDAP, no I/O logging, no 40 pages of options. Defaults it knows are listed
 * in apply_default(); unknown ones are ignored, so a stock sudoers file loads. Must be installed setuid root (chown root; chmod 4755).
 *
 *   sudo [-u USER] [-g GROUP] [-E] [-H] [-n] [-S] [-b] [--] COMMAND [ARGS...]
 *   sudo -i | -s [COMMAND]       login shell / shell of the target user
 *   sudo -l [COMMAND]            what may I run?      sudo -v refresh timestamp   sudo -k / -K forget it
 *   sudo --check [FILE]          check a sudoers file (like visudo -c)       sudo --tui   manage rules (see tui section)
 *   sudo -V | -h
 *
 * Rules: the sudoers grammar for  USERS HOSTS = (RUNAS[:GROUPS]) [NOPASSWD:|PASSWD:|SETENV:|NOEXEC:] CMD, CMD ...  with aliases
 * (User_Alias Runas_Alias Host_Alias Cmnd_Alias), ! negation, %group, #uid, wildcards, directories, the last matching rule wins,
 * and Defaults: timestamp_timeout, passwd_tries, env_reset, env_keep, secure_path, authenticate (the rest is ignored).
 * Includes: #include FILE, #includedir DIR, @include / @includedir.
 *
 * Build:  cc -O2 -o sudo sudo.c -lpam    (test build without PAM or root checks: -DSUDO_TEST -DSUDOERS_PATH='"/x/sudoers"' -DTS_DIR='"/x/ts"')
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <syslog.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#ifndef SUDO_TEST
#include <security/pam_appl.h>
#endif

#ifndef SUDOERS_PATH
#define SUDOERS_PATH "/etc/sudoers"
#endif
#ifndef TS_DIR
#define TS_DIR "/run/sudo/ts"
#endif
#define VERSION "0.1"

static void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
static void die(const char *fmt, ...)
{
	va_list ap;
	fputs("sudo: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(1);
}

/* ---- sudoers data ------------------------------------------------------------------------------------------------ */

typedef struct { char kind, *name, *list; } Alias;     /* kind: u user, r runas, h host, c command */
typedef struct { char *who, *host, *runas, *rungroup, *cmd; int nopasswd, setenv, noexec, line; char *file; } Rule;  /* one per command */
typedef struct { char scope, *scopearg, *text; } Def;  /* scope: 0 global, u user, h host, r runas */

static Alias *aliases; static int nalias;
static Rule *rules;   static int nrules;
static Def *defs;     static int ndefs;
static int errors, check_mode;

static char *xstrdup(const char *s) { char *p = strdup(s); if (!p) die("out of memory"); return p; }
static void *grow(void *p, int n, size_t sz) { p = realloc(p, (size_t)(n + 1) * sz); if (!p) die("out of memory"); return p; }

static void parse_err(const char *file, int line, const char *msg)
{
	fprintf(stderr, "sudo: %s:%d: %s\n", file, line, msg);
	errors++;
}

static char *trim(char *s)
{
	char *e;
	while (*s == ' ' || *s == '\t') s++;
	e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
	return s;
}

/* split s in place at top-level commas (not inside parentheses; a backslash escapes the next char) */
static int split_commas(char *s, char **out, int max)
{
	int n = 0, depth = 0;
	char *start = s, *w = s;
	for (char *p = s;; p++) {
		if (*p == '\\' && p[1]) { *w++ = p[1]; p++; continue; }
		if (*p == '(') depth++;
		if (*p == ')' && depth) depth--;
		if ((*p == ',' && !depth) || !*p) {
			int end = !*p;
			*w = 0;
			if (n < max) out[n++] = trim(start);
			if (end) break;
			w++;
			start = w;
			continue;
		}
		*w++ = *p;
	}
	return n;
}

static void parse_rule(char *line, const char *file, int lineno)
{
	char *eq = strchr(line, '='), *left, *words[8], *rest, *cmds[256], *runas = NULL, *rungroup = NULL, acc[1024] = "";
	int nw = 0, n, nopasswd = 0, setenv_ = 0, noexec = 0;
	if (!eq) { parse_err(file, lineno, "expected USER HOST = COMMANDS"); return; }
	*eq = 0;
	left = trim(line);
	rest = trim(eq + 1);
	for (char *t = strtok(left, " \t"); t && nw < 8; t = strtok(NULL, " \t")) {   /* a token ending in ',' joins the next one */
		strncat(acc, t, sizeof acc - strlen(acc) - 2);
		if (t[strlen(t) - 1] == ',') { strncat(acc, " ", sizeof acc - strlen(acc) - 1); continue; }
		words[nw++] = xstrdup(acc);
		acc[0] = 0;
	}
	if (nw != 2) { parse_err(file, lineno, "expected USER HOST before ="); return; }
	n = split_commas(rest, cmds, 256);
	for (int i = 0; i < n; i++) {
		char *c = cmds[i];
		for (;;) {   /* any number of (runas) and TAG: prefixes */
			c = trim(c);
			if (*c == '(') {
				char *close = strchr(c, ')'), *colon;
				if (!close) { parse_err(file, lineno, "missing )"); return; }
				*close = 0;
				colon = strchr(c + 1, ':');
				rungroup = NULL;
				if (colon) { *colon = 0; rungroup = xstrdup(trim(colon + 1)); }
				runas = xstrdup(trim(c + 1));
				if (!*runas) { free(runas); runas = xstrdup("root"); }
				c = close + 1;
				continue;
			}
			if (!strncmp(c, "NOPASSWD:", 9)) { nopasswd = 1; c += 9; continue; }
			if (!strncmp(c, "PASSWD:", 7)) { nopasswd = 0; c += 7; continue; }
			if (!strncmp(c, "SETENV:", 7)) { setenv_ = 1; c += 7; continue; }
			if (!strncmp(c, "NOSETENV:", 9)) { setenv_ = 0; c += 9; continue; }
			if (!strncmp(c, "NOEXEC:", 7)) { noexec = 1; c += 7; continue; }
			if (!strncmp(c, "EXEC:", 5)) { noexec = 0; c += 5; continue; }
			{   /* tags that only matter for logging/mail: accepted and ignored */
				static const char *const ign[] = { "LOG_INPUT:", "LOG_OUTPUT:", "NOLOG_INPUT:", "NOLOG_OUTPUT:", "MAIL:", "NOMAIL:", "FOLLOW:", "NOFOLLOW:", NULL };
				int hit = 0;
				for (int k = 0; ign[k]; k++) if (!strncmp(c, ign[k], strlen(ign[k]))) { c += strlen(ign[k]); hit = 1; break; }
				if (hit) continue;
			}
			break;
		}
		if (!*c) continue;
		rules = grow(rules, nrules, sizeof *rules);
		rules[nrules++] = (Rule){ xstrdup(words[0]), xstrdup(words[1]), xstrdup(runas ? runas : "root"), rungroup ? xstrdup(rungroup) : NULL, xstrdup(c),
		                          nopasswd, setenv_, noexec, lineno, xstrdup(file) };
	}
}

static void load_file(const char *path, int depth);

static void load_dir(const char *dir, int depth)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	char *names[512];
	int n = 0;
	if (!d) return;
	while ((e = readdir(d)) && n < 512) {
		size_t l = strlen(e->d_name);
		if (e->d_name[0] == '.' || (l && e->d_name[l - 1] == '~') || strchr(e->d_name, '.')) continue;   /* sudo skips names with . or ending ~ */
		names[n++] = xstrdup(e->d_name);
	}
	closedir(d);
	for (int i = 0; i < n; i++)
		for (int j = i + 1; j < n; j++) if (strcmp(names[j], names[i]) < 0) { char *t = names[i]; names[i] = names[j]; names[j] = t; }
	for (int i = 0; i < n; i++) {
		char p[PATH_MAX];
		snprintf(p, sizeof p, "%s/%s", dir, names[i]);
		load_file(p, depth + 1);
		free(names[i]);
	}
}

static const char *unquote(char *t)
{
	t = trim(t);
	if (*t == '"') { t++; t[strcspn(t, "\"")] = 0; }
	return t;
}

static void handle_line(char *line, const char *file, int lineno, int depth)
{
	size_t l = strcspn(line, " \t");
	if (!strncmp(line, "#include ", 9) || !strncmp(line, "@include ", 9)) { load_file(unquote(line + 9), depth + 1); return; }
	if (!strncmp(line, "#includedir ", 12) || !strncmp(line, "@includedir ", 12)) { load_dir(unquote(line + 12), depth); return; }
	if (*line == '#' || !*line) return;
	if (!strncmp(line, "Defaults", 8) && strchr(" \t:@!>", line[8])) {
		char *p = line + 8, scope = 0, *arg = NULL, *items[64];
		int n;
		if (*p == ':' || *p == '@' || *p == '!' || *p == '>') {
			char *sp = p + 1;
			scope = *p == ':' ? 'u' : *p == '@' ? 'h' : *p == '>' ? 'r' : 'c';
			arg = sp;
			while (*sp && *sp != ' ' && *sp != '\t') sp++;
			if (*sp) *sp++ = 0;
			arg = xstrdup(arg);
			p = sp;
		}
		n = split_commas(trim(p), items, 64);
		for (int i = 0; i < n; i++) {
			defs = grow(defs, ndefs, sizeof *defs);
			defs[ndefs++] = (Def){ scope, arg, xstrdup(items[i]) };
		}
		return;
	}
	{
		char first[32], kind = 0;
		snprintf(first, sizeof first, "%.*s", (int)(l < 31 ? l : 31), line);
		if (!strcmp(first, "User_Alias")) kind = 'u';
		else if (!strcmp(first, "Runas_Alias")) kind = 'r';
		else if (!strcmp(first, "Host_Alias")) kind = 'h';
		else if (!strcmp(first, "Cmnd_Alias") || !strcmp(first, "Cmd_Alias")) kind = 'c';
		if (kind) {   /* NAME = a, b : NAME2 = c */
			char *chunks[32], *body = trim(line + l);
			int n = 0;
			for (char *s = body; s && n < 32;) { chunks[n++] = s; s = strchr(s, ':'); if (s) *s++ = 0; }
			for (int i = 0; i < n; i++) {
				char *eq = strchr(chunks[i], '=');
				if (!eq) { parse_err(file, lineno, "bad alias"); return; }
				*eq = 0;
				aliases = grow(aliases, nalias, sizeof *aliases);
				aliases[nalias++] = (Alias){ kind, xstrdup(trim(chunks[i])), xstrdup(trim(eq + 1)) };
			}
			return;
		}
	}
	parse_rule(line, file, lineno);
}

static void load_file(const char *path, int depth)
{
	struct stat st;
	FILE *f;
	char buf[8192], raw[4096];
	int lineno = 0;
	size_t blen = 0;
	if (depth > 16) { fprintf(stderr, "sudo: include depth exceeded at %s\n", path); errors++; return; }
	f = fopen(path, "r");
	if (!f) { if (depth == 0) die("cannot read %s: %s", path, strerror(errno)); return; }
	if (fstat(fileno(f), &st) < 0 ||
#ifndef SUDO_TEST
	    st.st_uid != 0 ||
#endif
	    (st.st_mode & (S_IWGRP | S_IWOTH))) {
		fclose(f);
		die("%s must be owned by root and not writable by group or others", path);
	}
	buf[0] = 0;
	while (fgets(raw, sizeof raw, f)) {
		size_t l = strlen(raw);
		char *line;
		lineno++;
		while (l && (raw[l - 1] == '\n' || raw[l - 1] == '\r')) raw[--l] = 0;
		if (l && raw[l - 1] == '\\') raw[--l] = 0;   /* continuation: keep collecting */
		else l = (size_t)-1;
		{
			size_t add = strlen(raw);
			if (blen + add < sizeof buf) { memcpy(buf + blen, raw, add + 1); blen += add; }
		}
		if (l != (size_t)-1) continue;
		line = trim(buf);
		if (*line != '#') for (char *p = line; *p; p++) if (*p == '#' && (p == line || p[-1] == ' ' || p[-1] == '\t') && !(p[1] >= '0' && p[1] <= '9')) { *p = 0; break; }
		handle_line(trim(line), path, lineno, depth);
		buf[0] = 0; blen = 0;
	}
	fclose(f);
}

/* ---- identity and matching ---------------------------------------------------------------------------------------- */

static char me_name[128], hostname_[256];
static gid_t *my_groups;
static int n_my_groups;

static int user_in_group(const char *group)
{
	struct group *g = getgrnam(group);
	if (!g) return 0;
	for (int i = 0; i < n_my_groups; i++) if (my_groups[i] == g->gr_gid) return 1;
	return 0;
}

static int match_token(char kind, const char *tok, const char *subject, int depth);

static int match_list(char kind, const char *list, const char *subject, int depth)
{
	char *copy = xstrdup(list), *items[128];
	int n = split_commas(copy, items, 128), res = 0;
	for (int i = 0; i < n; i++) {
		char *t = items[i];
		int neg = 0;
		while (*t == '!') { neg = !neg; t = trim(t + 1); }
		if (match_token(kind, t, subject, depth)) res = !neg;
	}
	free(copy);
	return res;
}

static int match_token(char kind, const char *tok, const char *subject, int depth)
{
	if (depth > 16) return 0;
	if (!strcmp(tok, "ALL")) return 1;
	for (int i = 0; i < nalias; i++)
		if (aliases[i].kind == kind && !strcmp(aliases[i].name, tok)) return match_list(kind, aliases[i].list, subject, depth + 1);
	if (kind == 'h') return fnmatch(tok, subject, FNM_CASEFOLD) == 0;
	if (tok[0] == '%') {   /* group: for users, "is the caller in it"; for the run-as/group list, the group name itself */
		const char *g = tok + 1;
		if (kind == 'g') return fnmatch(g, subject, 0) == 0;
		if (kind != 'u') return 0;
		if (*g == '#') { gid_t id = atoi(g + 1); for (int i = 0; i < n_my_groups; i++) if (my_groups[i] == id) return 1; return 0; }
		return user_in_group(g);
	}
	if (tok[0] == '#') { struct passwd *p = getpwnam(subject); return p && p->pw_uid == (uid_t)atoi(tok + 1); }
	return fnmatch(tok, subject, 0) == 0;
}

/* ---- commands ---------------------------------------------------------------------------------------------------- */

typedef struct { char *path; char **argv; int argc; } Cmd;     /* path resolved, argv[0] as typed */

static int match_one_cmd(const char *spec, const Cmd *c, int depth);

static int match_cmd_list(const char *list, const Cmd *c, int depth)
{
	char *copy = xstrdup(list), *items[128];
	int n = split_commas(copy, items, 128), res = 0;
	for (int i = 0; i < n; i++) {
		char *t = items[i];
		int neg = 0;
		while (*t == '!') { neg = !neg; t = trim(t + 1); }
		if (match_one_cmd(t, c, depth)) res = !neg;
	}
	free(copy);
	return res;
}

static int match_one_cmd(const char *spec, const Cmd *c, int depth)
{
	char *copy = xstrdup(spec), *sp;
	int ok;
	if (depth > 16) { free(copy); return 0; }
	if (!strcmp(copy, "ALL")) { free(copy); return 1; }
	for (int i = 0; i < nalias; i++)
		if (aliases[i].kind == 'c' && !strcmp(aliases[i].name, copy)) { free(copy); return match_cmd_list(aliases[i].list, c, depth + 1); }
	if (!c->path) { free(copy); return 0; }
	sp = strpbrk(copy, " \t");
	if (sp) { *sp++ = 0; sp = trim(sp); }
	if (copy[0] != '/' && strcmp(copy, "sudoedit")) { free(copy); return 0; }
	{
		size_t l = strlen(copy);
		if (copy[l - 1] == '/') ok = !strncmp(c->path, copy, l) && !strchr(c->path + l, '/');   /* every file directly in that directory */
		else ok = fnmatch(copy, c->path, FNM_PATHNAME) == 0;
	}
	if (ok && sp) {
		char joined[4096] = "";
		for (int i = 1; i < c->argc; i++) { if (i > 1) strncat(joined, " ", sizeof joined - strlen(joined) - 1); strncat(joined, c->argv[i], sizeof joined - strlen(joined) - 1); }
		ok = !strcmp(sp, "\"\"") ? c->argc == 1 : fnmatch(sp, joined, 0) == 0;
	}
	free(copy);
	return ok;
}

static char *resolve_command(const char *name, const char *path_var)
{
	char full[PATH_MAX], *pv, *dir, *save = NULL;
	struct stat st;
	if (strchr(name, '/')) return !stat(name, &st) && S_ISREG(st.st_mode) ? xstrdup(name) : NULL;
	pv = xstrdup(path_var && *path_var ? path_var : "/usr/local/bin:/usr/bin:/bin");
	for (dir = strtok_r(pv, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
		snprintf(full, sizeof full, "%s/%s", *dir ? dir : ".", name);
		if (!stat(full, &st) && S_ISREG(st.st_mode) && !access(full, X_OK)) { free(pv); return xstrdup(full); }
	}
	free(pv);
	return NULL;
}

/* ---- defaults ---------------------------------------------------------------------------------------------------- */

static int timestamp_timeout = 5, passwd_tries = 3, env_reset = 1, authenticate = 1;
static char *secure_path, *env_keep_extra;

static void apply_default(const char *text)
{
	char *copy = xstrdup(text), *eq, *name = copy, *val = NULL;
	int neg = 0;
	if (*name == '!') { neg = 1; name++; }
	eq = strchr(name, '=');
	if (eq) {
		if (eq > name && (eq[-1] == '+' || eq[-1] == '-')) eq[-1] = 0;
		*eq = 0;
		val = trim(eq + 1);
		if (*val == '"') { val++; val[strcspn(val, "\"")] = 0; }
	}
	name = trim(name);
	if (!strcmp(name, "timestamp_timeout") && val) timestamp_timeout = atoi(val);
	else if (!strcmp(name, "passwd_tries") && val) passwd_tries = atoi(val);
	else if (!strcmp(name, "env_reset")) env_reset = !neg;
	else if (!strcmp(name, "authenticate")) authenticate = !neg;
	else if (!strcmp(name, "secure_path")) { free(secure_path); secure_path = val && !neg ? xstrdup(val) : NULL; }
	else if (!strcmp(name, "env_keep") && val) {
		size_t l = (env_keep_extra ? strlen(env_keep_extra) : 0) + strlen(val) + 2;
		int fresh = !env_keep_extra;
		env_keep_extra = realloc(env_keep_extra, l);
		if (!env_keep_extra) die("out of memory");
		if (fresh) env_keep_extra[0] = 0;
		strcat(env_keep_extra, " ");
		strcat(env_keep_extra, val);
	}
	free(copy);
}

static void apply_defaults(const char *tgt)
{
	for (int i = 0; i < ndefs; i++) {
		Def *d = &defs[i];
		int ok = 1;
		if (d->scope == 'u') ok = match_list('u', d->scopearg, me_name, 0);
		else if (d->scope == 'h') ok = match_list('h', d->scopearg, hostname_, 0);
		else if (d->scope == 'r') ok = match_list('r', d->scopearg, tgt, 0);
		else if (d->scope) ok = 0;   /* per-command defaults are not supported */
		if (ok) apply_default(d->text);
	}
}

/* ---- authorisation ---------------------------------------------------------------------------------------------- */

typedef struct { int allowed, nopasswd, setenv, noexec; } Verdict;

static int rule_runas_ok(const Rule *r, const char *tgt_user, const char *tgt_group)
{
	if (tgt_group) return r->rungroup && match_list('g', r->rungroup, tgt_group, 0) && match_list('r', r->runas, tgt_user, 0);
	return match_list('r', r->runas, tgt_user, 0);
}

static int rule_for_me(const Rule *r) { return match_list('u', r->who, me_name, 0) && match_list('h', r->host, hostname_, 0); }

static Verdict authorise(const Cmd *c, const char *tgt_user, const char *tgt_group)
{
	Verdict v = { 0 };
	for (int i = 0; i < nrules; i++) {   /* the last matching rule wins */
		Rule *r = &rules[i];
		char *entry = r->cmd;
		int neg = 0;
		if (!rule_for_me(r) || !rule_runas_ok(r, tgt_user, tgt_group)) continue;
		while (*entry == '!') { neg = !neg; entry = trim(entry + 1); }
		if (match_one_cmd(entry, c, 0)) v = (Verdict){ !neg, r->nopasswd, r->setenv, r->noexec };
	}
	return v;
}

/* ---- timestamps -------------------------------------------------------------------------------------------------- */

static void ts_name(char *out, size_t n)
{
	struct stat st;
	unsigned long tty = 0;
	if (!fstat(0, &st) && S_ISCHR(st.st_mode)) tty = st.st_rdev;
	snprintf(out, n, TS_DIR "/%u-%d-%lu", (unsigned)getuid(), (int)getsid(0), tty);
}

static long boot_secs(void) { struct timespec ts; clock_gettime(CLOCK_BOOTTIME, &ts); return (long)ts.tv_sec; }

static int ts_valid(void)
{
	char p[PATH_MAX];
	FILE *f;
	long t = 0;
	if (timestamp_timeout < 0) return 1;
	if (timestamp_timeout == 0) return 0;
	ts_name(p, sizeof p);
	if (!(f = fopen(p, "r"))) return 0;
	if (fscanf(f, "%ld", &t) != 1) t = 0;
	fclose(f);
	return t && boot_secs() - t < (long)timestamp_timeout * 60;
}

static void ts_touch(void)
{
	char p[PATH_MAX];
	FILE *f;
	mkdir("/run/sudo", 0711);
	mkdir(TS_DIR, 0700);
	ts_name(p, sizeof p);
	if ((f = fopen(p, "w"))) { fprintf(f, "%ld\n", boot_secs()); fclose(f); chmod(p, 0600); }
}

static void ts_forget(int all)
{
	DIR *d;
	struct dirent *e;
	char prefix[32], p[PATH_MAX];
	if (!all) { ts_name(p, sizeof p); unlink(p); return; }
	if (!(d = opendir(TS_DIR))) return;
	snprintf(prefix, sizeof prefix, "%u-", (unsigned)getuid());
	while ((e = readdir(d))) if (!strncmp(e->d_name, prefix, strlen(prefix))) { snprintf(p, sizeof p, TS_DIR "/%s", e->d_name); unlink(p); }
	closedir(d);
}

/* ---- authentication (PAM) -------------------------------------------------------------------------------------- */

static int opt_stdin_pw, opt_nonint;

static char *read_secret(const char *prompt, int echo)
{
	static char buf[512];
	FILE *tty = opt_stdin_pw ? NULL : fopen("/dev/tty", "r+");
	struct termios old, nw;
	int have_term = 0;
	char *r;
	if (tty && !echo && !tcgetattr(fileno(tty), &old)) { nw = old; nw.c_lflag &= ~(tcflag_t)ECHO; tcsetattr(fileno(tty), TCSAFLUSH, &nw); have_term = 1; }
	fputs(prompt, tty ? tty : stderr);
	fflush(tty ? tty : stderr);
	r = fgets(buf, sizeof buf, tty ? tty : stdin);
	if (have_term) { tcsetattr(fileno(tty), TCSAFLUSH, &old); fputc('\n', tty); }
	if (tty) fclose(tty);
	if (!r) return NULL;
	buf[strcspn(buf, "\n")] = 0;
	return buf;
}

#ifndef SUDO_TEST
static int conv(int n, const struct pam_message **msg, struct pam_response **resp, void *data)
{
	struct pam_response *r = calloc(n, sizeof *r);
	(void)data;
	if (!r) return PAM_BUF_ERR;
	for (int i = 0; i < n; i++) {
		if (msg[i]->msg_style == PAM_PROMPT_ECHO_OFF || msg[i]->msg_style == PAM_PROMPT_ECHO_ON) {
			char *s = read_secret(msg[i]->msg, msg[i]->msg_style == PAM_PROMPT_ECHO_ON);
			if (!s) { free(r); return PAM_CONV_ERR; }
			r[i].resp = strdup(s);
		} else fprintf(stderr, "%s\n", msg[i]->msg);
	}
	*resp = r;
	return PAM_SUCCESS;
}

static int authenticate_user(void)
{
	struct pam_conv pc = { conv, NULL };
	pam_handle_t *h = NULL;
	int rc = PAM_AUTH_ERR;
	if (pam_start("sudo", me_name, &pc, &h) != PAM_SUCCESS) die("PAM is not usable");
	for (int i = 0; i < passwd_tries; i++) {
		rc = pam_authenticate(h, 0);
		if (rc == PAM_SUCCESS) { rc = pam_acct_mgmt(h, 0); break; }
		if (rc == PAM_MAXTRIES || rc == PAM_ABORT) break;
		fputs("Sorry, try again.\n", stderr);
	}
	pam_end(h, rc);
	return rc == PAM_SUCCESS;
}
#else
static int authenticate_user(void) { (void)read_secret; return 1; }
#endif

/* ---- environment and exec ---------------------------------------------------------------------------------------- */

static const char *const keep_default[] = { "COLORS", "DISPLAY", "HOSTNAME", "KRB5CCNAME", "LS_COLORS", "PATH_LOCALE", "TERM", "XAUTHORIZATION", "XAUTHORITY",
                                            "WAYLAND_DISPLAY", "LANG", "LANGUAGE", "LC_*", "COLORTERM", "XDG_CURRENT_DESKTOP", NULL };

static int env_kept(const char *name)
{
	char *extra, *save = NULL, *t;
	for (int i = 0; keep_default[i]; i++) if (!fnmatch(keep_default[i], name, 0)) return 1;
	if (!env_keep_extra) return 0;
	extra = xstrdup(env_keep_extra);
	for (t = strtok_r(extra, " \t", &save); t; t = strtok_r(NULL, " \t", &save)) if (!fnmatch(t, name, 0)) { free(extra); return 1; }
	free(extra);
	return 0;
}

static int env_dangerous(const char *kv)
{
	static const char *const bad[] = { "LD_", "IFS=", "PS4=", "BASH_ENV=", "ENV=", "SHELLOPTS=", "BASHOPTS=", "PYTHONINSPECT=", "PERL5OPT=", "PERL5LIB=", "GCONV_PATH=", "LOCPATH=", "NLSPATH=", "TMPDIR=", "TERMCAP=", NULL };
	for (int i = 0; bad[i]; i++) if (!strncmp(kv, bad[i], strlen(bad[i]))) return 1;
	return 0;
}

static char **build_env(const struct passwd *tp, const char *cmdline, int preserve)
{
	extern char **environ;
	char **out = calloc(300, sizeof *out), buf[PATH_MAX + 32];
	int n = 0;
	for (char **e = environ; *e && n < 200; e++) {
		char name[128];
		size_t l = strcspn(*e, "=");
		snprintf(name, sizeof name, "%.*s", (int)(l < 127 ? l : 127), *e);
		if (env_dangerous(*e) || !strncmp(*e, "SUDO_", 5)) continue;
		if (!strncmp(*e, "HOME=", 5) || !strncmp(*e, "USER=", 5) || !strncmp(*e, "LOGNAME=", 8) || !strncmp(*e, "SHELL=", 6) || !strncmp(*e, "MAIL=", 5)) continue;
		if (secure_path && !strncmp(*e, "PATH=", 5)) continue;
		if (preserve || !env_reset || env_kept(name)) out[n++] = *e;
	}
	snprintf(buf, sizeof buf, "HOME=%s", tp->pw_dir); out[n++] = xstrdup(buf);
	snprintf(buf, sizeof buf, "USER=%s", tp->pw_name); out[n++] = xstrdup(buf);
	snprintf(buf, sizeof buf, "LOGNAME=%s", tp->pw_name); out[n++] = xstrdup(buf);
	snprintf(buf, sizeof buf, "SHELL=%s", tp->pw_shell && *tp->pw_shell ? tp->pw_shell : "/bin/sh"); out[n++] = xstrdup(buf);
	{
		int have = 0;
		for (int i = 0; i < n; i++) if (!strncmp(out[i], "PATH=", 5)) have = 1;
		if (secure_path || !have) { snprintf(buf, sizeof buf, "PATH=%s", secure_path ? secure_path : "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"); out[n++] = xstrdup(buf); }
	}
	snprintf(buf, sizeof buf, "SUDO_USER=%s", me_name); out[n++] = xstrdup(buf);
	snprintf(buf, sizeof buf, "SUDO_UID=%u", (unsigned)getuid()); out[n++] = xstrdup(buf);
	snprintf(buf, sizeof buf, "SUDO_GID=%u", (unsigned)getgid()); out[n++] = xstrdup(buf);
	if (cmdline) { snprintf(buf, sizeof buf, "SUDO_COMMAND=%.*s", PATH_MAX - 1, cmdline); out[n++] = xstrdup(buf); }
	out[n] = NULL;
	return out;
}

static void log_it(const char *what, const char *tgt, const char *cmdline)
{
	char cwd[PATH_MAX] = "?", tty[64] = "unknown";
	const char *t = ttyname(0);
	if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, "?");
	if (t) snprintf(tty, sizeof tty, "%s", !strncmp(t, "/dev/", 5) ? t + 5 : t);
	openlog("sudo", LOG_PID, LOG_AUTHPRIV);
	syslog(LOG_NOTICE, "%s : %sTTY=%s ; PWD=%s ; USER=%s ; COMMAND=%s", me_name, what, tty, cwd, tgt, cmdline);
	closelog();
}

static void close_fds(void)
{
#ifdef SYS_close_range
	if (syscall(SYS_close_range, 3u, ~0u, 0) == 0) return;
#endif
	for (int fd = 3; fd < 1024; fd++) close(fd);
}

static char *join_args(char **argv, int from, int to)
{
	size_t len = 1;
	char *s;
	for (int k = from; k < to; k++) len += strlen(argv[k]) + 1;
	s = malloc(len);
	if (!s) die("out of memory");
	s[0] = 0;
	for (int k = from; k < to; k++) { if (k > from) strcat(s, " "); strcat(s, argv[k]); }
	return s;
}

/* ---- tui (filled in below) ---------------------------------------------------------------------------------------- */
static int tui_main(void);

/* ---- main -------------------------------------------------------------------------------------------------------- */

static void usage(int rc) __attribute__((noreturn));
static void usage(int rc)
{
	fputs("usage: sudo [-u USER] [-g GROUP] [-EHnSb] [--] COMMAND [ARGS...]\n"
	      "       sudo -i | -s [COMMAND]    sudo -l [COMMAND]    sudo -v | -k | -K\n"
	      "       sudo --check [FILE]       sudo --tui           sudo -V\n", rc ? stderr : stdout);
	exit(rc);
}

static void list_rules(void)
{
	int shown = 0;
	printf("User %s may run the following commands on %s:\n", me_name, hostname_);
	for (int i = 0; i < nrules; i++) {
		Rule *r = &rules[i];
		if (!rule_for_me(r)) continue;
		printf("    (%s%s%s) %s%s%s\n", r->runas, r->rungroup ? " : " : "", r->rungroup ? r->rungroup : "", r->nopasswd ? "NOPASSWD: " : "", r->setenv ? "SETENV: " : "", r->cmd);
		shown++;
	}
	if (!shown) printf("    (none)\n");
}

int main(int argc, char **argv)
{
	const char *tgt_user = "root", *tgt_group = NULL, *sudoers = SUDOERS_PATH;
	int opt_E = 0, opt_i = 0, opt_s = 0, opt_l = 0, opt_v = 0, opt_k = 0, opt_K = 0, i = 1;
	struct passwd *pw, *tp;
	Cmd cmd = { 0 };
	Verdict v;
	char *cmdline = NULL, **envp, *shell_argv[4];

	for (; i < argc; i++) {
		const char *a = argv[i];
		if (a[0] != '-' || !strcmp(a, "-")) break;
		if (!strcmp(a, "--")) { i++; break; }
		if (!strcmp(a, "--check")) { check_mode = 1; if (i + 1 < argc) sudoers = argv[++i]; break; }
		if (!strcmp(a, "--tui")) return tui_main();
		if (!strcmp(a, "--help")) usage(0);
		if (!strcmp(a, "--version")) { puts("sudo (notcoreutils) " VERSION); return 0; }
		if (!strcmp(a, "--user") && i + 1 < argc) { tgt_user = argv[++i]; continue; }
		if (!strcmp(a, "--group") && i + 1 < argc) { tgt_group = argv[++i]; continue; }
		if (!strncmp(a, "--user=", 7)) { tgt_user = a + 7; continue; }
		if (!strncmp(a, "--group=", 8)) { tgt_group = a + 8; continue; }
		if (!strcmp(a, "--login")) { opt_i = 1; continue; }
		if (!strcmp(a, "--shell")) { opt_s = 1; continue; }
		if (!strcmp(a, "--list")) { opt_l = 1; continue; }
		if (!strcmp(a, "--non-interactive")) { opt_nonint = 1; continue; }
		if (!strcmp(a, "--preserve-env")) { opt_E = 1; continue; }
		if (!strcmp(a, "--stdin")) { opt_stdin_pw = 1; continue; }
		if (!strcmp(a, "--validate")) { opt_v = 1; continue; }
		if (!strcmp(a, "--reset-timestamp")) { opt_k = 1; continue; }
		if (!strcmp(a, "--remove-timestamp")) { opt_K = 1; continue; }
		if (a[1] == '-') die("unknown option %s", a);
		for (const char *p = a + 1; *p; p++) {
			switch (*p) {
			case 'E': opt_E = 1; break;
			case 'H': case 'b': case 'A': case 'B': case 'P': break;
			case 'n': opt_nonint = 1; break;
			case 'S': opt_stdin_pw = 1; break;
			case 'i': opt_i = 1; break;
			case 's': opt_s = 1; break;
			case 'l': opt_l = 1; break;
			case 'v': opt_v = 1; break;
			case 'k': opt_k = 1; break;
			case 'K': opt_K = 1; break;
			case 'V': puts("sudo (notcoreutils) " VERSION); return 0;
			case 'h': usage(0);
			case 'u': case 'g': case 'C': case 'D': case 'p': case 'r': case 't': case 'T': case 'U': case 'R': case 'c': {
				const char *val = p[1] ? p + 1 : (i + 1 < argc ? argv[++i] : NULL);
				if (!val) usage(1);
				if (*p == 'u') tgt_user = val; else if (*p == 'g') tgt_group = val;
				p += strlen(p) - 1;
				break;
			}
			default: die("unknown option -%c (try sudo -h)", *p);
			}
		}
	}

	if (check_mode) {
		load_file(sudoers, 0);
		if (errors) { printf("%s: %d error(s)\n", sudoers, errors); return 1; }
		printf("%s: parsed OK (%d rules, %d aliases, %d defaults)\n", sudoers, nrules, nalias, ndefs);
		return 0;
	}

#ifndef SUDO_TEST
	if (geteuid() != 0) die("must be owned by uid 0 and have the setuid bit set");
#endif
	umask(022);
	if (!(pw = getpwuid(getuid()))) die("you do not exist in the passwd database");
	snprintf(me_name, sizeof me_name, "%s", pw->pw_name);
	gethostname(hostname_, sizeof hostname_);
	{
		int n = 64;
		my_groups = malloc(n * sizeof *my_groups);
		if (getgrouplist(me_name, pw->pw_gid, my_groups, &n) < 0) { my_groups = realloc(my_groups, n * sizeof *my_groups); getgrouplist(me_name, pw->pw_gid, my_groups, &n); }
		n_my_groups = n;
	}

	if (opt_K) { ts_forget(1); return 0; }
	if (opt_k && i >= argc && !opt_l && !opt_v && !opt_i && !opt_s) { ts_forget(0); return 0; }

	load_file(sudoers, 0);
	if (errors) die("%s has errors; fix them first (sudo --check)", sudoers);

	if (tgt_user[0] == '#') tp = getpwuid(atoi(tgt_user + 1)); else tp = getpwnam(tgt_user);
	if (!tp) die("unknown user %s", tgt_user);
	tgt_user = xstrdup(tp->pw_name);
	apply_defaults(tgt_user);

	if (opt_i || opt_s) {
		const char *sh = opt_i || !getenv("SHELL") ? (tp->pw_shell && *tp->pw_shell ? tp->pw_shell : "/bin/sh") : getenv("SHELL");
		cmd.path = xstrdup(sh);
		shell_argv[0] = (char *)sh;
		if (i < argc) { shell_argv[1] = "-c"; shell_argv[2] = join_args(argv, i, argc); cmd.argc = 3; } else cmd.argc = 1;
		cmd.argv = shell_argv;
		cmdline = cmd.argc == 3 ? shell_argv[2] : xstrdup(sh);
	} else if (i < argc) {
		cmd.path = resolve_command(argv[i], secure_path ? secure_path : getenv("PATH"));
		cmd.argv = argv + i;
		cmd.argc = argc - i;
		if (!cmd.path && !opt_l) { fprintf(stderr, "sudo: %s: command not found\n", argv[i]); return 127; }
		if (cmd.path) { char *rest = join_args(argv, i + 1, argc); cmdline = malloc(strlen(cmd.path) + strlen(rest) + 2); sprintf(cmdline, "%s%s%s", cmd.path, *rest ? " " : "", rest); free(rest); }
	} else if (!opt_l && !opt_v) usage(1);

	if (opt_l && !cmd.argc) {
		int has = 0;
		for (int k = 0; k < nrules; k++) if (rule_for_me(&rules[k])) has = 1;
		if (!has) { fprintf(stderr, "Sorry, user %s may not run sudo on %s.\n", me_name, hostname_); return 1; }
		if (authenticate && !ts_valid()) { if (opt_nonint) die("a password is required"); if (!authenticate_user()) die("authentication failed"); ts_touch(); }
		list_rules();
		return 0;
	}
	if (opt_v && !cmd.argc) {
		if (!authenticate_user()) die("authentication failed");
		ts_touch();
		return 0;
	}

	v = authorise(&cmd, tgt_user, tgt_group);
	if (opt_l) {   /* sudo -l COMMAND: print the path when allowed */
		if (!v.allowed) return 1;
		puts(cmd.path ? cmd.path : cmd.argv[0]);
		return 0;
	}
	if (!v.allowed) {
		log_it("command not allowed ; ", tgt_user, cmdline ? cmdline : "?");
		fprintf(stderr, "sudo: %s is not allowed to run that command as %s\n", me_name, tgt_user);
		return 1;
	}
	if (opt_E && !v.setenv && getuid() != 0) die("sorry, you are not allowed to preserve the environment");

	if (authenticate && !v.nopasswd && getuid() != 0) {
		if (!ts_valid()) {
			if (opt_nonint) die("a password is required");
			if (!authenticate_user()) { log_it("authentication failure ; ", tgt_user, cmdline ? cmdline : "?"); die("authentication failed"); }
		}
		ts_touch();
	}
	if (opt_k) ts_forget(0);   /* -k COMMAND: run it, then forget */

	envp = build_env(tp, cmdline, opt_E);
	log_it("", tgt_user, cmdline ? cmdline : "?");
	close_fds();

	{
		gid_t gid = tp->pw_gid;
		if (tgt_group) {
			struct group *g = tgt_group[0] == '#' ? getgrgid(atoi(tgt_group + 1)) : getgrnam(tgt_group);
			if (!g) die("unknown group %s", tgt_group);
			gid = g->gr_gid;
		}
		if (initgroups(tp->pw_name, gid) < 0 && geteuid() == 0) die("initgroups: %s", strerror(errno));
		if (setresgid(gid, gid, gid) < 0) die("setresgid: %s", strerror(errno));
		if (setresuid(tp->pw_uid, tp->pw_uid, tp->pw_uid) < 0) die("setresuid: %s", strerror(errno));
	}
	if (opt_i && chdir(tp->pw_dir) < 0) fprintf(stderr, "sudo: cannot change to %s\n", tp->pw_dir);
	if (opt_i || opt_s) {
		char *sh_argv[4], name[PATH_MAX];
		const char *base = strrchr(cmd.path, '/') ? strrchr(cmd.path, '/') + 1 : cmd.path;
		snprintf(name, sizeof name, "%s%s", opt_i ? "-" : "", base);
		sh_argv[0] = name;
		if (cmd.argc == 3) { sh_argv[1] = "-c"; sh_argv[2] = cmd.argv[2]; sh_argv[3] = NULL; } else sh_argv[1] = NULL;
		execve(cmd.path, sh_argv, envp);
	} else execve(cmd.path, cmd.argv, envp);
	fprintf(stderr, "sudo: %s: %s\n", cmd.path, strerror(errno));
	return errno == ENOENT ? 127 : 126;
}

/* ---- tui ---------------------------------------------------------------------------------------------------------- */
/* `sudo --tui`: list every rule that applies, and add / delete / toggle NOPASSWD on the ones in the managed drop-in file
 * (MANAGED_FILE). Other files are shown read-only. Every save is written to a temp file, checked with `sudo --check`, and only then
 * renamed into place, so a typo cannot lock you out. Keys: j/k or arrows move, a add, d delete, n toggle NOPASSWD, c check, q quit. */

#ifndef MANAGED_FILE
#define MANAGED_FILE "/etc/sudoers.d/90-sudo-tui"
#endif

static char *mlines[256];
static int nm;
static struct termios tui_old;

static void tui_raw(int on)
{
	struct termios t;
	if (on) { tcgetattr(0, &tui_old); t = tui_old; t.c_lflag &= ~(tcflag_t)(ICANON | ECHO); t.c_cc[VMIN] = 1; t.c_cc[VTIME] = 0; tcsetattr(0, TCSAFLUSH, &t); fputs("\x1b[?1049h\x1b[?25l", stdout); }
	else { tcsetattr(0, TCSAFLUSH, &tui_old); fputs("\x1b[?25h\x1b[?1049l", stdout); }
	fflush(stdout);
}

/* run `sudo --check FILE` as a child of ourselves (not through sh: /proc/self/exe would then be sh) */
static int run_check(const char *file)
{
	char self[PATH_MAX];
	ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
	pid_t pid;
	int st = 0;
	if (n < 0) return -1;
	self[n] = 0;
	if ((pid = fork()) == 0) {
		int nul = open("/dev/null", O_RDWR);
		if (nul >= 0) { dup2(nul, 1); dup2(nul, 2); }
		execl(self, "sudo", "--check", file, (char *)NULL);
		_exit(127);
	}
	if (pid < 0 || waitpid(pid, &st, 0) < 0) return -1;
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void managed_load(void)
{
	FILE *f = fopen(MANAGED_FILE, "r");
	char line[1024];
	nm = 0;
	if (!f) return;
	while (fgets(line, sizeof line, f) && nm < 256) {
		line[strcspn(line, "\n")] = 0;
		if (*line && *line != '#') mlines[nm++] = xstrdup(line);
	}
	fclose(f);
}

/* write, verify, install. returns 0 on success; msg gets the reason otherwise */
static int managed_save(char *msg, size_t n)
{
	char tmp[PATH_MAX];
	FILE *f;
	int rc;
	snprintf(tmp, sizeof tmp, "%s.new", MANAGED_FILE);
	if (!(f = fopen(tmp, "w"))) { snprintf(msg, n, "cannot write %s: %s", tmp, strerror(errno)); return -1; }
	fputs("# managed by `sudo --tui`; edit by hand if you like, the TUI re-reads it\n", f);
	for (int i = 0; i < nm; i++) fprintf(f, "%s\n", mlines[i]);
	fclose(f);
	chmod(tmp, 0440);
	rc = run_check(tmp);
	if (rc != 0) { unlink(tmp); snprintf(msg, n, "refused: the result would not parse (nothing changed)"); return -1; }
	if (rename(tmp, MANAGED_FILE) < 0) { snprintf(msg, n, "rename failed: %s", strerror(errno)); return -1; }
	snprintf(msg, n, "saved %s", MANAGED_FILE);
	return 0;
}

static void ask(const char *prompt, char *out, size_t n, const char *def)
{
	tcsetattr(0, TCSAFLUSH, &tui_old);
	printf("\x1b[?25h\r\n%s%s%s%s: ", prompt, def ? " [" : "", def ? def : "", def ? "]" : "");
	fflush(stdout);
	if (!fgets(out, (int)n, stdin)) out[0] = 0;
	out[strcspn(out, "\n")] = 0;
	if (!*out && def) snprintf(out, n, "%s", def);
	{
		struct termios t = tui_old;
		t.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
		tcsetattr(0, TCSAFLUSH, &t);
	}
	fputs("\x1b[?25l", stdout);
}

static int tui_main(void)
{
	int sel = 0, quit = 0;
	char msg[256] = "";
	struct passwd *pw = getpwuid(getuid());
	Cmd any = { "/nonexistent/anything", NULL, 0 };
	Verdict v;
	if (!pw) die("you do not exist in the passwd database");
	snprintf(me_name, sizeof me_name, "%s", pw->pw_name);
	gethostname(hostname_, sizeof hostname_);
	{ int n = 64; my_groups = malloc(n * sizeof *my_groups); if (getgrouplist(me_name, pw->pw_gid, my_groups, &n) < 0) { my_groups = realloc(my_groups, n * sizeof *my_groups); getgrouplist(me_name, pw->pw_gid, my_groups, &n); } n_my_groups = n; }
#ifndef SUDO_TEST
	if (geteuid() != 0) die("must be owned by uid 0 and have the setuid bit set");
#endif
	load_file(SUDOERS_PATH, 0);
	if (errors) die("%s has errors; fix them first (sudo --check)", SUDOERS_PATH);
	apply_defaults("root");
	v = authorise(&any, "root", NULL);
	if (getuid() != 0 && !v.allowed) die("%s is not allowed to run everything, so it may not edit the rules", me_name);
	if (getuid() != 0 && !v.nopasswd && authenticate && !ts_valid()) { if (!authenticate_user()) die("authentication failed"); ts_touch(); }
	managed_load();
	if (!isatty(0)) die("--tui needs a terminal");
	tui_raw(1);
	while (!quit) {
		int total = nm, ro = 0;
		for (int i = 0; i < nrules; i++) if (strcmp(rules[i].file, MANAGED_FILE)) ro++;
		total += ro;
		if (sel >= total) sel = total ? total - 1 : 0;
		fputs("\x1b[H\x1b[2J", stdout);
		printf("\x1b[1m sudo rules\x1b[0m  (%s)   j/k move  a add  d delete  n toggle NOPASSWD  c check  q quit\r\n\r\n", MANAGED_FILE);
		for (int i = 0; i < nm; i++) printf("%s \x1b[32m*\x1b[0m %s\r\n", i == sel ? "\x1b[7m>" : " ", mlines[i]), i == sel ? fputs("\x1b[0m", stdout) : 0;
		{
			int row = nm;
			for (int i = 0; i < nrules; i++) {
				Rule *r = &rules[i];
				if (!strcmp(r->file, MANAGED_FILE)) continue;
				printf("%s   \x1b[2m%s %s = (%s%s%s) %s%s   [%s:%d]\x1b[0m\r\n", row == sel ? "\x1b[7m>" : " ", r->who, r->host, r->runas, r->rungroup ? ":" : "", r->rungroup ? r->rungroup : "",
				       r->nopasswd ? "NOPASSWD: " : "", r->cmd, r->file, r->line);
				if (row == sel) fputs("\x1b[0m", stdout);
				row++;
			}
		}
		printf("\r\n\x1b[33m%s\x1b[0m\r\n", msg);
		fflush(stdout);
		msg[0] = 0;
		{
			unsigned char k[8];
			ssize_t n = read(0, k, sizeof k);
			if (n <= 0) break;
			if (k[0] == 'q' || k[0] == 3) quit = 1;
			else if (k[0] == 'j' || (n >= 3 && k[0] == 27 && k[2] == 'B')) { if (sel + 1 < total) sel++; }
			else if (k[0] == 'k' || (n >= 3 && k[0] == 27 && k[2] == 'A')) { if (sel > 0) sel--; }
			else if (k[0] == 'a') {
				char who[128], runas[64], cmdt[512], pass[8], line[900];
				ask("who (user or %group)", who, sizeof who, me_name);
				ask("run as", runas, sizeof runas, "ALL");
				ask("command (full path, or ALL)", cmdt, sizeof cmdt, "ALL");
				ask("ask for password? (y/n)", pass, sizeof pass, "y");
				if (*who && *cmdt && nm < 256) {
					snprintf(line, sizeof line, "%s ALL=(%s) %s%s", who, runas, pass[0] == 'n' ? "NOPASSWD: " : "", cmdt);
					mlines[nm++] = xstrdup(line);
					if (managed_save(msg, sizeof msg)) { free(mlines[--nm]); }
				}
			} else if (k[0] == 'd' && sel < nm) {
				char *gone = mlines[sel];
				memmove(&mlines[sel], &mlines[sel + 1], (size_t)(nm - sel - 1) * sizeof *mlines);
				nm--;
				if (managed_save(msg, sizeof msg)) { memmove(&mlines[sel + 1], &mlines[sel], (size_t)(nm - sel) * sizeof *mlines); mlines[sel] = gone; nm++; }
				else free(gone);
			} else if (k[0] == 'n' && sel < nm) {
				char *old = mlines[sel], *p = strstr(old, "NOPASSWD: "), *nw = malloc(strlen(old) + 16);
				if (p) { memcpy(nw, old, (size_t)(p - old)); strcpy(nw + (p - old), p + 10); }
				else { char *eq = strchr(old, ')'); if (eq) { memcpy(nw, old, (size_t)(eq + 1 - old)); nw[eq + 1 - old] = 0; strcat(nw, " NOPASSWD:"); strcat(nw, eq + 1); } else strcpy(nw, old); }
				mlines[sel] = nw;
				if (managed_save(msg, sizeof msg)) { mlines[sel] = old; free(nw); } else free(old);
			} else if (k[0] == 'c') {
				int rc = run_check(SUDOERS_PATH);
				snprintf(msg, sizeof msg, rc == 0 ? "%s parses OK" : "%s has errors", SUDOERS_PATH);
			}
		}
	}
	tui_raw(0);
	return 0;
}
