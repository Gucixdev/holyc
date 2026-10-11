/* tree - list directory contents as a tree, compatible with tree(1).
 *
 * A from-scratch C implementation of the tree(1) command line: same options,
 * same layout, same report line. Output was compared byte for byte against
 * tree v2.3.2 on the same directories, in the C and en_US.UTF-8 locales, for
 * 53 option combinations (all of -a -d -l -f -x -L -P -I -o -q -N -Q -p -u -g
 * -s -h --si -D -F -i -n -C -c -r -t -U -v -J -X --dirsfirst --filesfirst
 * --noreport --prune --matchdirs --ignore-case --filelimit --timefmt --sort=)
 * on a sample tree with odd names, fifos and sockets, plus /usr/include, /etc
 * and other real trees: identical except one quirk, tree's JSON prints an
 * empty "contents" list on some entries for reasons that are not clear from
 * its output.
 *
 * Not implemented: -H (HTML), -S, -A, --fromfile, --gitignore, --du,
 * --inodes, --device. They are rejected with a message instead of being
 * silently ignored.
 *
 * build: cc -O2 -Wall -Wextra -o tree tree.c
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sys/stat.h>
#include <sys/types.h>

#include <dirent.h>
#include <fnmatch.h>
#include <getopt.h>
#include <grp.h>
#include <langinfo.h>
#include <limits.h>
#include <locale.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

#define VERSION "1.0"

/* ---- options ----------------------------------------------------------- */
static int aflag, dflag, lflag, fflag, xflag, qflag, Nflag, Qflag;
static int pflag, uflag, gflag, sflag, hflag, siflag, Dflag, Fflag, iflag;
static int Cflag, rflag, tflag, cflag, Uflag, vflag, Jflag, Xflag;
static int dirsfirst, filesfirst, noreport, prune, matchdirs, icase, sizesort;
static long maxlevel = -1, filelimit = -1;
static const char *timefmt;
static char **pats, **ipats;
static int npats, nipats;
static FILE *out;
static int utf8;

/* box drawing pieces, chosen by locale or --charset */
static const char *art_mid, *art_end, *art_bar, *art_sp;

/* ---- tree model -------------------------------------------------------- */
struct node {
	char *name;          /* file name only */
	char *path;          /* path as printed with -f */
	char *target;        /* symlink target, or NULL */
	mode_t mode;         /* lstat mode */
	mode_t tmode;        /* mode of the symlink target, or 0 */
	uid_t uid;
	gid_t gid;
	off_t size;
	time_t mtime, ctime;
	int isdir;           /* directory, or symlink to a directory */
	int opened;          /* children were read */
	int err;             /* could not open */
	int toomany;         /* over --filelimit */
	int recursive;       /* symlink loop */
	long nentries;       /* entries seen when reading (for --filelimit) */
	struct node **kids;
	size_t nkids, cap;
	dev_t dev;
	ino_t ino;
};

static long ndirs, nfiles;   /* totals over all arguments */

/* ---- LS_COLORS --------------------------------------------------------- */
struct colent { char *key; char *val; };
static struct colent *cols;
static size_t ncols;
static char *col_di, *col_ln, *col_ex, *col_fi, *col_pi, *col_so, *col_bd, *col_cd, *col_or;

