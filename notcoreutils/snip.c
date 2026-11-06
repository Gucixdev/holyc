/* snip - command snippet manager in the spirit of pet (knqyf263/pet), with tags.
 *
 *   snip add [-t TAGS] [-d DESC] [--] COMMAND...   store a snippet (COMMAND from stdin when absent)
 *   snip list [-t TAG]                             print the snippets
 *   snip pick [-t TAG]                             choose one, fill its <params>, print the command
 *   snip run  [-t TAG]                             the same, then run it with sh -c
 *   snip rm   [-t TAG]                             choose one and delete it
 *   snip tags                                      every tag with its snippet count
 *   snip edit                                      open the store in $EDITOR
 *   snip init bash|zsh                             shell code: a key that pastes a snippet into the line
 *
 * The store is a text file, one snippet per line: TAGS <tab> DESCRIPTION <tab> COMMAND, with \n \t \\
 * escaped. Path: $SNIP_FILE, else $XDG_CONFIG_HOME/snip/snippets, else ~/.config/snip/snippets.
 * Choosing uses fzf when one is on PATH (the sibling fzf of this directory is one), else a numbered menu.
 * A command may hold <name> or <name=default> placeholders that are asked for on the terminal.
 */
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct { char *b; size_t n, cap; } Buf;
typedef struct { char *tags, *desc, *cmd; } Snip;

static Snip *S;
static size_t nS, capS;
static char *path;

static void die(const char *m) { fprintf(stderr, "snip: %s\n", m); exit(1); }
static void *xrealloc(void *p, size_t n) { p = realloc(p, n); if (!p) die("out of memory"); return p; }
static char *xstrdup(const char *s) { char *p = strdup(s); if (!p) die("out of memory"); return p; }

static void bput(Buf *b, const char *s, size_t k)
{
	if (b->n + k + 1 > b->cap) { b->cap = (b->n + k + 1) * 2; b->b = xrealloc(b->b, b->cap); }
	if (k) memcpy(b->b + b->n, s, k);
	b->n += k;
	b->b[b->n] = 0;
}
static void bputc(Buf *b, char c) { bput(b, &c, 1); }
static char *bdone(Buf *b) { if (!b->b) return xstrdup(""); return b->b; }

static char *esc(const char *s)
{
	Buf b = {0};
	for (; *s; s++) {
		if (*s == '\\') bput(&b, "\\\\", 2);
		else if (*s == '\n') bput(&b, "\\n", 2);
		else if (*s == '\t') bput(&b, "\\t", 2);
		else bputc(&b, *s);
	}
	return bdone(&b);
}

static char *unesc(const char *s)
{
	Buf b = {0};
	for (; *s; s++) {
		if (*s == '\\' && s[1]) { s++; bputc(&b, *s == 'n' ? '\n' : *s == 't' ? '\t' : *s); }
		else bputc(&b, *s);
	}
	return bdone(&b);
}

static void push(char *tags, char *desc, char *cmd)
{
	if (nS == capS) { capS = capS ? capS * 2 : 32; S = xrealloc(S, capS * sizeof *S); }
	S[nS].tags = tags; S[nS].desc = desc; S[nS].cmd = cmd;
	nS++;
}

static void find_store(void)
{
	const char *e = getenv("SNIP_FILE"), *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
	char p[4096];
	if (e && *e) { path = xstrdup(e); return; }
	if (x && *x) snprintf(p, sizeof p, "%s/snip/snippets", x);
	else if (h && *h) snprintf(p, sizeof p, "%s/.config/snip/snippets", h);
	else die("no HOME, set SNIP_FILE");
	path = xstrdup(p);
}

static void load(void)
{
	FILE *f = fopen(path, "r");
	char *line = NULL;
	size_t lc = 0;
	ssize_t k;
	if (!f) { if (errno == ENOENT) return; die(strerror(errno)); }
	while ((k = getline(&line, &lc, f)) >= 0) {
		char *t1, *t2;
		if (k && line[k - 1] == '\n') line[--k] = 0;
		t1 = strchr(line, '\t');
		t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
		if (!t2) continue;
		*t1 = *t2 = 0;
		push(unesc(line), unesc(t1 + 1), unesc(t2 + 1));
	}
	free(line);
	fclose(f);
}

