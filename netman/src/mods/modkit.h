/* modkit.h - small helpers shared by ssh.c, mesh.c, bonding.c and tunnel.c. Every function is static: include it in a .c file,
 * there is nothing to link. Profiles are NAME.<ext> files of "key = value" lines (# comments) in a directory chosen by the config.
 * Process helpers never use a shell: argv arrays only.
 */
#ifndef NETMAN_MODKIT_H
#define NETMAN_MODKIT_H
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "enterprise/entcore.h"
#define MK static __attribute__((unused))

/* NAME: letters, digits, _ - . ; no leading dot or dash (it ends up in file names and argv) */
MK int mk_name_ok(const char *s) { return ent_safe_name(s, 32) && s[0] != '-'; }

/* a value that goes into an argv slot or a generated file: printable, no space, no quote, no leading '-' (no option injection) */
MK int mk_token_ok(const char *s, size_t max)
{
	size_t n = s ? strlen(s) : 0;
	if (!n || n >= max || s[0] == '-') return 0;
	for (size_t i = 0; i < n; i++)
		if ((unsigned char)s[i] <= ' ' || s[i] == 0x7f || s[i] == '"' || s[i] == '\'' || s[i] == '\\' || s[i] == '`' || s[i] == '$' || s[i] == ';' || s[i] == '|' || s[i] == '&') return 0;
	return 1;
}
MK void mk_cpy(char *d, size_t n, const char *s) { snprintf(d, n, "%s", s ? s : ""); }

/* calls cb(key, value, user) for every "key = value" line of the file; cb returns <0 to stop (that value is returned). -ENOENT if unreadable */
MK int mk_kv_read(const char *path, int (*cb)(const char *k, const char *v, void *u), void *u)
{
	char line[1024];
	FILE *f = fopen(path, "r");
	int r = 0;
	if (!f) return -errno;
	while (fgets(line, sizeof line, f)) {
		char *p = ent_trim(line), *eq;
		if (!*p || *p == '#') continue;
		if (!(eq = strchr(p, '='))) { r = -EINVAL; break; }
		*eq = 0;
		if ((r = cb(ent_trim(p), ent_trim(eq + 1), u)) < 0) break;
	}
	fclose(f);
	return r;
}

/* names (without extension) of DIR/NAME.EXT, sorted; returns the count */
MK int mk_names(const char *dir, const char *ext, char names[][32], int max)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	int n = 0;
	size_t el = strlen(ext);
	if (!d) return 0;
	while ((e = readdir(d)) && n < max) {
		size_t l = strlen(e->d_name);
		if (l <= el + 1 || strcmp(e->d_name + l - el, ext) || e->d_name[l - el - 1] != '.' || l - el - 1 >= 32) continue;
		memcpy(names[n], e->d_name, l - el - 1);
		names[n][l - el - 1] = 0;
		if (mk_name_ok(names[n])) n++;
	}
	closedir(d);
	for (int i = 1; i < n; i++)
		for (int j = i; j > 0 && strcmp(names[j - 1], names[j]) > 0; j--) { char t[32]; memcpy(t, names[j], 32); memcpy(names[j], names[j - 1], 32); memcpy(names[j - 1], t, 32); }
	return n;
}

MK int mk_exists(const char *p) { struct stat s; return stat(p, &s) == 0; }

/* 1 if BIN (a path, or a name searched in $PATH) is executable */
MK int mk_have(const char *bin)
{
	const char *p = getenv("PATH");
	char t[512];
	if (!bin || !*bin) return 0;
	if (strchr(bin, '/')) return access(bin, X_OK) == 0;
	for (p = p ? p : "/usr/bin:/bin"; *p;) {
		size_t l = strcspn(p, ":");
		snprintf(t, sizeof t, "%.*s/%s", (int)l, p, bin);
		if (access(t, X_OK) == 0) return 1;
		p += l + (p[l] ? 1 : 0);
	}
	return 0;
}

/* runs argv (no shell), output captured; returns 0, -ENOENT when the binary is missing, -EIO when it failed (text in out) */
MK int mk_run(char *const argv[], char *out, size_t n)
{
	char scratch[512];
	if (!out) { out = scratch; n = sizeof scratch; }
	return ent_exec(argv, NULL, out, n);
}

MK int mk_pid_alive(long pid) { return pid > 1 && (kill((pid_t)pid, 0) == 0 || errno == EPERM); }
MK long mk_pid_read(const char *path)
{
	char b[32];
	if (ent_slurp(path, b, sizeof b) < 0) return 0;
	return atol(b);
}
MK int mk_pid_write(const char *path, long pid)
{
	char b[32];
	int l = snprintf(b, sizeof b, "%ld\n", pid);
	return ent_write_atomic(path, b, (size_t)l, 0644);
}

/* "1" / "yes" / "true" / "on" */
MK int mk_bool(const char *v) { return !strcmp(v, "1") || !strcasecmp(v, "yes") || !strcasecmp(v, "true") || !strcasecmp(v, "on"); }
MK int mk_bool_valid(const char *v) { return mk_bool(v) || !strcmp(v, "0") || !strcasecmp(v, "no") || !strcasecmp(v, "false") || !strcasecmp(v, "off"); }

/* integer in [lo,hi] */
MK int mk_int(const char *v, int lo, int hi, int *o)
{
	char *e;
	long x = strtol(v, &e, 10);
	if (!*v || *e || x < lo || x > hi) return -1;
	*o = (int)x;
	return 0;
}

MK void mk_path(char *o, size_t n, const char *dir, const char *name, const char *ext) { snprintf(o, n, "%s/%s.%s", dir, name, ext); }

MK int mk_mkdirs(const char *d) { return ent_mkdirs(d); }
#endif
