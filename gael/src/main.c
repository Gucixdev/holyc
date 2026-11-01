/* gael - a small statically typed language with s-expressions, compiled to bytecode and run on a VM.
 *
 *   gael                   the REPL
 *   gael FILE ARGS...      compile FILE; when there are no errors, run it; (args) gives it ARGS (also gael run)
 *   gael check FILE        only compile: report every error and warning, run nothing (also gael -c)
 *   gael test FILE...      run the files' defs, then each (@test "name" ...); a gem's main file runs that gem's tests
 *   gael doc               the cheat sheet for models, docs/MODELS.md next to the binary
 *   gael -d FILE           compile and print the bytecode
 *   gael -m FILE           print FILE with its macros expanded
 *   gael -e CODE ARGS...   compile and run CODE
 *   gael -x FILE           compile FILE and list what it exports (@export), the way an editor's inspector shows it
 *   gael -n                the built-in names, one a line: forms, operators, functions and types (docs/REFERENCE.md
 *                          has each one)
 *   gael -h                this help
 *
 * gael's own folder is $GAEL_HOME or ~/.gael (like ~/.emacs.d). The REPL first runs main.gael from there when there is
 * one (like init.el), then appends every form that ran without an error to repldump.gael there: a session starts with
 * a line that says when and in which directory, and each form comes after a line with its date and time and the
 * functions of files it gave new bodies. The files themselves are never written: what changed lives in the running
 * program and in the dump. GAEL_HOME= (empty) turns both off.
 *
 * A file can use others: (use "lib.gael") at its top level, the path relative to the file. Every file is compiled
 * once, the used ones first, and the program runs only when none of them has an error.
 *
 * Gems are files by name: (enable name...) loads gems/NAME.gael or gems/NAME/main.gael, from the program's folder,
 * else from gael's own, else from the gems that come with gael (gems/ next to the binary), each after the gems its
 * manifest requires; a gem's main file starts with the manifest,
 * (gem name :requires (...) :provides (...) :excludes (...)). Only the gems named are read.
 *
 * Exit status: 0 fine, 1 a compile or runtime error, 2 bad usage; (exit n) ends with n.
 * One translation unit (this file includes the others), C89, no malloc and no recursion. */

#include "base.h"
#include <errno.h>
#include <sys/stat.h>
#include <time.h>
#include "ops.h"
#include "read.c"
#include "names.c"
#include "macro.c"
#include "compile.c"
#include "kernel.c"
#include "vm.c"
#include "gpu.c"
#include "wayland.c"
#include "gfx.c"

static const char *const g_usage[] = {
	"usage: gael                   the REPL\n",
	"       gael FILE ARGS...      compile FILE; when there are no errors, run it; (args) gives it ARGS\n",
	"       gael run FILE ARGS...  the same\n",
	"       gael check FILE        only compile: report every error and warning, run nothing (also gael -c)\n",
	"       gael test FILE...      compile the files, run their defs (not the rest of their top level), then each\n",
	"                              (@test \"name\" ...) in them: ok or FAIL for each; exit 1 when one failed. A\n",
	"                              gem's main file (gems/NAME.gael) runs that gem's own tests\n",
	"       gael doc               the cheat sheet for writing gael (docs/MODELS.md next to the binary)\n",
	"       gael -d FILE           compile and print the bytecode\n",
	"       gael -m FILE           print FILE with its macros expanded: the code the compiler checks\n",
	"       gael -g FILE           print the WGSL that each gpu-each in FILE runs on the GPU\n",
	"       gael -e CODE ARGS...   compile and run CODE\n",
	"       gael -x FILE           compile FILE and list its @export globals (name : type = value) and its\n",
	"                              (@warn ...) and (@err ...) marks, the way an editor shows them\n",
	"       gael -n                the built-in names, one a line (docs/REFERENCE.md explains each)\n",
	"       gael -h                this help\n",
	0
};
static const char *const g_usage_env[] = { /* one string each: C89 promises only 509 bytes in one */
	"GAEL_HEAP=SIZE  the heap for strings, vecs, maps and pools, and as much again for arenas and for the program's\n",
	"                own text (default 1G; K, M, G; the system gives pages only where they are written)\n",
	"GAEL_CHECK=1    freed memory is overwritten, and a program that ran to its end reports what it never freed\n",
	"GAEL_HOME=DIR   gael's own folder (default ~/.gael): the REPL runs main.gael from there first and appends\n",
	"                what ran to repldump.gael; its gems/ comes after the program's; set to nothing, none of it\n",
	"GAEL_GPU=0|1    0: every gpu-each runs on the VM; 1: a missing GPU is an error (default: the GPU if there is one)\n",
	"GAEL_WGPU=FILE  the wgpu-native library for gpu-each (default: lib/libwgpu_native.so next to gael, the system's)\n",
	0
};