static void save(void)
{
	char tmp[4200], *s;
	FILE *f;
	size_t i;
	for (s = path + 1; *s; s++)
		if (*s == '/') { *s = 0; mkdir(path, 0755); *s = '/'; }
	snprintf(tmp, sizeof tmp, "%s.%ld.tmp", path, (long)getpid());
	f = fopen(tmp, "w");
	if (!f) die(strerror(errno));
	for (i = 0; i < nS; i++) {
		char *a = esc(S[i].tags), *b = esc(S[i].desc), *c = esc(S[i].cmd);
		fprintf(f, "%s\t%s\t%s\n", a, b, c);
		free(a); free(b); free(c);
	}
	if (fclose(f) || rename(tmp, path)) { unlink(tmp); die(strerror(errno)); }
}

/* "a b,c" -> "a,b,c": tags are comma separated, blanks count as separators too */
static char *norm_tags(const char *t)
{
	Buf b = {0};
	int sep = 1;
	for (; *t; t++) {
		if (*t == ',' || isspace((unsigned char)*t)) { if (!sep) bputc(&b, ','); sep = 1; }
		else { bputc(&b, *t); sep = 0; }
	}
	if (b.n && b.b[b.n - 1] == ',') b.b[--b.n] = 0;
	return bdone(&b);
}

static int has_tag(const char *tags, const char *tag)
{
	size_t k = strlen(tag);
	const char *p = tags;
	if (!tag || !*tag) return 1;
	while (*p) {
		const char *e = strchr(p, ',');
		size_t l = e ? (size_t)(e - p) : strlen(p);
		if (l == k && !strncmp(p, tag, k)) return 1;
		if (!e) break;
		p = e + 1;
	}
	return 0;
}

/* ---- placeholders: <name> and <name=default> ---- */

static int placeholder(const char *p, const char **name_end, const char **def, const char **def_end, const char **end)
{
	const char *q = p + 1;
	while (isalnum((unsigned char)*q) || *q == '_' || *q == '-') q++;
	if (q == p + 1) return 0;
	*name_end = q;
	if (*q == '>') { *def = *def_end = NULL; *end = q + 1; return 1; }
	if (*q != '=') return 0;
	*def = q + 1;
	for (q = *def; *q && *q != '>' && *q != '<' && *q != '\n'; q++) ;
	if (*q != '>') return 0;
	*def_end = q;
	*end = q + 1;
	return 1;
}

static char *fill(const char *cmd)
{
	struct { char *name, *val; } seen[64];
	int ns = 0, i;
	Buf out = {0};
	FILE *tty = NULL;
	const char *p = cmd;
	while (*p) {
		const char *ne, *d, *de, *end;
		if (*p == '<' && placeholder(p, &ne, &d, &de, &end)) {
			size_t nl = (size_t)(ne - (p + 1));
			const char *val = NULL;
			for (i = 0; i < ns; i++)
				if (strlen(seen[i].name) == nl && !strncmp(seen[i].name, p + 1, nl)) val = seen[i].val;
			if (!val) {
				char in[1024], *r = NULL, *dv = d ? strndup(d, (size_t)(de - d)) : xstrdup("");
				if (!tty) tty = fopen("/dev/tty", "r+");
				if (tty) {
					fprintf(tty, "%.*s%s%s%s: ", (int)nl, p + 1, *dv ? " [" : "", dv, *dv ? "]" : "");
					fflush(tty);
					r = fgets(in, sizeof in, tty);
					if (r) in[strcspn(in, "\n")] = 0;
				}
				val = (r && *in) ? xstrdup(in) : dv;
				if (ns < 64) { seen[ns].name = strndup(p + 1, nl); seen[ns].val = (char *)val; ns++; }
			}
			bput(&out, val, strlen(val));
			p = end;
		} else bputc(&out, *p++);
	}
	if (tty) fclose(tty);
	return bdone(&out);
}

/* ---- choosing ---- */

static int have_fzf(void)
{
	const char *pa = getenv("PATH");
	char *c, *d, *s;
	if (!pa) return 0;
	c = d = xstrdup(pa);
	for (s = strtok(d, ":"); s; s = strtok(NULL, ":")) {
		char f[4200];
		snprintf(f, sizeof f, "%s/fzf", s);
		if (access(f, X_OK) == 0) { free(c); return 1; }
	}
	free(c);
	return 0;
}

