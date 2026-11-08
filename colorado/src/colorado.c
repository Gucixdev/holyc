/* colorado - create and customise one colour theme and apply it to every toolkit (GTK2/3/4, Qt5/6, terminals, our desktop, ...).
 * Thin main: all logic is in src/mods/ (stb-style single headers). Safety: `apply` is a dry run unless --apply is given; every real write is
 * backed up first and `colorado rollback` restores byte-identical state. Nothing is ever restarted or reloaded.
 * Build: see ../run.  Not to be confused with notcoreutils/colorado.c (the old stdin log colourizer, untouched; `colorado colorize` may expose it later).
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <signal.h>
#include "mods/apply.h"
#include "mods/gens.h"
#include "mods/image.h"
#include "mods/tui.h"

#define VERSION "0.1.0"
static char g_loaded_path[PATH_MAX];

static const char *usage =
	"usage: colorado COMMAND [ARGS]\n"
	"  list                         themes found (built-in files next to the binary, then ~/.config/colorado/themes)\n"
	"  show NAME                    print a theme with swatches\n"
	"  new [--name N] [--accent #hex] [--bg #hex] [--fg #hex] [--light|--dark] [--out FILE|-]\n"
	"                               derive a full theme from 1-3 colours (asks questions when run on a terminal without --accent)\n"
	"  edit NAME                    open the theme file in $EDITOR (a built-in is copied to your themes dir first)\n"
	"  export NAME GEN [--out DIR]  print (or write to DIR) the files generator GEN would produce; GEN = name, default or all\n"
	"  set NAME [--only a,b|--all] [--reload] [--dry-run]   THE main command: apply the theme everywhere: shows the diff, backs up, writes, remembers it as active\n"
	"  current [-v]                 what is active, and per toolkit whether its file still matches the theme (DRIFTED = changed behind our back); exit 1 on drift\n"
	"  watch NAME [--reload] [--interval S]   re-apply (with backup) whenever the theme file changes\n"
	"  apply NAME [--only a,b|--all] [--apply]\n"
	"                               the same as set, but a dry run unless --apply\n"
	"  rollback [DIR|list]          restore the newest backup (or DIR); `list` shows backups\n"
	"  check NAME                   WCAG contrast of all role pairs, exit 1 on failures\n"
	"  extract IMAGE [--name N] [--dark|--light] [--out FILE|-]   palette from an image (PPM/PAM built in, others need ImageMagick)\n"
	"  contrast A B                 contrast ratio of two #hex colours\n"
	"  preview [NAME]               the TUI on a terminal, a swatch table otherwise;  -tui [NAME] is the same\n"
	"  generators                   what each generator writes and whether its format was verified\n"
	"  version | help\n"
	"environment: HOME, XDG_CONFIG_HOME, XDG_DATA_HOME decide where apply reads/writes; COLORADO_THEMES adds a theme dir.\n";

/* ---------- theme lookup ---------- */
static int co_dirs(const co_roots *r, char dirs[4][PATH_MAX]) {
	int n = 0; const char *e = getenv("COLORADO_THEMES");
	if (e && *e) snprintf(dirs[n++], PATH_MAX, "%s", e);
	snprintf(dirs[n++], PATH_MAX, "%s/colorado/themes", r->config);
	char exe[PATH_MAX]; ssize_t k = readlink("/proc/self/exe", exe, sizeof exe - 1);
	if (k > 0) {
		exe[k] = 0; char *sl = strrchr(exe, '/'); if (sl) *sl = 0;
		snprintf(dirs[n++], PATH_MAX, "%s/../themes", exe); snprintf(dirs[n++], PATH_MAX, "%s/themes", exe);
	}
	return n;
}

