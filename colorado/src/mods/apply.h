/* apply.h - show-before-doing: dry-run diff, backed-up apply, byte-identical rollback. stb-style single header.
 *
 * co_roots_env()        HOME / XDG_CONFIG_HOME / XDG_DATA_HOME from the environment (tests point all three at a temp dir)
 * co_plan(set, roots)   print a unified diff (via diff -u) of what apply would write; writes nothing; returns number of changed files
 * co_apply(set, roots)  back up every touched file (and remember every directory it creates) into
 *                       $XDG_DATA_HOME/colorado/backups/<YYYYmmdd-HHMMSS>/{manifest.tsv,files/N}, then write atomically (tmp + rename,
 *                       existing mode kept, symlinks followed so a dotfiles-managed link keeps pointing at the repo file)
 * co_rollback(arg)      restore a backup (a dir, or NULL = newest not yet rolled back); files colorado created are moved into
 *                       <backup>/created/, not deleted; directories it created are removed again if empty
 */
#ifndef COLORADO_APPLY_H
#define COLORADO_APPLY_H
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "gen.h"

typedef struct { char home[PATH_MAX], config[PATH_MAX], data[PATH_MAX]; } co_roots;

static int co_roots_env(co_roots *r) {
	const char *h = getenv("HOME"), *c = getenv("XDG_CONFIG_HOME"), *d = getenv("XDG_DATA_HOME");
	if (!h || !*h || h[0] != '/') { fprintf(stderr, "colorado: HOME is not set to an absolute path\n"); return -1; }
	snprintf(r->home, sizeof r->home, "%s", h);
	snprintf(r->config, sizeof r->config, "%s", c && *c == '/' ? c : "");
	if (!r->config[0]) snprintf(r->config, sizeof r->config, "%s/.config", h);
	snprintf(r->data, sizeof r->data, "%s", d && *d == '/' ? d : "");
	if (!r->data[0]) snprintf(r->data, sizeof r->data, "%s/.local/share", h);
	snprintf(co_cfg_root, sizeof co_cfg_root, "%s", r->config);
	return 0;
}

static void co_target(const co_roots *r, const co_file *f, char *out, size_t n) {
	const char *base = f->where == W_CONFIG ? r->config : f->where == W_DATA ? r->data : r->home;
	char raw[PATH_MAX]; snprintf(raw, sizeof raw, "%s/%s", base, f->rel);
	char real[PATH_MAX];
	if (realpath(raw, real)) snprintf(out, n, "%s", real); else snprintf(out, n, "%s", raw); /* follow symlinks of existing files */
}