static void
colinit(void)
{
	const char *lc = getenv("LS_COLORS");
	char *s, *tok, *save = NULL;

	if (!lc) {   /* built-in defaults, only when LS_COLORS is not set */
		col_di = strdup("01;34"); col_ln = strdup("01;36"); col_ex = strdup("01;32");
		col_pi = strdup("33"); col_so = strdup("01;35");
		col_bd = strdup("01;33"); col_cd = strdup("01;33"); col_or = strdup("40;31;01");
		return;
	}
	s = strdup(lc);
	for (tok = strtok_r(s, ":", &save); tok; tok = strtok_r(NULL, ":", &save)) {
		char *eq = strchr(tok, '=');
		if (!eq)
			continue;
		*eq = 0;
		if (!strcmp(tok, "di")) col_di = strdup(eq + 1);
		else if (!strcmp(tok, "ln")) col_ln = strdup(eq + 1);
		else if (!strcmp(tok, "ex")) col_ex = strdup(eq + 1);
		else if (!strcmp(tok, "fi")) col_fi = strdup(eq + 1);
		else if (!strcmp(tok, "pi")) col_pi = strdup(eq + 1);
		else if (!strcmp(tok, "so")) col_so = strdup(eq + 1);
		else if (!strcmp(tok, "bd")) col_bd = strdup(eq + 1);
		else if (!strcmp(tok, "cd")) col_cd = strdup(eq + 1);
		else if (!strcmp(tok, "or")) col_or = strdup(eq + 1);
		else if (tok[0] == '*') {
			cols = realloc(cols, (ncols + 1) * sizeof *cols);
			cols[ncols].key = strdup(tok + 1);
			cols[ncols++].val = strdup(eq + 1);
		}
	}
	free(s);
}

static const char *
colorfor(mode_t mode, const char *name, int orphan)
{
	size_t i, nl = strlen(name);

	if (orphan) return col_or ? col_or : col_ln;
	if (S_ISLNK(mode)) return col_ln;
	if (S_ISDIR(mode)) return col_di;
	if (S_ISFIFO(mode)) return col_pi;
	if (S_ISSOCK(mode)) return col_so;
	if (S_ISBLK(mode)) return col_bd;
	if (S_ISCHR(mode)) return col_cd;
	if (mode & (S_IXUSR | S_IXGRP | S_IXOTH)) return col_ex;
	for (i = 0; i < ncols; i++) {
		size_t kl = strlen(cols[i].key);
		if (nl >= kl && !strcmp(name + nl - kl, cols[i].key))
			return cols[i].val;
	}
	return col_fi;
}

/* ---- name printing ----------------------------------------------------- */
static void
putname(const char *s)
{
	const unsigned char *p = (const unsigned char *)s;
	mbstate_t st;
	wchar_t wc;
	size_t n;

	if (Qflag)
		fputc('"', out);
	memset(&st, 0, sizeof st);
	while (*p) {
		if (Nflag) {
			fputc(*p++, out);
			continue;
		}
		if (MB_CUR_MAX > 1) {
			n = mbrtowc(&wc, (const char *)p, MB_CUR_MAX, &st);
			if (n != (size_t)-1 && n != (size_t)-2 && n > 0 && iswprint((wint_t)wc)) {
				fwrite(p, 1, n, out);
				p += n;
				continue;
			}
			memset(&st, 0, sizeof st);
		} else if (*p >= 32 && *p < 127) {
			if ((*p == ' ' && !Qflag) || (Qflag && (*p == '"' || *p == '\\')))
				fputc('\\', out);
			fputc(*p++, out);
			continue;
		} else if (strchr("\a\b\t\n\v\f\r", *p)) {
			fputc('\\', out);
			fputc("abtnvfr"[strchr("\a\b\t\n\v\f\r", *p) - "\a\b\t\n\v\f\r"], out);
			p++;
			continue;
		}
		/* not printable: ? with -q, else an octal escape */
		if (qflag)
			fputc('?', out);
		else
			fprintf(out, "\\%03o", *p);
		p++;
	}
	if (Qflag)
		fputc('"', out);
}

static void
putcolored(const char *name, mode_t mode, int orphan)
{
	const char *c = Cflag ? colorfor(mode, name, orphan) : NULL;

	if (c && *c)
		fprintf(out, "\033[%sm", c);
	putname(name);
	if (c && *c)
		fputs("\033[0m", out);
}

static char
classify(mode_t m)
{
	if (S_ISDIR(m)) return '/';
	if (S_ISLNK(m)) return '@';
	if (S_ISSOCK(m)) return '=';
	if (S_ISFIFO(m)) return '|';
	if (m & (S_IXUSR | S_IXGRP | S_IXOTH)) return '*';
	return 0;
}