enum { RUN, CHECK, DIS, EXPAND };

/* Every file a program uses, once, by its canonical path */
#define MAX_MODULES (1 << 14)
typedef struct {
	const char *real;  /* the canonical path: the same file reached two ways is one module */
	const char *shown; /* the path as messages show it */
	u8 state;          /* 0 not loaded, 1 on the load stack, 2 compiled */
	i32 gem;           /* the gem it belongs to, 0 for the program */
} Module;
static Module g_mods[MAX_MODULES];
static i32 g_nmods;

/* a source on the load stack: a file (src 0, read again from disk each time it comes to the top), or the text of
 * a REPL form or of -e */
typedef struct {
	const char *name, *src;
	size_t len;
	i32 mod;
	i32 gem;  /* the gem the file belongs to, 0 for the program */
	u8 main;  /* it is that gem's main file, which starts with the manifest */
} Source;
#define MAX_USE_DEPTH 256
static Source g_load[MAX_USE_DEPTH];
static i32 g_entries[MAX_MODULES + 1];

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* module_of PATH: the module for a file, made on first sight; -1 when the file is not there */
static i32 module_of(const char *path)
{
	char real[PATH_MAX];
	i32 i;
	if (!realpath(path, real)) return -1;
	for (i = 0; i < g_nmods; i++) {
		if (!strcmp(g_mods[i].real, real)) return i;
	}
	if (g_nmods == MAX_MODULES) die("too many files (MAX_MODULES)");
	g_mods[g_nmods].real = arena_strndup(&g_perm, real, strlen(real));
	g_mods[g_nmods].shown = arena_strndup(&g_perm, path, strlen(path));
	g_mods[g_nmods].state = 0;
	g_mods[g_nmods].gem = -1;
	return g_nmods++;
}

/* read_source SOURCE: its text into g_unit, then into nodes; 0 after an error it reported */
static int read_source(const Source *so)
{
	g_unit.file = so->name;
	g_unit.nerrors = g_unit.nwarnings = 0;
	g_unit_arena.used = 0;
	g_nnodes = 0;
	if (so->src) {
		g_unit.src = so->src;
		g_unit.len = so->len;
	} else {
		FILE *f = fopen(so->name, "rb");
		long size = -1;
		char *buf;
		if (f && !fseek(f, 0, SEEK_END)) size = ftell(f);
		if (!f || size < 0 || fseek(f, 0, SEEK_SET)) {
			if (f) fclose(f);
			fprintf(stderr, "gael: cannot read %s\n", so->name);
			return 0;
		}
		buf = (char *)arena_alloc(&g_unit_arena, (size_t)size + 1);
		if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
			fclose(f);
			fprintf(stderr, "gael: cannot read %s\n", so->name);
			return 0;
		}
		fclose(f);
		buf[size] = 0;
		g_unit.src = buf;
		g_unit.len = (size_t)size;
	}
	return 1;
}

/* next_use TOP FROM AT: the first (use "path") at the top level whose file is not compiled yet, as a module; -1 when
 * there is none, -2 after an error it reported. AT gets the path's node. The path is relative to FROM's folder */
static i32 next_use(i32 top, const char *from, i32 gem, i32 *at)
{
	const char *slash = strrchr(from, '/');
	char path[PATH_MAX];
	i32 n, m;
	for (n = top; n >= 0; n = next_of(n)) {
		i32 arg = next_of(first_of(n));
		size_t dir = slash ? (size_t)(slash - from) + 1 : 0;
		if (!head_is(n, "use") || arg < 0 || g_nodes[arg].kind != N_STR || next_of(arg) >= 0) continue;
		if (g_nodes[arg].text[0] == '/') dir = 0;
		if (dir + g_nodes[arg].len + 1 > sizeof(path)) {
			error_at(g_nodes[arg].line, g_nodes[arg].col, "use: the path is too long");
			return -2;
		}
		memcpy(path, from, dir);
		memcpy(path + dir, g_nodes[arg].text, g_nodes[arg].len + 1);
		if ((m = module_of(path)) < 0) {
			error_at(g_nodes[arg].line, g_nodes[arg].col, "use: there is no file %s", path);
			return -2;
		}
		if (g_mods[m].gem >= 0 && g_mods[m].gem != gem) {
			error_at(g_nodes[arg].line, g_nodes[arg].col, "use: %s belongs to %s%s; a gem's files are its own",
			         g_mods[m].shown, g_mods[m].gem ? "gem " : "", g_gems[g_mods[m].gem].name);
			return -2;
		}
		if (g_mods[m].state == 2) continue;
		*at = arg;
		return m;
	}
	return -1;
}

