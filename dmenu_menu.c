/* dmenu_menu - the main menu, in C, on top of dmenu.
 *
 * Replaces the rofi shell script bound to Super+W: one menu with
 *   Aplikacje   desktop applications that are not games
 *   Gry         desktop applications in the Game category
 *   Skrypty     executables from your script directories
 *   Kalkulator  type an expression, the result is copied to the clipboard
 *   Schowek     cliphist history, the chosen entry goes back to the clipboard
 * Every list is shown by dmenu (DMENU_CMD, default "dmenu -i"), one item
 * per line, and the choice is read back from its stdout.
 *
 * usage: dmenu_menu              the main menu
 *        dmenu_menu apps|games|scripts|calc|clip    jump straight to one entry
 *        dmenu_menu --eval EXPR  print the value of EXPR and exit (for scripts)
 *
 * environment:
 *   DMENU_CMD      picker command (default "dmenu -i"); a prompt is added as -p
 *   DMENU_SCRIPTS  colon separated script directories
 *                  (default ~/.config/src:~/.local/bin)
 *   DMENU_TERMINAL terminal for Terminal=true apps and scripts (default foot)
 *
 * Applications come from $XDG_DATA_HOME/applications, $XDG_DATA_DIRS/applications
 * and the Flatpak export directories (also $FLATPAK_USER_DIR/exports/share).
 *
 * build: cc -O2 -Wall -Wextra -o dmenu_menu dmenu_menu.c -lm
 */
#define _GNU_SOURCE
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- small helpers ----------------------------------------------------- */
static void *
xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);
	if (!p) {
		fputs("dmenu_menu: out of memory\n", stderr);
		exit(1);
	}
	return p;
}

static char *
xstrdup(const char *s)
{
	char *p = xmalloc(strlen(s) + 1);
	return strcpy(p, s);
}

static char *
join(const char *a, const char *sep, const char *b)
{
	char *p = xmalloc(strlen(a) + strlen(sep) + strlen(b) + 1);
	sprintf(p, "%s%s%s", a, sep, b);
	return p;
}

static const char *
home(void)
{
	const char *h = getenv("HOME");
	return h && *h ? h : "/";
}

/* ---- the picker (dmenu) ------------------------------------------------ */
struct item {
	char *label;  /* what dmenu shows */
	char *data;   /* what we act on (exec line, path, ...) */
	int terminal;
};

static struct item *
additem(struct item **v, size_t *n, size_t *cap, const char *label, const char *data, int terminal)
{
	if (*n == *cap) {
		*cap = *cap ? *cap * 2 : 64;
		*v = realloc(*v, *cap * sizeof **v);
		if (!*v)
			exit(1);
	}
	(*v)[*n].label = xstrdup(label);
	(*v)[*n].data = xstrdup(data ? data : "");
	(*v)[*n].terminal = terminal;
	return &(*v)[(*n)++];
}

static int
cmpitem(const void *a, const void *b)
{
	return strcasecmp(((const struct item *)a)->label, ((const struct item *)b)->label);
}

/* Show labels in the picker and return the chosen line (malloc'd, without the
 * newline), or NULL if it was cancelled. A line the user typed that is not in
 * the list comes back as is, like dmenu itself does. */