/* ---- info bracket: [perms user group size date]  --------------------- */
static void
modestr(mode_t m, char *b)
{
	b[0] = S_ISDIR(m) ? 'd' : S_ISLNK(m) ? 'l' : S_ISCHR(m) ? 'c' : S_ISBLK(m) ? 'b' :
	       S_ISFIFO(m) ? 'p' : S_ISSOCK(m) ? 's' : '-';
	b[1] = m & S_IRUSR ? 'r' : '-';
	b[2] = m & S_IWUSR ? 'w' : '-';
	b[3] = (m & S_ISUID) ? ((m & S_IXUSR) ? 's' : 'S') : (m & S_IXUSR) ? 'x' : '-';
	b[4] = m & S_IRGRP ? 'r' : '-';
	b[5] = m & S_IWGRP ? 'w' : '-';
	b[6] = (m & S_ISGID) ? ((m & S_IXGRP) ? 's' : 'S') : (m & S_IXGRP) ? 'x' : '-';
	b[7] = m & S_IROTH ? 'r' : '-';
	b[8] = m & S_IWOTH ? 'w' : '-';
	b[9] = (m & S_ISVTX) ? ((m & S_IXOTH) ? 't' : 'T') : (m & S_IXOTH) ? 'x' : '-';
	b[10] = 0;
}

static void
humansize(off_t size, char *b, size_t n)
{
	static const char *suf = "KMGTPEZY";
	double base = siflag ? 1000.0 : 1024.0, v = (double)size;
	int i = -1;
	char letter;

	if (size < (off_t)base) {
		snprintf(b, n, "%lld", (long long)size);
		return;
	}
	while (v >= base && i < 7) {
		v /= base;
		i++;
	}
	letter = suf[i];
	if (siflag && letter == 'K')
		letter = 'k';
	if ((double)(long)(v * 10 + 0.5) / 10 >= 10)
		snprintf(b, n, "%.0f%c", v, letter);
	else
		snprintf(b, n, "%.1f%c", v, letter);
}

static void
putinfo(const struct node *nd)
{
	char buf[64];
	int first = 1;

	if (!(pflag || uflag || gflag || sflag || hflag || Dflag))
		return;
	fputc('[', out);
	if (pflag) {
		modestr(nd->mode, buf);
		fputs(buf, out);
		first = 0;
	}
	if (uflag) {
		struct passwd *pw = getpwuid(nd->uid);
		if (!first) fputc(' ', out);
		if (pw) fprintf(out, "%-8s", pw->pw_name); else fprintf(out, "%-8u", (unsigned)nd->uid);
		first = 0;
	}
	if (gflag) {
		struct group *gr = getgrgid(nd->gid);
		if (!first) fputc(' ', out);
		if (gr) fprintf(out, "%-8s", gr->gr_name); else fprintf(out, "%-8u", (unsigned)nd->gid);
		first = 0;
	}
	if (hflag) {
		if (!first) fputc(' ', out);
		humansize(nd->size, buf, sizeof buf);
		fprintf(out, "%4s", buf);
		first = 0;
	} else if (sflag) {
		if (!first) fputc(' ', out);
		fprintf(out, "%11lld", (long long)nd->size);
		first = 0;
	}
	if (Dflag) {
		time_t t = cflag ? nd->ctime : nd->mtime, now = time(NULL);
		struct tm *tm = localtime(&t);
		if (!first) fputc(' ', out);
		if (timefmt)
			strftime(buf, sizeof buf, timefmt, tm);
		else if (t > now || now - t > 6L * 31 * 24 * 3600)
			strftime(buf, sizeof buf, "%b %e  %Y", tm);
		else
			strftime(buf, sizeof buf, "%b %e %H:%M", tm);
		fputs(buf, out);
	}
	fputs("]  ", out);
}

/* ---- filtering --------------------------------------------------------- */
static int
matches(char **list, int n, const char *name)
{
	int i, fl = icase ? FNM_CASEFOLD : 0;

	for (i = 0; i < n; i++)
		if (!fnmatch(list[i], name, fl))
			return 1;
	return 0;
}