/* load_failed: forget the files that were on the load stack, so the REPL can try them again */
static int load_failed(void)
{
	i32 i;
	for (i = 0; i < g_nmods; i++) {
		if (g_mods[i].state == 1) g_mods[i].state = 0;
	}
	for (i = 1; i < g_ngems; i++) {
		if (g_gems[i].state == GEM_LOADING) g_gems[i].state = GEM_KNOWN;
	}
	g_unit_gem = 0;
	g_unit_main = 0;
	return 1;
}

static char g_gem_root[PATH_MAX]; /* the folder of the program being loaded: its gems/ is looked at first */
static char g_gem_dir[PATH_MAX]; /* gael test on a gem's file: the folder that holds it, looked in before the others */
static int gael_home(char *path);

/* exe_dir PATH: the folder of the running gael binary, into PATH (PATH_MAX bytes): the gems that come with gael are in
 * its gems/. 0 when the system cannot tell (Linux's /proc/self/exe) */
static int exe_dir(char *path)
{
	ssize_t n = readlink("/proc/self/exe", path, PATH_MAX - 1);
	char *slash;
	if (n <= 0) return 0;
	path[n] = 0;
	if (!(slash = strrchr(path, '/'))) return 0;
	*slash = 0;
	return 1;
}

/* gem_path NAME OUT: the main file of gem NAME into OUT (PATH_MAX bytes) - gems/NAME.gael or gems/NAME/main.gael in
 * the program's folder, then in gael's own folder, then next to the gael binary (the gems that come with it); 0 when
 * there is none */
static int gem_path(const char *name, char *out)
{
	static char exe[PATH_MAX];
	char home[1024];
	const char *dirs[3];
	i32 nd = 0, i;
	const char *c;
	for (c = name; *c; c++) { /* a gem's name is its file's: letters, digits, - and _ */
		if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '-' || *c == '_')) return 0;
	}
	if (g_gem_dir[0] && strlen(g_gem_dir) + 2 * strlen(name) + 24 <= PATH_MAX) { /* the tested gem's neighbours */
		sprintf(out, "%s/%s.gael", g_gem_dir, name);
		if (!access(out, R_OK)) return 1;
		sprintf(out, "%s/%s/main.gael", g_gem_dir, name);
		if (!access(out, R_OK)) return 1;
	}
	dirs[nd++] = g_gem_root;
	if (gael_home(home)) dirs[nd++] = home;
	if (exe_dir(exe)) dirs[nd++] = exe;
	for (i = 0; i < nd; i++) {
		if (strlen(dirs[i]) + 2 * strlen(name) + 24 > PATH_MAX) continue;
		sprintf(out, "%s/gems/%s.gael", dirs[i], name);
		if (!access(out, R_OK)) return 1;
		sprintf(out, "%s/gems/%s/main.gael", dirs[i], name);
		if (!access(out, R_OK)) return 1;
	}
	return 0;
}

/* gem_register NAME PATH: gem NAME's manifest, read from its main file PATH, into the gem table; its index, or -2
 * after an error it reported (in that file) */
static i32 gem_register(const char *name, const char *path)
{
	Source so;
	Gem g;
	i32 top;
	if (g_ngems == MAX_GEMS) die("too many gems (MAX_GEMS)");
	memset(&so, 0, sizeof(so));
	so.name = arena_strndup(&g_perm, path, strlen(path));
	so.mod = -1;
	if (!read_source(&so)) return -2;
	top = read_unit();
	if (g_unit.nerrors) return -2;
	if (top < 0) {
		error_at(1, 1, "a gem's main file starts with its manifest: (gem %.64s :requires (...))", name);
		return -2;
	}
	if (!gem_manifest(top, name, &g)) return -2;
	g.tested = 0;
	g.file = so.name;
	g.state = GEM_KNOWN;
	g_gems[g_ngems] = g;
	return g_ngems++;
}

/* next_gem TOP SO: the gems that must be loaded before the source SO (just read; TOP is its first form) compiles -
 * those its (enable ...) forms name when it is the program, those its manifest requires when it is a gem's main
 * file. Returns a gem to load now, -1 when they are all there, -3 when a manifest was read (so SO must be read again),
 * -2 after an error it reported */