static int co_load_path(const char *path, co_theme *t) {
	char *txt = co_read_file(path, NULL); char err[160];
	if (!txt) { fprintf(stderr, "colorado: cannot read %s: %s\n", path, strerror(errno)); return -1; }
	int rc = co_theme_parse(txt, t, err, sizeof err); free(txt);
	if (rc) { fprintf(stderr, "colorado: %s: %s\n", path, err); return -1; }
	snprintf(g_loaded_path, sizeof g_loaded_path, "%s", path);
	return 0;
}
static int co_load(const co_roots *r, const char *name, co_theme *t) {
	if (strchr(name, '/') || (strlen(name) > 6 && !strcmp(name + strlen(name) - 6, ".theme"))) return co_load_path(name, t);
	char dirs[4][PATH_MAX]; int n = co_dirs(r, dirs);
	for (int i = 0; i < n; i++) { char p[PATH_MAX + 80]; snprintf(p, sizeof p, "%s/%s.theme", dirs[i], name); if (!access(p, R_OK)) return co_load_path(p, t); }
	fprintf(stderr, "colorado: no theme '%s' (try `colorado list`)\n", name);
	return -1;
}
static int co_user_path(const co_roots *r, const char *name, char *out, size_t n) {
	for (const char *p = name; *p; p++) if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_' && *p != '.') { fprintf(stderr, "colorado: theme names may only contain letters, digits, - _ .\n"); return -1; }
	if (!*name || name[0] == '.') { fprintf(stderr, "colorado: bad theme name\n"); return -1; }
	snprintf(out, n, "%s/colorado/themes/%s.theme", r->config, name); return 0;
}
static int co_save_theme(const co_theme *t, const char *path) {
	if (strcmp(path, "-")) { char d[PATH_MAX + 4]; snprintf(d, sizeof d, "%s", path); char *sl = strrchr(d, '/'); if (sl) { strcpy(sl, "/x"); if (co_mkdir_p(d, NULL)) { fprintf(stderr, "colorado: mkdir for %s: %s\n", path, strerror(errno)); return -1; } } }
	co_sb o = { 0 }; co_theme_emit(t, &o); int rc = 0;
	if (!strcmp(path, "-")) fwrite(o.p, 1, o.n, stdout);
	else { char tmp[PATH_MAX + 8]; snprintf(tmp, sizeof tmp, "%s.tmp", path); FILE *f = fopen(tmp, "w");
		if (!f || fwrite(o.p, 1, o.n, f) != o.n || fclose(f) || rename(tmp, path)) { fprintf(stderr, "colorado: writing %s: %s\n", path, strerror(errno)); rc = -1; } else fprintf(stderr, "saved %s\n", path); }
	co_sb_free(&o); return rc;
}

/* ---------- generator selection ---------- */
static char g_gens[512];
static int co_select(co_set *s, const co_theme *t, const char *only, int all) {
	g_gens[0] = 0;
	if (only && strcmp(only, "default") && strcmp(only, "all")) {
		char *cp = strdup(only);
		for (char *tok = strtok(cp, ","); tok; tok = strtok(NULL, ",")) { const co_gen *g = co_gen_find(tok); if (!g) { fprintf(stderr, "colorado: no generator '%s' (see `colorado generators`)\n", tok); free(cp); return -1; } g->fn(t, s); strncat(g_gens, g_gens[0] ? "," : "", sizeof g_gens - strlen(g_gens) - 1); strncat(g_gens, tok, sizeof g_gens - strlen(g_gens) - 1); }
		free(cp); return 0;
	}
	for (int i = 0; i < CO_NGENS; i++) if (all || (only && !strcmp(only, "all")) || co_gens[i].verified) { co_gens[i].fn(t, s); strncat(g_gens, g_gens[0] ? "," : "", sizeof g_gens - strlen(g_gens) - 1); strncat(g_gens, co_gens[i].name, sizeof g_gens - strlen(g_gens) - 1); }
	return 0;
}

/* ---------- commands ---------- */
static void co_swatch(const co_theme *t, int i, int color) {
	char h[8]; co_hex(t->c[i], h);
	if (color) printf("%-13s %s  \033[48;2;%d;%d;%dm      \033[0m\n", co_role_name(i), h, t->c[i].r, t->c[i].g, t->c[i].b);
	else printf("%-13s %s\n", co_role_name(i), h);
}