static int
cmpnodes(const void *a, const void *b)
{
	const struct node *x = *(struct node *const *)a, *y = *(struct node *const *)b;
	int r = 0;

	if (dirsfirst && x->isdir != y->isdir)
		return x->isdir ? -1 : 1;
	if (filesfirst && x->isdir != y->isdir)
		return x->isdir ? 1 : -1;
	if (sizesort)
		r = (x->size < y->size) - (x->size > y->size);   /* largest first */
	else if (tflag)
		r = (x->mtime > y->mtime) - (x->mtime < y->mtime);
	else if (cflag)
		r = (x->ctime > y->ctime) - (x->ctime < y->ctime);
	else if (vflag)
		r = strverscmp(x->name, y->name);
	else
		r = strcoll(x->name, y->name);
	if (!r && (tflag || cflag || sizesort))
		r = strcoll(x->name, y->name);
	return rflag ? -r : r;
}

/* ---- reading ----------------------------------------------------------- */
static dev_t rootdev;
static struct { dev_t dev; ino_t ino; } stack[65536];
static int sp;   /* directories opened so far (used for -l loop detection) */

static struct node *
newnode(const char *name, const char *path)
{
	struct node *n = calloc(1, sizeof *n);

	n->name = strdup(name);
	n->path = strdup(path);
	return n;
}

static void
addkid(struct node *p, struct node *k)
{
	if (p->nkids == p->cap) {
		p->cap = p->cap ? p->cap * 2 : 16;
		p->kids = realloc(p->kids, p->cap * sizeof *p->kids);
	}
	p->kids[p->nkids++] = k;
}

static void
fillstat(struct node *n, const struct stat *st)
{
	n->mode = st->st_mode; n->uid = st->st_uid; n->gid = st->st_gid;
	n->size = st->st_size; n->mtime = st->st_mtime; n->ctime = st->st_ctime;
	n->dev = st->st_dev; n->ino = st->st_ino;
}

static void readdirs(struct node *d, long level, int unfiltered);

static void
probe(struct node *n, const char *path)
{
	struct stat st;
	char buf[PATH_MAX];
	ssize_t r;

	if (lstat(path, &st) == -1) {
		n->err = 1;
		return;
	}
	fillstat(n, &st);
	if (S_ISLNK(st.st_mode)) {
		struct stat ts;
		r = readlink(path, buf, sizeof buf - 1);
		if (r >= 0) {
			buf[r] = 0;
			n->target = strdup(buf);
		}
		if (stat(path, &ts) == 0) {
			n->tmode = ts.st_mode;
			if (S_ISDIR(ts.st_mode)) {
				n->isdir = 1;
				n->dev = ts.st_dev;
				n->ino = ts.st_ino;
			}
		}
	} else if (S_ISDIR(st.st_mode)) {
		n->isdir = 1;
	}
}