static char *
pick(const char *prompt, char **labels, size_t n)
{
	int in[2], out[2];
	pid_t pid;
	const char *base = getenv("DMENU_CMD");
	char *cmd, *res = NULL;
	size_t i, len = 0, cap = 256;
	FILE *w;
	char *buf;
	ssize_t r;

	if (!base || !*base)
		base = "dmenu -i";
	cmd = xmalloc(strlen(base) + strlen(prompt) + 16);
	sprintf(cmd, "%s -p '%s'", base, prompt);
	if (pipe(in) || pipe(out))
		return NULL;
	if ((pid = fork()) == 0) {
		dup2(in[0], 0);
		dup2(out[1], 1);
		close(in[0]); close(in[1]); close(out[0]); close(out[1]);
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
	close(in[0]);
	close(out[1]);
	if (pid < 0)
		return NULL;
	w = fdopen(in[1], "w");
	for (i = 0; i < n; i++)
		fprintf(w, "%s\n", labels[i]);
	fclose(w);

	buf = xmalloc(cap);
	while ((r = read(out[0], buf + len, cap - len - 1)) > 0) {
		len += (size_t)r;
		if (cap - len < 2) {
			cap *= 2;
			buf = realloc(buf, cap);
			if (!buf)
				exit(1);
		}
	}
	close(out[0]);
	waitpid(pid, NULL, 0);
	buf[len] = 0;
	while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
		buf[--len] = 0;
	if (len)
		res = buf;
	else
		free(buf);
	free(cmd);
	return res;
}

static char *
pickitem(const char *prompt, struct item *v, size_t n, struct item **chosen)
{
	char **labels = xmalloc((n + 1) * sizeof *labels);
	char *r;
	size_t i;

	for (i = 0; i < n; i++)
		labels[i] = v[i].label;
	r = pick(prompt, labels, n);
	free(labels);
	*chosen = NULL;
	if (r)
		for (i = 0; i < n; i++)
			if (!strcmp(v[i].label, r)) {
				*chosen = &v[i];
				break;
			}
	return r;
}

/* ---- launching --------------------------------------------------------- */
static void
detach_run(const char *cmd, int terminal)
{
	pid_t pid = fork();

	if (pid == 0) {
		const char *term = getenv("DMENU_TERMINAL");
		if (setsid() < 0)
			_exit(1);
		if (pid == 0 && chdir(home()) < 0)
			_exit(1);
		if (terminal)
			execlp(term && *term ? term : "foot", term && *term ? term : "foot", "-e", "sh", "-c", cmd, (char *)NULL);
		else
			execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
}

/* ---- desktop entries --------------------------------------------------- */
static char *
stripcodes(const char *exec)
{
	char *out = xmalloc(strlen(exec) + 1), *o = out;
	const char *p;

	for (p = exec; *p; p++) {
		if (*p == '%' && p[1]) {
			if (p[1] == '%')
				*o++ = '%';
			p++;              /* %f %F %u %U %i %c %k ... are dropped */
			continue;
		}
		*o++ = *p;
	}
	*o = 0;
	while (o > out && o[-1] == ' ')
		*--o = 0;
	return out;
}

static int
truthy(const char *v)
{
	return v && !strcmp(v, "true");
}

static void
readdesktop(const char *path, int want_games, struct item **v, size_t *n, size_t *cap)
{
	FILE *f = fopen(path, "r");
	char line[4096], *name = NULL, *exec = NULL, *cats = NULL, *type = NULL;
	int in = 0, nodisplay = 0, hidden = 0, terminal = 0;

	if (!f)
		return;
	while (fgets(line, sizeof line, f)) {
		char *eq;
		line[strcspn(line, "\r\n")] = 0;
		if (line[0] == '[') {
			if (in)
				break;          /* only the [Desktop Entry] group */
			in = !strcmp(line, "[Desktop Entry]");
			continue;
		}
		if (!in || !(eq = strchr(line, '=')))
			continue;
		*eq = 0;
		if (!strcmp(line, "Name") && !name) name = xstrdup(eq + 1);
		else if (!strcmp(line, "Exec") && !exec) exec = xstrdup(eq + 1);
		else if (!strcmp(line, "Categories") && !cats) cats = xstrdup(eq + 1);
		else if (!strcmp(line, "Type") && !type) type = xstrdup(eq + 1);
		else if (!strcmp(line, "NoDisplay")) nodisplay = truthy(eq + 1);
		else if (!strcmp(line, "Hidden")) hidden = truthy(eq + 1);
		else if (!strcmp(line, "Terminal")) terminal = truthy(eq + 1);
	}
	fclose(f);
	if (name && exec && type && !strcmp(type, "Application") && !nodisplay && !hidden) {
		int isgame = cats && (strstr(cats, "Game;") || (strlen(cats) >= 4 && !strcmp(cats + strlen(cats) - 4, "Game")));
		if (isgame == want_games) {
			char *cmd = stripcodes(exec);
			additem(v, n, cap, name, cmd, terminal);
			free(cmd);
		}
	}
	free(name); free(exec); free(cats); free(type);
}

static void
scandir_apps(const char *dir, int want_games, struct item **v, size_t *n, size_t *cap)
{
	DIR *d = opendir(dir);
	struct dirent *e;

	if (!d)
		return;
	while ((e = readdir(d))) {
		size_t l = strlen(e->d_name);
		char *p;
		if (l < 9 || strcmp(e->d_name + l - 8, ".desktop"))
			continue;
		p = join(dir, "/", e->d_name);
		readdesktop(p, want_games, v, n, cap);
		free(p);
	}
	closedir(d);
}

static void
appdirs(void (*fn)(const char *, void *), void *arg)
{
	const char *xdh = getenv("XDG_DATA_HOME"), *xdd = getenv("XDG_DATA_DIRS"), *fu = getenv("FLATPAK_USER_DIR");
	char *dirs, *save = NULL, *t, *p;

	p = xdh && *xdh ? join(xdh, "/applications", "") : join(home(), "/.local/share/applications", "");
	fn(p, arg); free(p);
	dirs = xstrdup(xdd && *xdd ? xdd : "/usr/local/share:/usr/share");
	for (t = strtok_r(dirs, ":", &save); t; t = strtok_r(NULL, ":", &save)) {
		p = join(t, "/applications", "");
		fn(p, arg); free(p);
	}
	free(dirs);
	if (fu && *fu) {
		p = join(fu, "/exports/share/applications", "");
		fn(p, arg); free(p);
	}
	p = join(home(), "/.local/share/flatpak/exports/share/applications", "");
	fn(p, arg); free(p);
	fn("/var/lib/flatpak/exports/share/applications", arg);
}

struct appctx { int games; struct item **v; size_t *n, *cap; };

static void
appdir_cb(const char *dir, void *arg)
{
	struct appctx *c = arg;
	scandir_apps(dir, c->games, c->v, c->n, c->cap);
}

static void
menu_apps(int games)
{
	struct item *v = NULL, *sel, *w = NULL;
	size_t n = 0, cap = 0, i, m = 0;
	struct appctx c = { games, &v, &n, &cap };
	char *r;

	appdirs(appdir_cb, &c);
	qsort(v, n, sizeof *v, cmpitem);
	/* the same application can be in several directories: keep the first */
	w = xmalloc((n + 1) * sizeof *w);
	for (i = 0; i < n; i++)
		if (!m || strcmp(w[m - 1].label, v[i].label))
			w[m++] = v[i];
	r = pickitem(games ? "Gry:" : "Aplikacje:", w, m, &sel);
	if (sel)
		detach_run(sel->data, sel->terminal);
	free(r);
}

/* ---- scripts ----------------------------------------------------------- */
static void
menu_scripts(void)
{
	struct item *v = NULL, *sel;
	size_t n = 0, cap = 0;
	const char *env = getenv("DMENU_SCRIPTS");
	char *dirs, *save = NULL, *t, *r;

	if (env && *env) {
		dirs = xstrdup(env);
	} else {
		char *a = join(home(), "/.config/src", ""), *b = join(home(), "/.local/bin", "");
		dirs = join(a, ":", b);
		free(a); free(b);
	}
	for (t = strtok_r(dirs, ":", &save); t; t = strtok_r(NULL, ":", &save)) {
		DIR *d = opendir(t);
		struct dirent *e;
		if (!d)
			continue;
		while ((e = readdir(d))) {
			char *p = join(t, "/", e->d_name);
			struct stat st;
			if (e->d_name[0] != '.' && stat(p, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & S_IXUSR))
				additem(&v, &n, &cap, e->d_name, p, 1);
			free(p);
		}
		closedir(d);
	}
	free(dirs);
	qsort(v, n, sizeof *v, cmpitem);
	r = pickitem("Skrypty:", v, n, &sel);
	if (sel)
		detach_run(sel->data, sel->terminal);
	free(r);
}

/* ---- calculator -------------------------------------------------------- */
static const char *g_p;
static int g_err;

static double expr(void);

static void
skipws(void)
{
	while (*g_p == ' ' || *g_p == '\t')
		g_p++;
}

static double
primary(void)
{
	double v;
	char name[16];
	int i;

	skipws();
	if (*g_p == '(') {
		g_p++;
		v = expr();
		skipws();
		if (*g_p == ')')
			g_p++;
		else
			g_err = 1;
		return v;
	}
	if (isdigit((unsigned char)*g_p) || *g_p == '.' || *g_p == ',') {
		char buf[64];
		i = 0;
		while ((isdigit((unsigned char)*g_p) || *g_p == '.' || *g_p == ',') && i < 63)
			buf[i++] = *g_p == ',' ? '.' : *g_p, g_p++;
		buf[i] = 0;
		return atof(buf);
	}
	if (isalpha((unsigned char)*g_p)) {
		i = 0;
		while (isalpha((unsigned char)*g_p) && i < 15)
			name[i++] = (char)tolower((unsigned char)*g_p++);
		name[i] = 0;
		if (!strcmp(name, "pi")) return M_PI;
		if (!strcmp(name, "e")) return M_E;
		skipws();
		if (*g_p == '(') {
			g_p++;
			v = expr();
			skipws();
			if (*g_p == ')')
				g_p++;
			else
				g_err = 1;
			if (!strcmp(name, "sqrt")) return sqrt(v);
			if (!strcmp(name, "sin")) return sin(v);
			if (!strcmp(name, "cos")) return cos(v);
			if (!strcmp(name, "tan")) return tan(v);
			if (!strcmp(name, "ln")) return log(v);
			if (!strcmp(name, "log")) return log10(v);
			if (!strcmp(name, "exp")) return exp(v);
			if (!strcmp(name, "abs")) return fabs(v);
			if (!strcmp(name, "floor")) return floor(v);
			if (!strcmp(name, "ceil")) return ceil(v);
			if (!strcmp(name, "round")) return round(v);
		}
	}
	g_err = 1;
	return 0;
}

static double
unary(void)
{
	skipws();
	if (*g_p == '-') { g_p++; return -unary(); }
	if (*g_p == '+') { g_p++; return unary(); }
	return primary();
}

static double
power(void)
{
	double b = unary();

	skipws();
	if (*g_p == '^') {
		g_p++;
		return pow(b, power());   /* right associative */
	}
	return b;
}

static double
term(void)
{
	double v = power();

	for (;;) {
		skipws();
		if (*g_p == '*' || *g_p == 'x') { g_p++; v *= power(); }
		else if (*g_p == '/') { g_p++; v /= power(); }
		else if (*g_p == '%') { g_p++; v = fmod(v, power()); }
		else return v;
	}
}

static double
expr(void)
{
	double v = term();

	for (;;) {
		skipws();
		if (*g_p == '+') { g_p++; v += term(); }
		else if (*g_p == '-') { g_p++; v -= term(); }
		else return v;
	}
}

/* value of s, or NaN on a syntax error */
static double
evaluate(const char *s)
{
	double v;

	g_p = s;
	g_err = 0;
	v = expr();
	skipws();
	if (g_err || *g_p)
		return NAN;
	return v;
}

static void
copy_text(const char *s)
{
	FILE *w = popen("wl-copy", "w");

	if (w) {
		fputs(s, w);
		pclose(w);
	} else {
		puts(s);
	}
}

static void
menu_calc(void)
{
	char *in = pick("=", (char *[]){ NULL }, 0);

	while (in) {
		double v = evaluate(in);
		char res[64], *label[1], *choice;

		if (v != v) {
			snprintf(res, sizeof res, "syntax error");
		} else {
			snprintf(res, sizeof res, "%.12g", v);
		}
		label[0] = res;
		choice = pick(in, label, 1);
		free(in);
		if (!choice)
			return;
		if (v == v && !strcmp(choice, res)) {
			copy_text(res);
			free(choice);
			return;
		}
		in = choice;   /* edited: evaluate the new text */
	}
}

/* ---- clipboard (cliphist) ---------------------------------------------- */
static void
menu_clip(void)
{
	if (system("command -v cliphist >/dev/null 2>&1") != 0) {
		char *m[] = { "cliphist is not installed" };
		free(pick("Schowek:", m, 1));
		return;
	}
	(void)!system("cliphist list | ${DMENU_CMD:-dmenu -i} -p 'Schowek:' | cliphist decode | wl-copy");
}

/* ---- main -------------------------------------------------------------- */
static void
mainmenu(void)
{
	char *entries[] = { "Aplikacje", "Gry", "Skrypty", "Kalkulator", "Schowek" };
	char *r = pick("Menu:", entries, 5);

	if (!r)
		return;
	if (!strcmp(r, "Aplikacje")) menu_apps(0);
	else if (!strcmp(r, "Gry")) menu_apps(1);
	else if (!strcmp(r, "Skrypty")) menu_scripts();
	else if (!strcmp(r, "Kalkulator")) menu_calc();
	else if (!strcmp(r, "Schowek")) menu_clip();
	free(r);
}

/* Use the dmenu that sits next to this binary (a source-tree build that was never installed) unless
 * DMENU_CMD says otherwise; without one, fall back to whatever "dmenu" is in PATH. */
static void
default_picker(void)
{
	char exe[4096], cmd[4200];
	ssize_t n;
	char *slash;

	if (getenv("DMENU_CMD") && *getenv("DMENU_CMD"))
		return;
	n = readlink("/proc/self/exe", exe, sizeof(exe) - 8);
	if (n <= 0)
		return;
	exe[n] = '\0';
	if (!(slash = strrchr(exe, '/')))
		return;
	strcpy(slash + 1, "dmenu");
	if (access(exe, X_OK) != 0)
		return;
	snprintf(cmd, sizeof(cmd), "'%s' -i", exe);
	setenv("DMENU_CMD", cmd, 0);
}

int
main(int argc, char *argv[])
{
	default_picker();
	if (argc >= 3 && !strcmp(argv[1], "--eval")) {
		double v = evaluate(argv[2]);
		if (v != v) {
			fputs("syntax error\n", stderr);
			return 1;
		}
		printf("%.12g\n", v);
		return 0;
	}
	if (argc >= 2) {
		if (!strcmp(argv[1], "apps")) menu_apps(0);
		else if (!strcmp(argv[1], "games")) menu_apps(1);
		else if (!strcmp(argv[1], "scripts")) menu_scripts();
		else if (!strcmp(argv[1], "calc")) menu_calc();
		else if (!strcmp(argv[1], "clip")) menu_clip();
		else {
			fputs("usage: dmenu_menu [apps|games|scripts|calc|clip | --eval EXPR]\n", stderr);
			return 1;
		}
		return 0;
	}
	mainmenu();
	return 0;
}