static int cmd_list(const co_roots *r) {
	char dirs[4][PATH_MAX]; int n = co_dirs(r, dirs), found = 0;
	for (int i = 0; i < n; i++) {
		DIR *d = opendir(dirs[i]); if (!d) continue;
		struct dirent **names = NULL; int nn = 0, cap = 0;
		for (struct dirent *e; (e = readdir(d));) { size_t l = strlen(e->d_name); if (l > 6 && !strcmp(e->d_name + l - 6, ".theme")) { if (nn == cap) { cap = cap ? cap * 2 : 16; names = realloc(names, (size_t)cap * sizeof *names); } names[nn] = malloc(sizeof **names); memcpy(names[nn], e, sizeof **names); nn++; } }
		closedir(d);
		for (int a = 0; a < nn; a++) for (int b = a + 1; b < nn; b++) if (strcmp(names[a]->d_name, names[b]->d_name) > 0) { struct dirent *x = names[a]; names[a] = names[b]; names[b] = x; }
		for (int a = 0; a < nn; a++) {
			char p[PATH_MAX + 300]; snprintf(p, sizeof p, "%s/%s", dirs[i], names[a]->d_name); co_theme t; char *txt = co_read_file(p, NULL); char err[160];
			if (txt && !co_theme_parse(txt, &t, err, sizeof err)) { printf("%-20s %-6s %s\n", t.name, t.dark ? "dark" : "light", dirs[i]); found++; }
			else printf("%-20s %-6s %s (invalid: %s)\n", names[a]->d_name, "?", dirs[i], txt ? err : "unreadable");
			free(txt); free(names[a]);
		}
		free(names);
	}
	if (!found) fprintf(stderr, "colorado: no themes found\n");
	return found ? 0 : 1;
}

static int cmd_show(const co_roots *r, const char *name) {
	co_theme t; if (co_load(r, name, &t)) return 1;
	int color = isatty(1);
	printf("theme %s (%s)  font %s %d / %s  cursor %s %d  icons %s  radius %d gap %d border %d\n", t.name, t.dark ? "dark" : "light", t.font, t.font_size, t.mono_font, t.cursor_theme, t.cursor_size, t.icon_theme, t.radius, t.gap, t.border_width);
	for (int i = 0; i < CR_N; i++) co_swatch(&t, i, color);
	return 0;
}

static int cmd_check(const co_roots *r, const char *name) {
	co_theme t; if (co_load(r, name, &t)) return 2;
	co_sb o = { 0 }; int f = co_theme_check(&t, &o); fputs(o.p, stdout); co_sb_free(&o);
	return f ? 1 : 0;
}

static int ask(const char *q, char *buf, size_t n) { printf("%s", q); fflush(stdout); if (!fgets(buf, (int)n, stdin)) return -1; buf[strcspn(buf, "\r\n")] = 0; return 0; }

static int cmd_new(const co_roots *r, int argc, char **argv) {
	const char *name = NULL, *out = NULL; co_rgb acc, bg, fg; int has_acc = 0, has_bg = 0, has_fg = 0, dark = -1;
	for (int i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
		else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
		else if (!strcmp(argv[i], "--accent") && i + 1 < argc) { if (co_hex_parse(argv[++i], &acc)) { fprintf(stderr, "colorado: bad --accent\n"); return 2; } has_acc = 1; }
		else if (!strcmp(argv[i], "--bg") && i + 1 < argc) { if (co_hex_parse(argv[++i], &bg)) { fprintf(stderr, "colorado: bad --bg\n"); return 2; } has_bg = 1; }
		else if (!strcmp(argv[i], "--fg") && i + 1 < argc) { if (co_hex_parse(argv[++i], &fg)) { fprintf(stderr, "colorado: bad --fg\n"); return 2; } has_fg = 1; }
		else if (!strcmp(argv[i], "--dark")) dark = 1; else if (!strcmp(argv[i], "--light")) dark = 0;
		else { fprintf(stderr, "colorado new: unknown argument %s\n", argv[i]); return 2; }
	}
	char b[128], nm[64];
	if (!has_acc) {
		if (!isatty(0)) { fprintf(stderr, "colorado new: --accent #hex is required when not on a terminal\n"); return 2; }
		do { if (ask("accent colour (#rrggbb): ", b, sizeof b)) return 2; } while (co_hex_parse(b, &acc)); has_acc = 1;
		if (!ask("background (#rrggbb, empty = derive): ", b, sizeof b) && *b && !co_hex_parse(b, &bg)) has_bg = 1;
		if (!ask("foreground (#rrggbb, empty = derive): ", b, sizeof b) && *b && !co_hex_parse(b, &fg)) has_fg = 1;
		if (dark < 0 && !has_bg) { if (!ask("dark or light [d/l]: ", b, sizeof b)) dark = b[0] != 'l' && b[0] != 'L'; }
		if (!name) { if (!ask("name: ", nm, sizeof nm)) name = nm; }
	}
	if (dark < 0) dark = 1;
	if (!name || !*name) name = "custom";
	co_theme t; co_theme_derive(&t, name, acc, has_bg ? &bg : NULL, has_fg ? &fg : NULL, dark);
	char path[PATH_MAX]; if (!out) { if (co_user_path(r, name, path, sizeof path)) return 2; out = path; }
	if (co_save_theme(&t, out)) return 1;
	co_sb o = { 0 }; int f = co_theme_check(&t, &o); co_sb_free(&o);
	fprintf(stderr, "%d WCAG failure%s (run `colorado check %s`)\n", f, f == 1 ? "" : "s", name);
	return 0;
}