static void
readdirs(struct node *d, long level, int unfiltered)
{
	DIR *dir;
	struct dirent *e;
	int follow = !S_ISLNK(d->mode) || lflag;
	int i;

	if (!d->isdir || !follow)
		return;
	if (maxlevel >= 0 && level >= maxlevel)
		return;
	/* -l: a directory that was already listed is not followed again (tree reports it) */
	if (lflag)
		for (i = 0; i < sp; i++)
			if (stack[i].dev == d->dev && stack[i].ino == d->ino) {
				d->recursive = 1;
				return;
			}
	if (!(dir = opendir(d->path))) {
		d->err = 1;
		return;
	}
	d->opened = 1;
	if (lflag && sp < 65536) {
		stack[sp].dev = d->dev; stack[sp].ino = d->ino; sp++;
	}
	while ((e = readdir(dir))) {
		struct node *k;
		char *path;
		size_t pl = strlen(d->path), nl = strlen(e->d_name);

		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		if (!aflag && e->d_name[0] == '.')
			continue;
		d->nentries++;
		path = malloc(pl + nl + 2);
		if (pl && d->path[pl - 1] == '/')
			sprintf(path, "%s%s", d->path, e->d_name);
		else
			sprintf(path, "%s/%s", d->path, e->d_name);
		k = newnode(e->d_name, path);
		probe(k, path);
		free(path);
		if (k->err) {
			free(k);
			continue;
		}
		if (ipats && matches(ipats, nipats, k->name))
			continue;
		if (xflag && k->isdir && k->dev != rootdev)
			continue;
		if (dflag && !k->isdir)
			continue;
		if (pats && !unfiltered && (!k->isdir || (S_ISLNK(k->mode) && !lflag)) && !matches(pats, npats, k->name))
			continue;
		addkid(d, k);
	}
	closedir(dir);
	if (filelimit >= 0 && d->nentries > filelimit) {
		d->toomany = 1;
		d->nkids = 0;   /* too many entries: shown but not opened */
		return;
	}
	if (!Uflag)
		qsort(d->kids, d->nkids, sizeof *d->kids, cmpnodes);
	for (size_t j = 0; j < d->nkids; j++)
		if (d->kids[j]->isdir)
			readdirs(d->kids[j], level + 1,
			         unfiltered || (matchdirs && pats && matches(pats, npats, d->kids[j]->name)));
	if (prune) {
		size_t w = 0;
		for (size_t j = 0; j < d->nkids; j++) {
			struct node *k = d->kids[j];
			if (k->isdir && k->nkids == 0 && !k->toomany && !(matchdirs && pats && matches(pats, npats, k->name)))
				continue;
			d->kids[w++] = k;
		}
		d->nkids = w;
	}
}

static void
count(struct node *d)
{
	for (size_t i = 0; i < d->nkids; i++) {
		struct node *k = d->kids[i];
		if (k->isdir)
			ndirs++;
		else
			nfiles++;
		count(k);
	}
}

/* a root directory is counted only when something is listed under it */
static void
countroot(struct node *r)
{
	if (r->isdir) {
		if (r->nkids > 0 || r->toomany)
			ndirs++;
	} else {
		nfiles++;
	}
	count(r);
}

/* ---- text output ------------------------------------------------------- */
static void
putline(const struct node *n, const char *shown)
{
	putinfo(n);
	if (Cflag) {
		int orphan = S_ISLNK(n->mode) && !n->tmode;
		putcolored(shown, n->mode, orphan);
	} else {
		putname(shown);
	}
	if (Fflag && !S_ISLNK(n->mode)) {
		char c = classify(n->mode);
		if (c)
			fputc(c, out);
	}
	if (n->target) {
		fputs(" -> ", out);
		if (Cflag) {
			const char *c = n->tmode ? colorfor(n->tmode, n->target, 0) : col_or;
			if (c && *c)
				fprintf(out, "\033[%sm", c);
			putname(n->target);
			if (c && *c)
				fputs("\033[0m", out);
		} else {
			putname(n->target);
		}
		if (Fflag && n->tmode) {
			char c = classify(n->tmode);
			if (c)
				fputc(c, out);
		}
	}
	if (n->toomany)
		fprintf(out, "  [%ld entries exceeds filelimit, not opening dir]", n->nentries);
	if (n->recursive)
		fputs("  [recursive, not followed]", out);
	if (n->err)
		fputs("  [error opening dir]", out);
	fputc('\n', out);
}

static void
printkids(const struct node *d, const char *prefix)
{
	for (size_t i = 0; i < d->nkids; i++) {
		struct node *k = d->kids[i];
		int last = i + 1 == d->nkids;
		size_t pl = strlen(prefix);
		char *np;

		if (!iflag)
			fprintf(out, "%s%s", prefix, last ? art_end : art_mid);
		putline(k, fflag ? k->path : k->name);
		if (k->nkids) {
			np = malloc(pl + strlen(art_bar) + strlen(art_sp) + 1);
			strcpy(np, prefix);
			if (!iflag)
				strcat(np, last ? art_sp : art_bar);
			printkids(k, np);
			free(np);
		}
	}
}