static char *oneline(const Snip *s)
{
	Buf b = {0};
	const char *p;
	if (*s->tags) { bputc(&b, '['); bput(&b, s->tags, strlen(s->tags)); bput(&b, "] ", 2); }
	if (*s->desc) { bput(&b, s->desc, strlen(s->desc)); bput(&b, "  ::  ", 6); }
	for (p = s->cmd; *p; p++) bputc(&b, (*p == '\n' || *p == '\t') ? ' ' : *p);
	return bdone(&b);
}

/* index of the chosen snippet among those carrying tag, or -1 (nothing there, or cancelled) */
static long pick(const char *tag)
{
	size_t i, shown = 0;
	long res = -1;
	for (i = 0; i < nS; i++) shown += has_tag(S[i].tags, tag);
	if (!shown) { fprintf(stderr, "snip: no snippets%s%s\n", tag && *tag ? " tagged " : "", tag ? tag : ""); return -1; }

	if (have_fzf()) {
		const char *td = getenv("TMPDIR");
		char tmp[4200], cmd[4400], ln[64];
		int fd;
		FILE *f, *pp;
		if (!td || !*td || strchr(td, '\'')) td = "/tmp";
		snprintf(tmp, sizeof tmp, "%s/snip.XXXXXX", td);
		fd = mkstemp(tmp);
		if (fd < 0) die("cannot make a temp file");
		f = fdopen(fd, "w");
		for (i = 0; i < nS; i++)
			if (has_tag(S[i].tags, tag)) { char *o = oneline(&S[i]); fprintf(f, "%zu\t%s\n", i, o); free(o); }
		fclose(f);
		snprintf(cmd, sizeof cmd, "fzf --delimiter='\t' --with-nth=2 --reverse --height=40%% --prompt='snip> ' < '%s'", tmp);
		pp = popen(cmd, "r");
		if (pp) {
			if (fgets(ln, sizeof ln, pp)) res = strtol(ln, NULL, 10);
			if (pclose(pp) != 0) res = -1;
		}
		unlink(tmp);
	} else {
		FILE *tty = fopen("/dev/tty", "r+");
		char in[64];
		long c;
		if (!tty) die("no fzf and no terminal to ask on");
		for (i = 0; i < nS; i++)
			if (has_tag(S[i].tags, tag)) { char *o = oneline(&S[i]); fprintf(tty, "%3zu  %s\n", i + 1, o); free(o); }
		fprintf(tty, "snip> ");
		fflush(tty);
		if (fgets(in, sizeof in, tty)) {
			c = strtol(in, NULL, 10);
			if (c >= 1 && (size_t)c <= nS && has_tag(S[c - 1].tags, tag)) res = c - 1;
		}
		fclose(tty);
	}
	return res;
}

/* ---- commands ---- */

static void usage(FILE *o)
{
	fputs("usage: snip add [-t TAGS] [-d DESC] [--] COMMAND...\n"
	      "       snip list [-t TAG]    snip pick [-t TAG]    snip run [-t TAG]    snip rm [-t TAG]\n"
	      "       snip tags             snip edit             snip init bash|zsh\n", o);
}

static const char *tag_arg(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[0], "-t")) return argv[1];
	if (argc == 0) return "";
	usage(stderr);
	exit(2);
}

static int cmd_add(int argc, char **argv)
{
	const char *tags = "", *desc = "";
	int i = 0;
	Buf cmd = {0};
	size_t k;
	for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
		if (!strcmp(argv[i], "--")) { i++; break; }
		if (!strcmp(argv[i], "-t") && i + 1 < argc) tags = argv[++i];
		else if (!strcmp(argv[i], "-d") && i + 1 < argc) desc = argv[++i];
		else { usage(stderr); return 2; }
	}
	for (; i < argc; i++) { if (cmd.n) bputc(&cmd, ' '); bput(&cmd, argv[i], strlen(argv[i])); }
	if (!cmd.n && !isatty(0)) {
		char chunk[4096];
		while ((k = fread(chunk, 1, sizeof chunk, stdin)) > 0) bput(&cmd, chunk, k);
	}
	while (cmd.n && (cmd.b[cmd.n - 1] == '\n' || cmd.b[cmd.n - 1] == ' ')) cmd.b[--cmd.n] = 0;
	if (!cmd.n) { fputs("snip: add: no command given\n", stderr); return 2; }
	for (k = 0; k < nS; k++)
		if (!strcmp(S[k].cmd, cmd.b)) { fputs("snip: already stored\n", stderr); return 1; }
	push(norm_tags(tags), xstrdup(desc), cmd.b);
	save();
	return 0;
}