static i32 next_gem(i32 top, const Source *so)
{
	static char names[MAX_GEMS][64];
	static i32 pos[MAX_GEMS][2];
	char path[PATH_MAX];
	i32 n = 0, i, g, x, k;
	const char *why;
	if (so->main) { /* a gem: each thing it requires comes from a gem on, or one known that provides it, or the gem of that name */
		const Gem *me = &g_gems[so->gem];
		i32 line = g_nodes[top].line, col = g_nodes[top].col;
		for (i = 0; i < me->nrequires; i++) {
			const char *what = me->requires[i];
			i32 known = -1, nknown = 0;
			for (g = 1; g < g_ngems; g++) {
				if (g == so->gem || !gem_gives(g, what)) continue;
				if (g_gems[g].state >= GEM_LOADING) break;
				known = g;
				nknown++;
			}
			if (g < g_ngems) { /* there already */
				if (g_gems[g].state == GEM_LOADING) {
					error_at(line, col, "gem %.64s requires %.64s, which needs %.64s first: they require each other", me->name, what, me->name);
					return -2;
				}
				if (g_gems[g].state == GEM_OFF) {
					error_at(line, col, "gem %.64s requires %.64s, and gem %.64s is off; (enable %.64s) first", me->name, what, g_gems[g].name, g_gems[g].name);
					return -2;
				}
				continue;
			}
			if (nknown > 1) {
				error_at(line, col, "gem %.64s requires %.64s, which more than one gem provides; enable the one to use", me->name, what);
				return -2;
			}
			if (nknown == 1) {
				if ((why = gem_clash(known)) != 0) { error_at(line, col, "%s", why); return -2; }
				return known;
			}
			if (!gem_path(what, path)) {
				error_at(line, col, "gem %.64s requires %.64s, and no gem provides it: none is on, and there is no gems/%.64s.gael or gems/%.64s/main.gael",
				         me->name, what, what, what);
				return -2;
			}
			return gem_register(what, path) < 0 ? -2 : -3;
		}
		return -1;
	}
	if (so->gem) return -1; /* another file of a gem */
	for (x = top; x >= 0; x = next_of(x)) { /* the program: the names in its (enable ...) forms */
		if (!head_is(x, "enable")) continue;
		for (k = next_of(first_of(x)); k >= 0; k = next_of(k)) {
			if (!is_sym(k) || g_nodes[k].len >= sizeof(names[0]) || n == MAX_GEMS) continue; /* compile_unit says what is wrong */
			memcpy(names[n], g_nodes[k].text, g_nodes[k].len + 1);
			pos[n][0] = g_nodes[k].line;
			pos[n][1] = g_nodes[k].col;
			n++;
		}
	}
	for (i = 0; i < n; i++) { /* every manifest first, so a requirement can be met by a gem named later in the list */
		if (gem_find(names[i]) >= 0) continue;
		if (!gem_path(names[i], path)) {
			error_at(pos[i][0], pos[i][1], "there is no gem %.64s: no gems/%.64s.gael or gems/%.64s/main.gael here, in gael's folder or with gael itself",
			         names[i], names[i], names[i]);
			return -2;
		}
		return gem_register(names[i], path) < 0 ? -2 : -3;
	}
	for (i = 0; i < n; i++) {
		g = gem_find(names[i]);
		if (g_gems[g].state != GEM_KNOWN) continue;
		if ((why = gem_clash(g)) != 0) {
			error_at(pos[i][0], pos[i][1], "%s", why);
			return -2;
		}
		return g;
	}
	return -1;
}

/* load NAME SRC LEN REPL MODE: compile NAME (SRC 0: read the file) and, before it, every file it uses and they use.
 * Then, by MODE, run or disassemble them in that order. A program runs only when no file has an error; in the REPL
 * a file runs as soon as it is compiled, like the forms typed after it. */