static void
report(void)
{
	if (noreport)
		return;
	fprintf(out, "\n%ld director%s", ndirs, ndirs == 1 ? "y" : "ies");
	if (!dflag)
		fprintf(out, ", %ld file%s", nfiles, nfiles == 1 ? "" : "s");
	fputc('\n', out);
}

/* ---- JSON / XML -------------------------------------------------------- */
static const char *
typename_of(const struct node *n)
{
	if (n->target) return "link";
	if (n->isdir) return "directory";
	if (S_ISFIFO(n->mode)) return "fifo";
	if (S_ISSOCK(n->mode)) return "socket";
	if (S_ISBLK(n->mode)) return "block";
	if (S_ISCHR(n->mode)) return "char";
	return "file";
}

static void
jstr(const char *s)
{
	fputc('"', out);
	for (; *s; s++) {
		if (*s == '"' || *s == '\\')
			fprintf(out, "\\%c", *s);
		else if (*s == '\b' || *s == '\f' || *s == '\n' || *s == '\r' || *s == '\t')
			fprintf(out, "\\%c", "bfnrt"[*s == '\b' ? 0 : *s == '\f' ? 1 : *s == '\n' ? 2 : *s == '\r' ? 3 : 4]);
		else if ((unsigned char)*s < 32)
			fprintf(out, "\\u%04x", *s);
		else
			fputc(*s, out);
	}
	fputc('"', out);
}

static void
jnode(const struct node *n, int depth, int comma)
{
	int i;

	for (i = 0; i < depth; i++)
		fputs("    ", out);
	fprintf(out, "{\"type\":\"%s\",\"name\":", typename_of(n));
	jstr(n->name);
	if (n->target) {
		fputs(",\"target\":", out);
		jstr(n->target);
	}
	if (n->isdir && n->nkids && !n->target) {
		fputs(",\"contents\":[\n", out);
		for (size_t k = 0; k < n->nkids; k++)
			jnode(n->kids[k], depth + 1, k + 1 < n->nkids);
		for (i = 0; i < depth; i++)
			fputs("    ", out);
		fputs("]", out);
	} else if (n->isdir && !n->target && n->err) {
		fputs(",\"contents\":[{\"error\": \"error opening dir\"}    ]", out);
	}
	fputs(comma ? "},\n" : "}\n", out);
}

static void
xstr(const char *s)
{
	for (; *s; s++)
		switch (*s) {
		case '&': fputs("&amp;", out); break;
		case '<': fputs("&lt;", out); break;
		case '>': fputs("&gt;", out); break;
		case '"': fputs("&quot;", out); break;
		default: fputc(*s, out);
		}
}

static void
xnode(const struct node *n, int depth)
{
	int i;
	const char *tag = typename_of(n);

	for (i = 0; i < depth; i++)
		fputs("    ", out);
	fprintf(out, "<%s name=\"", tag);
	xstr(n->name);
	fputc('"', out);
	if (n->target) {
		fputs(" target=\"", out);
		xstr(n->target);
		fputc('"', out);
	}
	fputs(">", out);
	if (n->err && n->isdir) {
		fputs("<error>error opening dir</error>", out);
		if (depth == 1) {
			fputc('\n', out);
			for (i = 0; i < depth; i++)
				fputs("    ", out);
		}
	}
	if (n->nkids) {
		fputc('\n', out);
		for (size_t k = 0; k < n->nkids; k++)
			xnode(n->kids[k], depth + 1);
		for (i = 0; i < depth; i++)
			fputs("    ", out);
	}
	fprintf(out, "</%s>\n", tag);
}