static int cmd_extract(const co_roots *r, int argc, char **argv) {
	if (argc < 1) { fprintf(stderr, "usage: colorado extract IMAGE [--name N] [--dark|--light] [--out FILE|-]\n"); return 2; }
	const char *img = argv[0], *name = NULL, *out = NULL; int dark = -1;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i]; else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
		else if (!strcmp(argv[i], "--dark")) dark = 1; else if (!strcmp(argv[i], "--light")) dark = 0;
		else { fprintf(stderr, "colorado extract: unknown argument %s\n", argv[i]); return 2; }
	}
	co_img im = { 0 }; char err[300];
	if (co_img_load(img, &im, err, sizeof err)) { fprintf(stderr, "colorado: %s\n", err); return 1; }
	char def[64]; if (!name) { const char *bn = strrchr(img, '/'); bn = bn ? bn + 1 : img; snprintf(def, sizeof def, "x-%.40s", bn); char *dot = strrchr(def, '.'); if (dot) *dot = 0; for (char *p = def; *p; p++) if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_') *p = '-'; name = def; }
	co_theme t; co_extract(&im, name, dark, &t); free(im.rgb);
	char path[PATH_MAX]; if (!out) { if (co_user_path(r, name, path, sizeof path)) return 2; out = path; }
	if (co_save_theme(&t, out)) return 1;
	return 0;
}

static int cmd_contrast(int argc, char **argv) {
	co_rgb a, b; if (argc != 2 || co_hex_parse(argv[0], &a) || co_hex_parse(argv[1], &b)) { fprintf(stderr, "usage: colorado contrast #fg #bg\n"); return 2; }
	double c = co_contrast(a, b);
	printf("%.2f:1  AA text %s  AA large/UI %s  AAA text %s\n", c, c >= 4.5 ? "pass" : "FAIL", c >= 3 ? "pass" : "FAIL", c >= 7 ? "pass" : "FAIL");
	return c >= 4.5 ? 0 : 1;
}

static int cmd_export(const co_roots *r, int argc, char **argv) {
	if (argc < 2) { fprintf(stderr, "usage: colorado export NAME GEN [--out DIR]\n"); return 2; }
	const char *outdir = NULL; for (int i = 2; i < argc; i++) { if (!strcmp(argv[i], "--out") && i + 1 < argc) outdir = argv[++i]; else { fprintf(stderr, "colorado export: unknown argument %s\n", argv[i]); return 2; } }
	co_theme t; if (co_load(r, argv[0], &t)) return 1;
	co_set s = { 0 }; if (co_select(&s, &t, argv[1], 0)) return 2;
	for (int i = 0; i < s.n; i++) {
		co_file *f = &s.f[i];
		if (outdir) {
			char p[PATH_MAX]; snprintf(p, sizeof p, "%s/%s", outdir, f->rel); char d[PATH_MAX]; snprintf(d, sizeof d, "%s", p);
			co_sb out = { 0 }; co_merge("", f, &out);
			if (co_mkdir_p(p, NULL) || co_write_all(p, out.p, out.n, 0644)) { fprintf(stderr, "colorado: writing %s: %s\n", p, strerror(errno)); return 1; }
			co_sb_free(&out); printf("wrote %s\n", p);
		} else { co_sb out = { 0 }; co_merge("", f, &out); printf("==> %s <==\n%s", f->rel, out.p); if (out.n && out.p[out.n - 1] != '\n') puts(""); co_sb_free(&out); }
	}
	if (!outdir && s.notes.p) printf("==> notes <==\n%s", s.notes.p);
	co_set_free(&s); return 0;
}