static int load(const char *name, const char *src, size_t len, int repl, int mode)
{
	i32 depth = 1, nentries = 0, i, top, entry, m, at = -1;
	int st;
	g_load[0].name = name;
	g_load[0].src = src;
	g_load[0].len = len;
	g_load[0].mod = -1;
	g_load[0].gem = 0;
	g_load[0].main = 0;
	strcpy(g_gem_root, ".");
	if (!src && strlen(name) < sizeof(g_gem_root) && strrchr(name, '/')) {
		strcpy(g_gem_root, name);
		*strrchr(g_gem_root, '/') = 0;
	}
	if (!src) {
		if ((m = module_of(name)) < 0) {
			fprintf(stderr, "gael: cannot open %s\n", name);
			return 1;
		}
		g_mods[m].state = 1;
		g_load[0].mod = m;
	}
	while (depth > 0) {
		Source *so = &g_load[depth - 1];
		if (!read_source(so)) return load_failed();
		top = read_unit();
		if (g_unit.nerrors) return load_failed();
		if ((m = next_gem(top, so)) == -2) return load_failed();
		if (m == -3) continue;
		if (m >= 0) { /* a gem to load first, from its main file */
			i32 g = m;
			if (depth == MAX_USE_DEPTH) {
				error_at(g_nodes[top].line, g_nodes[top].col, "gems nested deeper than %d", MAX_USE_DEPTH);
				return load_failed();
			}
			if ((m = module_of(g_gems[g].file)) < 0) {
				fprintf(stderr, "gael: cannot open %s\n", g_gems[g].file);
				return load_failed();
			}
			g_gems[g].state = GEM_LOADING;
			g_mods[m].state = 1;
			g_mods[m].gem = g;
			g_load[depth].name = g_mods[m].shown;
			g_load[depth].src = 0;
			g_load[depth].len = 0;
			g_load[depth].mod = m;
			g_load[depth].gem = g;
			g_load[depth].main = 1;
			depth++;
			continue;
		}
		m = next_use(top, so->name, so->gem, &at);
		if (m == -2) return load_failed();
		if (m >= 0) {
			if (g_mods[m].state == 1) {
				error_at(g_nodes[at].line, g_nodes[at].col, "use: %s uses this file, directly or through others", g_mods[m].shown);
				return load_failed();
			}
			if (depth == MAX_USE_DEPTH) {
				error_at(g_nodes[at].line, g_nodes[at].col, "use: files nested deeper than %d", MAX_USE_DEPTH);
				return load_failed();
			}
			g_mods[m].state = 1;
			g_mods[m].gem = so->gem;
			g_load[depth].name = g_mods[m].shown;
			g_load[depth].src = 0;
			g_load[depth].len = 0;
			g_load[depth].mod = m;
			g_load[depth].gem = so->gem;
			g_load[depth].main = 0;
			depth++;
			continue;
		}
		g_unit_gem = so->gem;
		g_unit_main = so->main;
		g_show_expanded = mode == EXPAND && depth == 1; /* gael -m: the file it was given, not those it uses */
		entry = compile_unit(top, repl && depth == 1);
		if (entry < 0) return load_failed();
		g_unit_gem = 0;
		g_unit_main = 0;
		if (so->main) g_gems[so->gem].state = GEM_ON;
		if (so->mod >= 0) g_mods[so->mod].state = 2;
		if (mode == DIS) {
			if (depth > 1) printf("; %s\n", so->name);
			disassemble(entry, g_ncode);
		} else if (mode == RUN && repl) {
			if ((st = vm_run(entry)) != 0) return st;
		} else {
			g_entries[nentries++] = entry;
		}
		depth--;
	}
	for (i = 0; mode == RUN && i < nentries; i++) {
		if ((st = vm_run(g_entries[i])) != 0) return st;
	}
	return 0;
}

/* balance TEXT: how many '(' are still open, outside strings and comments; the REPL reads more lines while > 0 */
static long balance(const char *s)
{
	long depth = 0;
	int in_str = 0;
	for (; *s; s++) {
		if (in_str) {
			if (*s == '\\' && s[1]) s++;
			else if (*s == '"') in_str = 0;
		} else if (*s == '"') {
			in_str = 1;
		} else if (*s == ';') {
			while (s[1] && s[1] != '\n') s++;
		} else if (*s == '(') {
			depth++;
		} else if (*s == ')') {
			depth--;
		}
	}
	return in_str ? 1 : depth;
}

/* has_form TEXT: the text holds more than spaces and comments - something the dump keeps */
static int has_form(const char *s)
{
	for (; *s; s++) {
		if (*s == ';') {
			while (s[1] && s[1] != '\n') s++;
		} else if (*s != ' ' && *s != '\t' && *s != '\n' && *s != '\r') {
			return 1;
		}
	}
	return 0;
}

/* leaks: with GAEL_CHECK=1, after a program ran to its end its globals are freed too, and then nothing may be left on
 * the heap. What is left was kept alive by a cycle (a vec, map or pool that holds itself) or lost by a counting bug */