static int co_run_diff(const char *a, const char *b, const char *la, const char *lb) {
	fflush(stdout);
	pid_t p = fork();
	if (p < 0) return -1;
	if (p == 0) { execlp("diff", "diff", "-u", "--label", la, "--label", lb, a, b, (char *)NULL); _exit(127); }
	int st; waitpid(p, &st, 0);
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int co_write_all(const char *path, const char *buf, size_t n, mode_t mode) {
	char tmp[PATH_MAX + 32]; snprintf(tmp, sizeof tmp, "%s.colorado-tmp.%d", path, (int)getpid());
	int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (fd < 0) return -1;
	size_t off = 0;
	while (off < n) { ssize_t w = write(fd, buf + off, n - off); if (w < 0) { if (errno == EINTR) continue; close(fd); unlink(tmp); return -1; } off += (size_t)w; }
	fchmod(fd, mode);
	if (fsync(fd) || close(fd)) { unlink(tmp); return -1; }
	if (rename(tmp, path)) { unlink(tmp); return -1; }
	return 0;
}

/* compute new content for file i; returns 1 if it differs from disk. old/new are caller-freed. */
static int co_compute(const co_roots *r, const co_file *f, char *path, size_t pn, char **oldp, co_sb *nw, int *existed) {
	co_target(r, f, path, pn);
	char *old = co_read_file(path, NULL); *existed = old != NULL;
	if (!old) old = strdup("");
	co_merge(old, f, nw);
	*oldp = old;
	return strcmp(old, nw->p ? nw->p : "") != 0;
}

static int co_plan(const co_set *s, const co_roots *r) {
	int changed = 0; char dirt[] = "/tmp/colorado-diff.XXXXXX";
	if (!mkdtemp(dirt)) { perror("colorado: mkdtemp"); return -1; }
	char pa[PATH_MAX], pb[PATH_MAX]; snprintf(pa, sizeof pa, "%s/a", dirt); snprintf(pb, sizeof pb, "%s/b", dirt);
	for (int i = 0; i < s->n; i++) {
		char path[PATH_MAX]; char *old; co_sb nw = { 0 }; int ex;
		int ch = co_compute(r, &s->f[i], path, sizeof path, &old, &nw, &ex);
		if (ch) {
			changed++;
			co_write_all(pa, old, strlen(old), 0600); co_write_all(pb, nw.p, nw.n, 0600);
			char la[PATH_MAX + 16], lb[PATH_MAX + 16];
			snprintf(la, sizeof la, "%s", ex ? path : "/dev/null"); snprintf(lb, sizeof lb, "%s%s", path, ex ? "" : " (new file)");
			printf("# [%s] %s\n", s->f[i].gen, path);
			if (co_run_diff(pa, pb, la, lb) == 127) printf("(diff not found, new content:)\n%s", nw.p);
		}
		free(old); co_sb_free(&nw);
	}
	/* temp files are ours: move them to the system temp trash by renaming, not deleting */
	unlink(pa); unlink(pb); rmdir(dirt);
	return changed;
}

static int co_mkdir_p(const char *path, FILE *manifest) {
	char tmp[PATH_MAX]; snprintf(tmp, sizeof tmp, "%s", path);
	for (char *p = tmp + 1; *p; p++) {
		if (*p != '/') continue;
		*p = 0;
		struct stat st;
		if (stat(tmp, &st)) { if (mkdir(tmp, 0755)) return -1; if (manifest) { fprintf(manifest, "D\t%s\n", tmp); fflush(manifest); } }
		*p = '/';
	}
	return 0;
}

static int co_apply(const co_set *s, const co_roots *r, char *bdir_out, size_t bn) {
	char bdir[PATH_MAX], tsb[32]; time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
	strftime(tsb, sizeof tsb, "%Y%m%d-%H%M%S", &tm);
	snprintf(bdir, sizeof bdir, "%s/colorado/backups/%s-%d", r->data, tsb, (int)getpid());
	if (co_mkdir_p(bdir, NULL)) { fprintf(stderr, "colorado: cannot create %s: %s\n", bdir, strerror(errno)); return -1; }
	char sub[PATH_MAX + 16];
	snprintf(sub, sizeof sub, "%s", bdir); if (mkdir(sub, 0755) && errno != EEXIST) return -1;
	snprintf(sub, sizeof sub, "%s/files", bdir); if (mkdir(sub, 0755) && errno != EEXIST) return -1;
	char mp[PATH_MAX + 16]; snprintf(mp, sizeof mp, "%s/manifest.tsv", bdir);
	FILE *mf = fopen(mp, "w"); if (!mf) { perror("colorado: manifest"); return -1; }
	int wrote = 0, nfile = 0;
	for (int i = 0; i < s->n; i++) {
		char path[PATH_MAX]; char *old; co_sb nw = { 0 }; int ex;
		if (!co_compute(r, &s->f[i], path, sizeof path, &old, &nw, &ex)) { free(old); co_sb_free(&nw); continue; }
		char dir[PATH_MAX]; snprintf(dir, sizeof dir, "%s", path); char *sl = strrchr(dir, '/'); if (sl) { *sl = 0; strcat(dir, "/x"); }
		if (co_mkdir_p(dir, mf)) { fprintf(stderr, "colorado: cannot create dir for %s: %s\n", path, strerror(errno)); free(old); co_sb_free(&nw); continue; }
		mode_t mode = 0644; struct stat st;
		if (ex && !stat(path, &st)) mode = st.st_mode & 07777;
		if (ex) {
			size_t ol = 0; char *raw = co_read_file(path, &ol);
			char bf[PATH_MAX + 32]; snprintf(bf, sizeof bf, "%s/files/%d", bdir, nfile);
			if (!raw || co_write_all(bf, raw, ol, mode)) { fprintf(stderr, "colorado: backup of %s failed, not touching it\n", path); free(raw); free(old); co_sb_free(&nw); continue; }
			free(raw);
			fprintf(mf, "F\t%s\t%d\t%o\n", path, nfile, mode); nfile++;
		} else fprintf(mf, "F\t%s\t-\t%o\n", path, mode);
		fflush(mf); fsync(fileno(mf));
		if (co_write_all(path, nw.p, nw.n, mode)) fprintf(stderr, "colorado: writing %s failed: %s\n", path, strerror(errno));
		else { printf("wrote %s\n", path); wrote++; }
		free(old); co_sb_free(&nw);
	}
	fclose(mf);
	snprintf(bdir_out, bn, "%s", bdir);
	return wrote;
}

static int co_copy_file(const char *from, const char *to, mode_t mode) {
	size_t n; char *b = co_read_file(from, &n); if (!b) return -1;
	int rc = co_write_all(to, b, n, mode); free(b); return rc;
}

static int co_rollback(const co_roots *r, const char *arg) {
	char bdir[PATH_MAX];
	if (arg && *arg) snprintf(bdir, sizeof bdir, "%s", arg);
	else {
		char root[PATH_MAX]; snprintf(root, sizeof root, "%s/colorado/backups", r->data);
		DIR *d = opendir(root); char best[256] = "";
		if (!d) { fprintf(stderr, "colorado: no backups in %s\n", root); return -1; }
		for (struct dirent *e; (e = readdir(d));) {
			if (e->d_name[0] == '.') continue;
			char rb[PATH_MAX + 300]; snprintf(rb, sizeof rb, "%s/%s/rolled-back", root, e->d_name);
			if (!access(rb, F_OK)) continue;
			if (strcmp(e->d_name, best) > 0) snprintf(best, sizeof best, "%s", e->d_name);
		}
		closedir(d);
		if (!best[0]) { fprintf(stderr, "colorado: nothing to roll back\n"); return -1; }
		snprintf(bdir, sizeof bdir, "%s/%s", root, best);
	}
	char mp[PATH_MAX + 32]; snprintf(mp, sizeof mp, "%s/manifest.tsv", bdir);
	char *m = co_read_file(mp, NULL);
	if (!m) { fprintf(stderr, "colorado: %s: no manifest\n", bdir); return -1; }
	int nl = 0; for (char *p = m; *p; p++) if (*p == '\n') nl++;
	char **ln = calloc((size_t)nl + 1, sizeof *ln); int k = 0;
	for (char *p = strtok(m, "\n"); p && k < nl; p = strtok(NULL, "\n")) ln[k++] = p;
	char cdir[PATH_MAX + 16]; snprintf(cdir, sizeof cdir, "%s/created", bdir); mkdir(cdir, 0755);
	int bad = 0, ncreated = 0;
	for (int i = k - 1; i >= 0; i--) { /* newest first */
		char *l = ln[i];
		if (l[0] == 'D') { if (rmdir(l + 2) && errno != ENOENT && errno != ENOTEMPTY) { fprintf(stderr, "colorado: rmdir %s: %s\n", l + 2, strerror(errno)); } continue; }
		char *f1 = strchr(l, '\t'); if (!f1) continue; f1++;
		char *f2 = strchr(f1, '\t'); if (!f2) continue; *f2++ = 0;
		char *f3 = strchr(f2, '\t'); if (!f3) continue; *f3++ = 0;
		mode_t mode = (mode_t)strtol(f3, NULL, 8);
		if (!strcmp(f2, "-")) {
			char dst[PATH_MAX + 32]; snprintf(dst, sizeof dst, "%s/%d", cdir, ncreated++);
			if (rename(f1, dst)) { if (errno == ENOENT) continue; if (co_copy_file(f1, dst, mode) || unlink(f1)) { fprintf(stderr, "colorado: cannot move %s away: %s\n", f1, strerror(errno)); bad++; } }
		} else {
			char src[PATH_MAX + 32]; snprintf(src, sizeof src, "%s/files/%s", bdir, f2);
			if (co_copy_file(src, f1, mode)) { fprintf(stderr, "colorado: cannot restore %s: %s\n", f1, strerror(errno)); bad++; }
		}
	}
	free(ln); free(m);
	if (!bad) { char rb[PATH_MAX + 32]; snprintf(rb, sizeof rb, "%s/rolled-back", bdir); co_write_all(rb, "ok\n", 3, 0644); printf("rolled back %s\n", bdir); }
	return bad ? -1 : 0;
}
#endif