/* ---------- reload hooks: only things that are safe on a running session, never kill or restart anything ---------- */
static int co_spawn_quiet(char *const argv[]) {
	fflush(stdout); pid_t p = fork(); if (p < 0) return -1;
	if (p == 0) { int nul = open("/dev/null", O_RDWR); if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); } execvp(argv[0], argv); _exit(127); }
	int st; if (waitpid(p, &st, 0) < 0) return -1; return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
static int co_signal_named(const char *comm, int sig) { /* SIGnal our own user's processes whose /proc/PID/comm is exactly `comm`; returns how many */
	DIR *d = opendir("/proc"); int n = 0; if (!d) return 0;
	for (struct dirent *e; (e = readdir(d));) {
		if (!isdigit((unsigned char)e->d_name[0])) continue;
		char p[300]; snprintf(p, sizeof p, "/proc/%s/comm", e->d_name); struct stat st;
		if (stat(p, &st) || st.st_uid != getuid()) continue;
		char *c = co_read_file(p, NULL); if (!c) continue;
		c[strcspn(c, "\n")] = 0;
		if (!strcmp(c, comm) && kill((pid_t)atoi(e->d_name), sig) == 0) n++;
		free(c);
	}
	closedir(d); return n;
}
static int co_has_gen(const char *list, const char *g) { char *cp = strdup(list); int r = 0; for (char *t = strtok(cp, ","); t; t = strtok(NULL, ",")) if (!strcmp(t, g)) r = 1; free(cp); return r; }

static void co_hooks(const co_roots *r, const char *gens, int run) {
	printf("\n# live reload (only hooks that cannot disturb a running app; nothing is ever killed or restarted)%s\n", run ? "" : " - not run, add --reload");
	if (co_has_gen(gens, "tmux")) {
		char f[PATH_MAX + 32]; snprintf(f, sizeof f, "%s/tmux/colorado.conf", r->config);
		char *a1[] = { "tmux", "has-session", NULL }; char *a2[] = { "tmux", "source-file", f, NULL };
		if (!run) printf("  tmux        would `tmux source-file %s` if a server is running\n", f);
		else if (co_spawn_quiet(a1) != 0) printf("  tmux        no running server, nothing to do\n");
		else printf("  tmux        %s\n", co_spawn_quiet(a2) == 0 ? "reloaded (source-file)" : "source-file FAILED");
	}
	if (co_has_gen(gens, "xsettingsd")) {
		if (!run) printf("  xsettingsd  would send SIGHUP to a running xsettingsd (its documented reload)\n");
		else { int n = co_signal_named("xsettingsd", SIGHUP); printf("  xsettingsd  %s\n", n ? "SIGHUP sent (reloads settings)" : "not running"); }
	}
	if (co_has_gen(gens, "xresources")) {
		char f[PATH_MAX + 8]; snprintf(f, sizeof f, "%s/.Xresources", r->home); char *a[] = { "xrdb", "-merge", f, NULL };
		if (!run) printf("  xresources  would run `xrdb -merge %s` when DISPLAY is set (new X clients only)\n", f);
		else if (!getenv("DISPLAY")) printf("  xresources  no DISPLAY, skipped\n"); else printf("  xresources  %s\n", co_spawn_quiet(a) == 0 ? "merged" : "xrdb missing or failed");
	}
	printf("  everything else is picked up on the app's next start / new window (see readme: live vs restart)\n");
}

/* the file that remembers what is active: part of every apply, so it is backed up and rolled back like the rest */
static void co_add_active(co_set *s, const co_theme *t, const char *path) {
	char rp[PATH_MAX]; if (!realpath(path, rp)) snprintf(rp, sizeof rp, "%s", path);
	co_sb_f(&co_set_add(s, "colorado", W_CONFIG, "colorado/active", K_WHOLE)->body, "name=%s\npath=%s\ngens=%s\n", t->name, rp, g_gens);
}