static int leaks(void)
{
	static const char *const kinds[] = { "?", "string", "vec", "map", "pool", "coroutine", "closure" };
	int k, any = 0;
	if (!g_check || !leak_check()) return 0;
	fflush(stdout);
	fputs("gael: GAEL_CHECK: never freed:", stderr);
	for (k = 1; k < 7; k++) {
		if (!g_objects[k]) continue;
		fprintf(stderr, "%s %ld %s%s", any ? "," : "", (long)g_objects[k], kinds[k], g_objects[k] == 1 ? "" : "s");
		any = 1;
	}
	if (!any) fprintf(stderr, " %ld cells", (long)g_live);
	fputc('\n', stderr);
	return 1;
}

static char g_line_buf[1 << 16];

/* gael_home PATH: gael's own folder - $GAEL_HOME, or ~/.gael - into PATH (1024 bytes), made when it is not there;
 * 0 when there is none: GAEL_HOME set to "", or no HOME */
static int gael_home(char *path)
{
	const char *home = getenv("GAEL_HOME"), *h = getenv("HOME");
	if (home && !*home) return 0;
	if (!home && (!h || !*h)) return 0;
	if (strlen(home ? home : h) > 900) return 0;
	if (home) strcpy(path, home);
	else sprintf(path, "%s/.gael", h);
	mkdir(path, 0755); /* there already, or it cannot be made: opening a file in it says which */
	return 1;
}

static FILE *g_dump; /* repldump.gael, opened for appending when the REPL starts */
static int g_dumped; /* the session's first line is written */

/* dump FORM: append a form that ran without an error, after a line with its date and time and the functions of files
 * it replaced (";; 2026-10-25 16:26:03, replaces hurt (game.gael:12)"); the session's first form comes after a
 * line that says when and where the session ran */
static o dump(const char *form)
{
	char stamp[64], dir[900];
	time_t now = time(0);
	size_t n = strlen(form);
	i32 k;
	if (!g_dump) return;
	strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", localtime(&now));
	if (!g_dumped) {
		if (!getcwd(dir, sizeof(dir))) strcpy(dir, "?");
		fseek(g_dump, 0, SEEK_END);
		fprintf(g_dump, "%s; REPL session, %s, in %s\n", ftell(g_dump) > 0 ? "\n" : "", stamp, dir);
		g_dumped = 1;
	}
	fprintf(g_dump, ";; %s", stamp);
	for (k = 0; k < g_nreplaced; k++)
		fprintf(g_dump, "%s %.64s (%.300s:%ld)", k ? "," : ", replaces", g_replaced[k].name, g_replaced[k].file, (long)g_replaced[k].line);
	fputc('\n', g_dump);
	fputs(form, g_dump);
	if (n && form[n - 1] != '\n') fputc('\n', g_dump);
	fflush(g_dump); /* on disk now, even if the session is killed */
}

static int repl(void)
{
	size_t used = 0;
	int interactive = 0;
	char home[1024], path[1100];
	FILE *tty = fopen("/dev/tty", "r"), *f;
	if (tty) { interactive = 1; fclose(tty); } /* a prompt only where someone types */
	if (gael_home(home)) {
		sprintf(path, "%s/main.gael", home);
		if ((f = fopen(path, "r")) != 0) { /* its definitions are there for the session; an error in it is reported */
			fclose(f);
			(void)load(path, 0, 0, 1, RUN);
		}
		sprintf(path, "%s/repldump.gael", home);
		if (!(g_dump = fopen(path, "a"))) fprintf(stderr, "gael: the REPL is not saved: %s: %s\n", path, strerror(errno));
	}
	if (interactive) puts("gael 0.3 - :q quits, a form may span lines");
	for (;;) {
		if (interactive) {
			fputs(used ? "  ... " : "gael> ", stdout);
			fflush(stdout);
		}
		if (!fgets(g_line_buf + used, (int)(sizeof(g_line_buf) - used), stdin)) {
			if (interactive) putchar('\n');
			return 0;
		}
		if (!used && (!strcmp(g_line_buf, ":q\n") || !strcmp(g_line_buf, ":q"))) return 0;
		used = strlen(g_line_buf);
		if (used + 1 >= sizeof(g_line_buf)) {
			fputs("gael: input too long\n", stderr);
			used = 0;
			continue;
		}
		if (balance(g_line_buf) > 0) continue;
		if (!load("<repl>", g_line_buf, used, 1, RUN) && has_form(g_line_buf)) dump(g_line_buf);
		fflush(stdout);
		used = 0;
	}
}

/* gem_file PATH NAME: 1 when the first form of the file PATH is a (gem ...) manifest, with the gem's name - the file's,
 * gems/NAME.gael, or its folder's, gems/NAME/main.gael - in NAME (64 bytes) */