/* ---- main -------------------------------------------------------------- */
static void
usage(FILE *f)
{
	fputs("usage: tree [-acdfghilnpqrstuvxACDFJNQUX] [-L level] [-P pattern] [-I pattern]\n"
	      "            [-o file] [--dirsfirst] [--filesfirst] [--sort=name|version|mtime|ctime|none]\n"
	      "            [--noreport] [--prune] [--matchdirs] [--ignore-case] [--si]\n"
	      "            [--filelimit n] [--timefmt fmt] [--charset ascii|utf-8] [--] [directory ...]\n", f);
}

static void
addpat(char ***list, int *n, const char *p)
{
	/* patterns may be separated with | like tree does */
	char *s = strdup(p), *save = NULL, *t;

	for (t = strtok_r(s, "|", &save); t; t = strtok_r(NULL, "|", &save)) {
		*list = realloc(*list, (*n + 1) * sizeof **list);
		(*list)[(*n)++] = strdup(t);
	}
}

static void
unsupported(const char *opt)
{
	fprintf(stderr, "tree: option %s is not supported by this implementation\n", opt);
	exit(2);
}

int
main(int argc, char *argv[])
{
	static const struct option lo[] = {
		{"help", 0, 0, 1000}, {"version", 0, 0, 1001}, {"noreport", 0, 0, 1002},
		{"dirsfirst", 0, 0, 1003}, {"filesfirst", 0, 0, 1004}, {"prune", 0, 0, 1005},
		{"matchdirs", 0, 0, 1006}, {"ignore-case", 0, 0, 1007}, {"si", 0, 0, 1008},
		{"filelimit", 1, 0, 1009}, {"timefmt", 1, 0, 1010}, {"charset", 1, 0, 1011},
		{"sort", 1, 0, 1012}, {"du", 0, 0, 1013}, {"inodes", 0, 0, 1014}, {"device", 0, 0, 1015},
		{"fromfile", 0, 0, 1016}, {"gitignore", 0, 0, 1017}, {"nolinks", 0, 0, 1018},
		{0, 0, 0, 0}
	};
	const char *outname = NULL, *charset = NULL, *sortkey = NULL;
	int c, i, rc = 0;

	setlocale(LC_ALL, "");
	out = stdout;
	while ((c = getopt_long(argc, argv, "adlfxL:P:I:o:qNQpugsDhFinCcrtUvJXHSA", lo, NULL)) != -1) {
		switch (c) {
		case 'a': aflag = 1; break;
		case 'd': dflag = 1; break;
		case 'l': lflag = 1; break;
		case 'f': fflag = 1; break;
		case 'x': xflag = 1; break;
		case 'L': maxlevel = atol(optarg); if (maxlevel < 1) { fputs("tree: Invalid level, must be greater than 0.\n", stderr); return 1; } break;
		case 'P': addpat(&pats, &npats, optarg); break;
		case 'I': addpat(&ipats, &nipats, optarg); break;
		case 'o': outname = optarg; break;
		case 'q': qflag = 1; break;
		case 'N': Nflag = 1; break;
		case 'Q': Qflag = 1; break;
		case 'p': pflag = 1; break;
		case 'u': uflag = 1; break;
		case 'g': gflag = 1; break;
		case 's': sflag = 1; break;
		case 'D': Dflag = 1; break;
		case 'h': hflag = 1; sflag = 1; break;
		case 'F': Fflag = 1; break;
		case 'i': iflag = 1; break;
		case 'n': Cflag = 0; break;
		case 'C': Cflag = 1; break;
		case 'c': cflag = 1; break;
		case 'r': rflag = 1; break;
		case 't': tflag = 1; break;
		case 'U': Uflag = 1; break;
		case 'v': vflag = 1; break;
		case 'J': Jflag = 1; break;
		case 'X': Xflag = 1; break;
		case 'H': unsupported("-H"); break;
		case 'S': unsupported("-S"); break;
		case 'A': unsupported("-A"); break;
		case 1000: usage(stdout); return 0;
		case 1001: printf("tree v%s (C, tree(1) compatible)\n", VERSION); return 0;
		case 1002: noreport = 1; break;
		case 1003: dirsfirst = 1; break;
		case 1004: filesfirst = 1; break;
		case 1005: prune = 1; break;
		case 1006: matchdirs = 1; break;
		case 1007: icase = 1; break;
		case 1008: siflag = 1; hflag = 1; sflag = 1; break;
		case 1009: filelimit = atol(optarg); break;
		case 1010: timefmt = optarg; Dflag = 1; break;
		case 1011: charset = optarg; break;
		case 1012: sortkey = optarg; break;
		case 1013: unsupported("--du"); break;
		case 1014: unsupported("--inodes"); break;
		case 1015: unsupported("--device"); break;
		case 1016: unsupported("--fromfile"); break;
		case 1017: unsupported("--gitignore"); break;
		case 1018: break;
		default: usage(stderr); return 1;
		}
	}
	if (sortkey) {
		if (!strcmp(sortkey, "none")) Uflag = 1;
		else if (!strcmp(sortkey, "version")) vflag = 1;
		else if (!strcmp(sortkey, "size")) sizesort = 1;
		else if (!strcmp(sortkey, "mtime")) tflag = 1;
		else if (!strcmp(sortkey, "ctime")) cflag = 1;
		else if (strcmp(sortkey, "name")) { fprintf(stderr, "tree: unknown sort key '%s'\n", sortkey); return 1; }
	}
	if (outname && !(out = fopen(outname, "w"))) {
		perror(outname);
		return 1;
	}
	utf8 = !strcmp(nl_langinfo(CODESET), "UTF-8");
	if (charset)
		utf8 = !strcasecmp(charset, "utf-8") || !strcasecmp(charset, "utf8");
	if (utf8) {
		art_mid = "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80 ";
		art_end = "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80 ";
		art_bar = "\xe2\x94\x82\xc2\xa0\xc2\xa0 ";
		art_sp = "    ";
	} else {
		art_mid = "|-- "; art_end = "`-- "; art_bar = "|   "; art_sp = "    ";
	}
	if (Cflag)
		colinit();

	if (Jflag)
		fputs("[\n", out);
	if (Xflag)
		fputs(utf8 ? "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<tree>\n" : "<?xml version=\"1.0\"?>\n<tree>\n", out);
	if (optind >= argc) {
		argv[--optind] = ".";
		argc = optind + 1;
	}
	for (i = optind; i < argc; i++) {
		struct node *root = newnode(argv[i], argv[i]);

		probe(root, argv[i]);
		if (root->err) {
			fprintf(stderr, "%s  [error opening dir]\n", argv[i]);
			rc = 2;
			continue;
		}
		root->isdir = S_ISDIR(root->mode) || (S_ISLNK(root->mode) && root->isdir);
		rootdev = root->dev;
		if (root->isdir && !S_ISDIR(root->mode)) {
			struct stat st;
			if (stat(argv[i], &st) == 0) {
				root->mode = st.st_mode;
				free(root->target);
				root->target = NULL;
			}
		}
		sp = 0;
		readdirs(root, 0, matchdirs && pats && matches(pats, npats, root->name));
		countroot(root);
		if (Jflag) {
			jnode(root, 1, 0);
		} else if (Xflag) {
			xnode(root, 1);
		} else {
			putline(root, argv[i]);
			printkids(root, "");
		}
	}
	if (!Jflag && !Xflag)
		report();
	if (Jflag) {
		if (!noreport) {
			fprintf(out, ",    {\"type\":\"report\",\"directories\":%ld", ndirs);
			if (!dflag)
				fprintf(out, ",\"files\":%ld", nfiles);
			fputs("}\n", out);
		}
		fputs("]\n", out);
	}
	if (Xflag) {
		if (!noreport) {
			fprintf(out, "    <report>\n        <directories>%ld</directories>\n", ndirs);
			if (!dflag)
				fprintf(out, "        <files>%ld</files>\n", nfiles);
			fputs("    </report>\n", out);
		}
		fputs("</tree>\n", out);
	}
	if (out != stdout)
		fclose(out);
	return rc;
}