static int cmd_apply(const co_roots *r, int argc, char **argv, int is_set) {
	if (argc < 1) { fprintf(stderr, "usage: colorado %s NAME [--only a,b|--all]%s\n", is_set ? "set" : "apply", is_set ? " [--dry-run] [--reload]" : " [--apply]"); return 2; }
	const char *only = NULL; int all = 0, real = is_set, reload = 0;
	for (int i = 1; i < argc; i++) { if (!strcmp(argv[i], "--only") && i + 1 < argc) only = argv[++i]; else if (!strcmp(argv[i], "--all")) all = 1;
		else if (!is_set && !strcmp(argv[i], "--apply")) real = 1; else if (is_set && !strcmp(argv[i], "--dry-run")) real = 0; else if (!strcmp(argv[i], "--reload")) reload = 1;
		else { fprintf(stderr, "colorado %s: unknown argument %s\n", is_set ? "set" : "apply", argv[i]); return 2; } }
	co_theme t; if (co_load(r, argv[0], &t)) return 1;
	co_set s = { 0 }; if (co_select(&s, &t, only, all)) return 2;
	char gens[512]; snprintf(gens, sizeof gens, "%s", g_gens);
	co_add_active(&s, &t, g_loaded_path);
	printf("# colorado %s %s -> HOME=%s config=%s data=%s\n", is_set ? "set" : "apply", t.name, r->home, r->config, r->data);
	int changed = co_plan(&s, r);
	if (changed < 0) return 1;
	if (s.notes.p) printf("\n# notes (nothing below is run by colorado)\n%s", s.notes.p);
	int rc = 0;
	if (!real) printf("\n# dry run: %d file%s would change. Nothing was written. Re-run with %s to back up and write.\n", changed, changed == 1 ? "" : "s", is_set ? "no --dry-run" : "--apply");
	else if (!changed) printf("\n# nothing to change.\n");
	else { char bd[PATH_MAX]; int w = co_apply(&s, r, bd, sizeof bd); if (w < 0) rc = 1; else printf("\n# wrote %d file%s. Backup: %s\n# Undo with: colorado rollback\n", w, w == 1 ? "" : "s", bd); }
	if (rc == 0) co_hooks(r, gens, real && reload);
	co_set_free(&s); return rc;
}

/* parse colorado/active into name/path/gens; returns 0 when something is active */
static int co_read_active(const co_roots *r, char *name, char *path, char *gens) {
	char p[PATH_MAX + 32]; snprintf(p, sizeof p, "%s/colorado/active", r->config); char *txt = co_read_file(p, NULL);
	if (!txt) return -1;
	name[0] = path[0] = gens[0] = 0;
	for (char *l = strtok(txt, "\n"); l; l = strtok(NULL, "\n")) {
		if (!strncmp(l, "name=", 5)) snprintf(name, 64, "%s", l + 5); else if (!strncmp(l, "path=", 5)) snprintf(path, PATH_MAX, "%s", l + 5); else if (!strncmp(l, "gens=", 5)) snprintf(gens, 512, "%s", l + 5);
	}
	free(txt); return name[0] && path[0] ? 0 : -1;
}

static int cmd_current(const co_roots *r, int verbose) {
	char name[64], path[PATH_MAX], gens[512];
	if (co_read_active(r, name, path, gens)) { printf("no theme has been set with colorado in %s (run `colorado set NAME`)\n", r->config); return 3; }
	co_theme t; if (co_load_path(path, &t)) { fprintf(stderr, "colorado: the active theme file %s cannot be loaded; `colorado set` another one\n", path); return 1; }
	printf("active theme: %s (%s)  file: %s\n", t.name, t.dark ? "dark" : "light", path);
	co_set s = { 0 }; char *cp = strdup(gens); int drift = 0;
	for (char *tok = strtok(cp, ","); tok; tok = strtok(NULL, ",")) { const co_gen *g = co_gen_find(tok); if (g) g->fn(&t, &s); }
	free(cp);
	printf("%-11s %-9s %s\n", "toolkit", "state", "file");
	for (int i = 0; i < s.n; i++) {
		char fp[PATH_MAX]; char *old; co_sb nw = { 0 }; int ex; int ch = co_compute(r, &s.f[i], fp, sizeof fp, &old, &nw, &ex);
		printf("%-11s %-9s %s\n", s.f[i].gen, !ex ? "MISSING" : ch ? "DRIFTED" : "in sync", fp);
		if (ch) { drift++; if (verbose && ex) { const char *a = old, *b = nw.p; int ln = 1; while (*a && *b) { const char *ea = strchr(a, '\n'), *eb = strchr(b, '\n'); size_t la = ea ? (size_t)(ea - a) : strlen(a), lb = eb ? (size_t)(eb - b) : strlen(b);
			if (la != lb || strncmp(a, b, la)) { printf("            first difference at line %d: have '%.*s' want '%.*s'\n", ln, (int)la, a, (int)lb, b); break; }
			a += la + (ea ? 1 : 0); b += lb + (eb ? 1 : 0); ln++; } } }
		free(old); co_sb_free(&nw);
	}
	co_set_free(&s);
	if (drift) printf("%d file%s differ%s from the theme: edited by hand, by another tool, or the theme file changed. `colorado set %s` re-applies (with backup).\n", drift, drift == 1 ? "" : "s", drift == 1 ? "s" : "", t.name);
	else printf("everything managed is in sync with the theme\n");
	return drift ? 1 : 0;
}