static int gem_file(const char *path, char *name)
{
	const char *base = strrchr(path, '/') ? strrchr(path, '/') + 1 : path, *from = base;
	char head[4];
	size_t len;
	FILE *f;
	int c, n;
	if (!(f = fopen(path, "r"))) return 0;
	do { /* past blank space and comments to the first form */
		c = getc(f);
		if (c == ';') while (c != '\n' && c != EOF) c = getc(f);
	} while (c == ' ' || c == '\t' || c == '\n' || c == '\r');
	n = 0;
	if (c == '(') for (; n < 4 && (c = getc(f)) != EOF; n++) head[n] = (char)c;
	fclose(f);
	if (n < 4 || memcmp(head, "gem", 3) || !strchr(" \t\r\n)", head[3])) return 0;
	len = strlen(base);
	if (!strcmp(base, "main.gael")) { /* a gem in a folder: the folder's name */
		if (base == path) return 0;
		for (from = base - 1; from > path && from[-1] != '/'; from--) {}
		len = (size_t)(base - 1 - from);
	} else if (len > 5 && !strcmp(base + len - 5, ".gael")) len -= 5;
	if (len == 0 || len >= 64) return 0;
	memcpy(name, from, len);
	name[len] = 0;
	return 1;
}

/* parent_dir PATH: PATH (PATH_MAX bytes) becomes the folder that holds it */
static void parent_dir(char *p)
{
	char *s = strrchr(p, '/');
	if (s && s[1] && strcmp(s + 1, ".") && strcmp(s + 1, "..")) *(s == p ? s + 1 : s) = 0;
	else if (!s && strcmp(p, ".") && strcmp(p, "..")) strcpy(p, ".");
	else if (strlen(p) + 4 < PATH_MAX) strcat(p, "/..");
}

/* run_tests N FILES: gael test - compile the files with their top level's own forms jumped over (their defs run),
 * then call each (@test "name" ...) of them in turn, each in a run of its own, so one that fails stops only itself.
 * A gem's main file is turned on as that gem, from that file; the tests of the gems a program turns on do not run */
static int run_tests(int n, char **files)
{
	i32 k, pass = 0, fail = 0, g;
	int st, i;
	char name[100];
	g_test_mode = 1;
	for (i = 0; i < n; i++) { /* the gems named first, so their tests are there also when another one needs them */
		if (!gem_file(files[i], name)) continue;
		if ((g = gem_find(name)) < 0 && (g = gem_register(name, files[i])) < 0) return 1;
		g_gems[g].tested = 1;
	}
	for (i = 0; i < n; i++) {
		if (gem_file(files[i], name)) { /* gael test gems/NAME.gael: the program (enable NAME), with NAME from here */
			char src[100];
			sprintf(src, "(enable %.64s)\n", name);
			if (strlen(files[i]) >= sizeof(g_gem_dir)) return 1;
			strcpy(g_gem_dir, files[i]); /* the gems it requires: next to it first */
			parent_dir(g_gem_dir);
			if (!strcmp(strrchr(files[i], '/') ? strrchr(files[i], '/') + 1 : files[i], "main.gael")) parent_dir(g_gem_dir);
			st = load("<gael test>", src, strlen(src), 0, RUN);
			g_gem_dir[0] = 0;
			if (st) return st;
		} else if ((st = load(files[i], 0, 0, 0, RUN)) != 0) return st;
	}
	for (k = 0; k < g_ntests; k++) {
		i32 fi, stub;
		sprintf(name, "@test:%ld", (long)k);
		if ((fi = find_fn(name)) < 0) continue;
		stub = g_ncode;
		emit(OP_ENTER, 0, 0, g_tests[k].line);
		emit(OP_CALL, fi, 0, g_tests[k].line);
		emit(OP_HALT, 0, 0, g_tests[k].line);
		st = vm_run(stub);
		fflush(stderr);
		if (!st) {
			printf("ok %s\n", g_tests[k].name);
			pass++;
		} else {
			printf("FAIL %s (%s:%ld)\n", g_tests[k].name, g_tests[k].file, (long)g_tests[k].line);
			fail++;
		}
		fflush(stdout);
	}
	printf("%ld passed, %ld failed\n", (long)pass, (long)fail);
	return fail ? 1 : 0;
}