static int cmd_list(const char *tag)
{
	size_t i;
	for (i = 0; i < nS; i++) {
		if (!has_tag(S[i].tags, tag)) continue;
		printf("%3zu  %s%s%s%s%s\n      %s\n", i + 1, *S[i].tags ? "[" : "", S[i].tags, *S[i].tags ? "]" : "",
		       *S[i].tags && *S[i].desc ? "  " : "", S[i].desc, S[i].cmd);
	}
	return 0;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static int cmd_tags(void)
{
	char **all = NULL;
	size_t n = 0, i, j;
	for (i = 0; i < nS; i++) {
		char *t = xstrdup(S[i].tags), *s;
		for (s = strtok(t, ","); s; s = strtok(NULL, ",")) {
			all = xrealloc(all, (n + 1) * sizeof *all);
			all[n++] = xstrdup(s);
		}
		free(t);
	}
	qsort(all, n, sizeof *all, cmp_str);
	for (i = 0; i < n; i = j) {
		for (j = i; j < n && !strcmp(all[i], all[j]); j++) ;
		printf("%-20s %zu\n", all[i], j - i);
	}
	return 0;
}

static int cmd_init(const char *sh)
{
	if (!strcmp(sh, "bash"))
		puts("# paste a snippet into the command line with Ctrl-X s\n"
		     "snip-widget() {\n"
		     "\tlocal s\n"
		     "\ts=$(snip pick) || return\n"
		     "\tREADLINE_LINE=\"${READLINE_LINE:0:READLINE_POINT}${s}${READLINE_LINE:READLINE_POINT}\"\n"
		     "\tREADLINE_POINT=$((READLINE_POINT + ${#s}))\n"
		     "}\n"
		     "bind -x '\"\\C-xs\": snip-widget'");
	else if (!strcmp(sh, "zsh"))
		puts("# paste a snippet into the command line with Ctrl-X s\n"
		     "snip-widget() {\n"
		     "\tlocal s\n"
		     "\ts=$(snip pick </dev/tty) && LBUFFER+=$s\n"
		     "\tzle reset-prompt\n"
		     "}\n"
		     "zle -N snip-widget\n"
		     "bindkey '^Xs' snip-widget");
	else { usage(stderr); return 2; }
	return 0;
}

int main(int argc, char **argv)
{
	const char *c;
	long i;
	if (argc < 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) { usage(argc < 2 ? stderr : stdout); return argc < 2 ? 2 : 0; }
	c = argv[1];
	if (!strcmp(c, "init")) return argc == 3 ? cmd_init(argv[2]) : (usage(stderr), 2);
	find_store();
	load();
	if (!strcmp(c, "add")) return cmd_add(argc - 2, argv + 2);
	if (!strcmp(c, "list")) return cmd_list(tag_arg(argc - 2, argv + 2));
	if (!strcmp(c, "tags")) return cmd_tags();
	if (!strcmp(c, "edit")) {
		const char *ed = getenv("EDITOR");
		execlp(ed && *ed ? ed : "vi", ed && *ed ? ed : "vi", path, (char *)NULL);
		die(strerror(errno));
	}
	if (!strcmp(c, "pick") || !strcmp(c, "run") || !strcmp(c, "rm")) {
		const char *tag = tag_arg(argc - 2, argv + 2);
		i = pick(tag);
		if (i < 0) return 1;
		if (!strcmp(c, "rm")) {
			memmove(&S[i], &S[i + 1], (nS - (size_t)i - 1) * sizeof *S);
			nS--;
			save();
			return 0;
		}
		{
			char *cmd = fill(S[i].cmd);
			if (!strcmp(c, "pick")) { puts(cmd); return 0; }
			execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
			die(strerror(errno));
		}
	}
	usage(stderr);
	return 2;
}