static int cmd_watch(const co_roots *r, int argc, char **argv) {
	if (argc < 1) { fprintf(stderr, "usage: colorado watch NAME [--reload] [--interval SECONDS] [--max N]\n"); return 2; }
	int reload = 0, interval = 2, max = 0;
	for (int i = 1; i < argc; i++) { if (!strcmp(argv[i], "--reload")) reload = 1; else if (!strcmp(argv[i], "--interval") && i + 1 < argc) interval = atoi(argv[++i]); else if (!strcmp(argv[i], "--max") && i + 1 < argc) max = atoi(argv[++i]); else { fprintf(stderr, "colorado watch: unknown argument %s\n", argv[i]); return 2; } }
	if (interval < 1) interval = 1;
	co_theme t; if (co_load(r, argv[0], &t)) return 1;
	char file[PATH_MAX]; snprintf(file, sizeof file, "%s", g_loaded_path);
	struct stat st; if (stat(file, &st)) { perror("colorado: stat"); return 1; }
	time_t last = st.st_mtime; off_t lsz = st.st_size; int done = 0;
	fprintf(stderr, "colorado: watching %s (every %ds), re-applying with backup on change; Ctrl-C stops\n", file, interval);
	for (;;) {
		sleep((unsigned)interval);
		if (stat(file, &st)) continue;
		if (st.st_mtime == last && st.st_size == lsz) continue;
		last = st.st_mtime; lsz = st.st_size;
		co_theme n; if (co_load_path(file, &n)) { fprintf(stderr, "colorado watch: theme is invalid right now, keeping the previous result\n"); continue; }
		co_set s = { 0 }; if (co_select(&s, &n, NULL, 0)) { co_set_free(&s); continue; }
		char gens[512]; snprintf(gens, sizeof gens, "%s", g_gens); co_add_active(&s, &n, file);
		char bd[PATH_MAX]; int w = co_apply(&s, r, bd, sizeof bd);
		printf("watch: theme changed, %d file%s written, backup %s\n", w, w == 1 ? "" : "s", w > 0 ? bd : "-");
		if (w > 0) co_hooks(r, gens, reload);
		co_set_free(&s); fflush(stdout);
		if (max && ++done >= max) return 0;
	}
}

static int cmd_rollback(const co_roots *r, int argc, char **argv) {
	if (argc >= 1 && !strcmp(argv[0], "list")) {
		char root[PATH_MAX]; snprintf(root, sizeof root, "%s/colorado/backups", r->data); DIR *d = opendir(root);
		if (!d) { fprintf(stderr, "colorado: no backups\n"); return 1; }
		for (struct dirent *e; (e = readdir(d));) if (e->d_name[0] != '.') { char rb[PATH_MAX + 300]; snprintf(rb, sizeof rb, "%s/%s/rolled-back", root, e->d_name); printf("%s/%s%s\n", root, e->d_name, access(rb, F_OK) ? "" : "  (rolled back)"); }
		closedir(d); return 0;
	}
	return co_rollback(r, argc >= 1 ? argv[0] : NULL) ? 1 : 0;
}