int main(int argc, char **argv)
{
	int st, k;
	vm_init(getenv("GAEL_CHECK") && !strcmp(getenv("GAEL_CHECK"), "1")); /* the text arena first: compile_init writes to it */
	compile_init();
	if (argc == 1) return repl();
	if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
		for (k = 0; g_usage[k]; k++) fputs(g_usage[k], stdout);
		for (k = 0; g_usage_env[k]; k++) fputs(g_usage_env[k], stdout);
		return 0;
	}
	if (!strcmp(argv[1], "-n") && argc == 2) { /* every name the language takes, once: for the reference and for editors */
		size_t i;
		size_t j;
		for (i = 0; i < GAEL_COUNT(g_reserved); i++) puts(g_reserved[i]);
		for (i = 0; i < GAEL_COUNT(g_builtins); i++) {
			for (j = 0; j < GAEL_COUNT(g_reserved) && strcmp(g_reserved[j], g_builtins[i].name); j++) {}
			if (j == GAEL_COUNT(g_reserved)) puts(g_builtins[i].name);
		}
		for (i = 0; i < GAEL_COUNT(g_soft_names); i++) puts(g_soft_names[i]); /* min, max...: built in, not reserved */
		for (i = T_UNIT; i < T_USER; i++) {
			const NameSlot *sl = names_get(g_prim_tys[i].name);
			if (sl && sl->type >= 0 && !sl->reserved) puts(g_prim_tys[i].name); /* NodePath: a type, not a reserved word */
		}
		return 0;
	}
	if (argc >= 3 && !strcmp(argv[1], "test")) {
		st = run_tests(argc - 2, argv + 2);
	} else if (argc == 2 && !strcmp(argv[1], "doc")) { /* the cheat sheet for models, the one that came with this gael */
		char path[PATH_MAX];
		FILE *f;
		int c;
		st = 1;
		if (exe_dir(path) && strlen(path) + 20 < sizeof(path)) {
			strcat(path, "/docs/MODELS.md");
			if ((f = fopen(path, "r")) != 0) {
				while ((c = getc(f)) != EOF) putchar(c);
				fclose(f);
				st = 0;
			}
		}
		if (st) fprintf(stderr, "gael: no docs/MODELS.md next to the gael binary\n");
	} else if (argc == 3 && !strcmp(argv[1], "check")) {
		st = load(argv[2], 0, 0, 0, CHECK);
	} else if (argv[1][0] != '-') {
		k = argc >= 3 && !strcmp(argv[1], "run"); /* gael run FILE is gael FILE */
		args_init(argc - 2 - k, argv + 2 + k);
		st = load(argv[1 + k], 0, 0, 0, RUN);
		if (!st) st = leaks();
	} else if (argc == 3 && (!strcmp(argv[1], "-c") || !strcmp(argv[1], "-d") || !strcmp(argv[1], "-m"))) {
		st = load(argv[2], 0, 0, 0, argv[1][1] == 'c' ? CHECK : argv[1][1] == 'd' ? DIS : EXPAND);
	} else if (argc == 3 && !strcmp(argv[1], "-g")) { /* the WGSL of each gpu-each, as the GPU gets it */
		i32 i;
		st = load(argv[2], 0, 0, 0, CHECK);
		for (i = 0; !st && i < g_nkernels; i++) printf("// gpu-each at %s:%ld\n%s", g_kernels[i].file, (long)g_kernels[i].line, g_kernels[i].wgsl ? g_kernels[i].wgsl : "");
	} else if (argc == 3 && !strcmp(argv[1], "-x")) { /* the inspector, as text */
		i32 i;
		st = load(argv[2], 0, 0, 0, CHECK);
		for (i = 0; !st && i < g_nglobals; i++) {
			const Global *gl = &g_globals[i];
			if (!gl->exported) continue;
			printf("%s : %s", gl->name, tn(gl->type));
			if (gl->value) printf(gl->type == T_STR ? " = \"%s\"" : " = %s", gl->value);
			printf("\t%s:%ld\n", gl->file, (long)gl->line);
		}
		for (i = 0; !st && i < g_nmarks; i++)
			printf("%s:%ld: @%s: %s\n", g_marks[i].file, (long)g_marks[i].line, g_marks[i].err ? "err" : "warn", g_marks[i].text);
	} else if (argc >= 3 && !strcmp(argv[1], "-e")) {
		args_init(argc - 3, argv + 3);
		st = load("<-e>", argv[2], strlen(argv[2]), 0, RUN);
		if (!st) st = leaks();
	} else {
		for (k = 0; g_usage[k]; k++) fputs(g_usage[k], stderr);
		for (k = 0; g_usage_env[k]; k++) fputs(g_usage_env[k], stderr);
		return 2;
	}
	fflush(stdout);
	return st;
}