static int cmd_generators(void) {
	printf("%-11s %-4s %s\n", "generator", "set", "writes / status");
	for (int i = 0; i < CO_NGENS; i++) printf("%-11s %-4s %s\n%-16s%s\n", co_gens[i].name, co_gens[i].verified ? "dflt" : "opt", co_gens[i].what, "", co_gens[i].status);
	printf("\ndflt = in the default apply set (verified formats); opt = only with --only NAME or --all\n");
	return 0;
}

static int cmd_preview(const co_roots *r, const char *name) {
	co_theme t; if (co_load(r, name ? name : "dark", &t)) return 1;
	if (isatty(0) && isatty(1)) {
		char save[PATH_MAX];
		if (strstr(g_loaded_path, "/colorado/themes/")) snprintf(save, sizeof save, "%s", g_loaded_path);
		else if (co_user_path(r, t.name, save, sizeof save)) return 1;
		char d[PATH_MAX + 4]; snprintf(d, sizeof d, "%s", save); char *sl = strrchr(d, '/'); if (sl) { strcpy(sl, "/x"); co_mkdir_p(d, NULL); }
		return co_tui(&t, save);
	}
	return cmd_show(r, name ? name : "dark");
}

int main(int argc, char **argv) {
	setvbuf(stdout, NULL, _IOLBF, 0);
	co_roots r; if (co_roots_env(&r)) return 2;
	if (argc < 2) { fputs(usage, stdout); return 0; }
	const char *c = argv[1]; int n = argc - 2; char **a = argv + 2;
	if (!strcmp(c, "-tui") || !strcmp(c, "--tui") || !strcmp(c, "-t")) return cmd_preview(&r, n ? a[0] : NULL);
	if (!strcmp(c, "help") || !strcmp(c, "-h") || !strcmp(c, "--help")) { fputs(usage, stdout); return 0; }
	if (!strcmp(c, "version") || !strcmp(c, "--version")) { puts("colorado " VERSION); return 0; }
	if (!strcmp(c, "list")) return cmd_list(&r);
	if (!strcmp(c, "show") && n == 1) return cmd_show(&r, a[0]);
	if (!strcmp(c, "check") && n == 1) return cmd_check(&r, a[0]);
	if (!strcmp(c, "new")) return cmd_new(&r, n, a);
	if (!strcmp(c, "extract")) return cmd_extract(&r, n, a);
	if (!strcmp(c, "contrast")) return cmd_contrast(n, a);
	if (!strcmp(c, "export")) return cmd_export(&r, n, a);
	if (!strcmp(c, "apply")) return cmd_apply(&r, n, a, 0);
	if (!strcmp(c, "set")) return cmd_apply(&r, n, a, 1);
	if (!strcmp(c, "current")) return cmd_current(&r, n && !strcmp(a[0], "-v"));
	if (!strcmp(c, "watch")) return cmd_watch(&r, n, a);
	if (!strcmp(c, "rollback")) return cmd_rollback(&r, n, a);
	if (!strcmp(c, "generators")) return cmd_generators();
	if (!strcmp(c, "preview")) return cmd_preview(&r, n ? a[0] : NULL);
	if (!strcmp(c, "edit") && n == 1) {
		co_theme t; if (co_load(&r, a[0], &t)) return 1;
		char up[PATH_MAX]; if (co_user_path(&r, t.name, up, sizeof up)) return 1;
		if (access(up, F_OK) && co_save_theme(&t, up)) return 1;
		const char *ed = getenv("EDITOR"); if (!ed || !*ed) ed = "vi";
		fflush(stdout); pid_t p = fork();
		if (p == 0) { execlp(ed, ed, up, (char *)NULL); _exit(127); }
		int st = 0; if (p < 0 || waitpid(p, &st, 0) < 0 || !WIFEXITED(st) || WEXITSTATUS(st)) { fprintf(stderr, "colorado: editor failed\n"); return 1; }
		co_theme t2; if (co_load_path(up, &t2)) { fprintf(stderr, "colorado: the file stays as you left it; fix it and run `colorado check %s`\n", t.name); return 1; }
		return 0;
	}
	fprintf(stderr, "colorado: unknown command or wrong arguments: %s\n\n%s", c, usage);
	return 2;
}
