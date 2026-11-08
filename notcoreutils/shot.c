/* shot - screenshot, screen recording and area selection for Wayland, one C file, no shell.
 *
 *   shot [OPTIONS] [MODE]              take a screenshot (default MODE: full)
 *   shot rec [OPTIONS] [MODE]          start a screen recording; run it again to stop it (a toggle, bind it to one key)
 *   shot sel                           let you drag an area and print it as "X,Y WxH" (for scripts and other tools)
 *   shot -l                            list the outputs (screens)
 *
 * MODE   full      every screen together (default)
 *        output    one screen: -o NAME, or the selected screen when no NAME is given (dwl)
 *        area      drag a rectangle
 *        window    the focused window (dwl: from its control socket)
 * OPTIONS
 *   -o NAME   which screen (implies MODE output)       -f FILE   write here instead of ~/Pictures/shot-DATE.png
 *   -d SEC    wait before capturing                    -c        clipboard only, no file
 *   -n        no clipboard copy, no notification       -a        (rec) record audio too
 *
 * Needs the Wayland tools grim (shots), slurp (area) and wf-recorder (rec) on PATH, wl-copy for the clipboard and, if there is
 * one, notify-send for the pop-up. The screens and the focused window come from dwl's control socket ($XDG_RUNTIME_DIR/dwl.sock)
 * when it is there; on other compositors `-o NAME` still works and `-l` falls back to wlr-randr.
 * `shot rec` keeps its state in $XDG_RUNTIME_DIR/shot.rec (the recorder's pid and the file).
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static char state_dir[256];

/* run argv, wait; capture stdout into OUT when given. returns the exit status (127 when it cannot start) */
static int run(char *const argv[], char *out, size_t outn, const char *stdin_file)
{
	int p[2] = { -1, -1 };
	pid_t pid;
	int st;
	if (out && pipe(p) < 0) return 127;
	pid = fork();
	if (pid < 0) return 127;
	if (pid == 0) {
		if (out) { dup2(p[1], 1); close(p[0]); close(p[1]); }
		if (stdin_file) { int fd = open(stdin_file, O_RDONLY); if (fd >= 0) { dup2(fd, 0); close(fd); } }
		execvp(argv[0], argv);
		_exit(127);
	}
	if (out) {
		size_t n = 0;
		ssize_t r;
		close(p[1]);
		while (n + 1 < outn && (r = read(p[0], out + n, outn - 1 - n)) > 0) n += r;
		out[n] = 0;
		close(p[0]);
	}
	waitpid(pid, &st, 0);
	return WIFEXITED(st) ? WEXITSTATUS(st) : 127;
}

static int have(const char *prog)
{
	const char *path = getenv("PATH");
	char buf[1024];
	if (!path) return 0;
	for (const char *s = path; *s;) {
		size_t n = strcspn(s, ":");
		snprintf(buf, sizeof buf, "%.*s/%s", (int)n, s, prog);
		if (access(buf, X_OK) == 0) return 1;
		s += n + (s[n] == ':');
	}
	return 0;
}

static void notify(const char *title, const char *body)
{
	if (have("notify-send")) {
		char *a[] = { "notify-send", "-a", "shot", (char *)title, (char *)body, NULL };
		run(a, NULL, 0, NULL);
	}
}

/* ---- dwl control socket ------------------------------------------------------------------------------------------ */

static int dwl_state(char *out, size_t n)
{
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	const char *rt = getenv("XDG_RUNTIME_DIR");
	size_t got = 0;
	ssize_t r;
	int fd;
	if (!rt) return -1;
	snprintf(a.sun_path, sizeof a.sun_path, "%s/dwl.sock", rt);
	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) return -1;
	if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0 || write(fd, "state\n", 6) < 0) { close(fd); return -1; }
	while (got + 1 < n && (r = read(fd, out + got, n - 1 - got)) > 0) got += r;
	out[got] = 0;
	close(fd);
	return 0;
}

/* integer value of "key": on a line */
static int jint(const char *line, const char *key, int *v)
{
	char pat[32];
	const char *p;
	snprintf(pat, sizeof pat, "\"%s\":", key);
	if (!(p = strstr(line, pat))) return 0;
	*v = atoi(p + strlen(pat));
	return 1;
}

static int jstrv(const char *line, const char *key, char *out, size_t n)
{
	char pat[32];
	const char *p;
	size_t i = 0;
	snprintf(pat, sizeof pat, "\"%s\":\"", key);
	if (!(p = strstr(line, pat))) return 0;
	for (p += strlen(pat); *p && *p != '"' && i + 1 < n; p++) out[i++] = *p;
	out[i] = 0;
	return 1;
}

/* walk the reply line by line; for each line call cb */
static void each_line(char *buf, void (*cb)(const char *, void *), void *arg)
{
	for (char *l = buf, *nl; l && *l; l = nl ? nl + 1 : NULL) {
		nl = strchr(l, '\n');
		if (nl) *nl = 0;
		cb(l, arg);
		if (!nl) break;
	}
}

struct sel { char name[64]; char geom[64]; int want_window; };

static void find_cb(const char *line, void *arg)
{
	struct sel *s = arg;
	int v, x, y, w, h, selected = 0;
	char nm[64];
	if (s->want_window) {
		if (jint(line, "focused", &v) && v && jint(line, "x", &x) && jint(line, "y", &y) && jint(line, "w", &w) && jint(line, "h", &h) &&
		    strstr(line, "\"appid\""))
			snprintf(s->geom, sizeof s->geom, "%d,%d %dx%d", x, y, w, h);
	} else if (strstr(line, "\"view\"") && jint(line, "selected", &selected) && selected && jstrv(line, "name", nm, sizeof nm))
		snprintf(s->name, sizeof s->name, "%s", nm);
}

static void list_cb(const char *line, void *arg)
{
	char nm[64];
	int w, h, sel = 0;
	if (strstr(line, "\"view\"") && jstrv(line, "name", nm, sizeof nm) && jint(line, "w", &w) && jint(line, "h", &h)) {
		jint(line, "selected", &sel);
		printf("%s%s\t%dx%d\n", nm, sel ? "*" : "", w, h);
	}
}

static int list_outputs(void)
{
	static char buf[65536];
	if (dwl_state(buf, sizeof buf) == 0) {
		each_line(buf, list_cb, NULL);
		return 0;
	}
	{ char *a[] = { "wlr-randr", NULL }; return run(a, NULL, 0, NULL); }
}

/* ---- paths ------------------------------------------------------------------------------------------------------- */

static void default_path(char *out, size_t n, const char *kind, const char *ext)
{
	const char *home = getenv("HOME"), *pic = getenv(strcmp(kind, "shot") ? "XDG_VIDEOS_DIR" : "XDG_PICTURES_DIR");
	char dir[512], stamp[32];
	time_t t = time(NULL);
	strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", localtime(&t));
	if (pic) snprintf(dir, sizeof dir, "%s", pic);
	else snprintf(dir, sizeof dir, "%s/%s", home ? home : ".", strcmp(kind, "shot") ? "Videos" : "Pictures");
	mkdir(dir, 0755);
	snprintf(out, n, "%s/%s-%s.%s", dir, kind, stamp, ext);
	for (int k = 2; k < 1000 && access(out, F_OK) == 0; k++)   /* two shots in one second must not overwrite each other */
		snprintf(out, n, "%s/%s-%s-%d.%s", dir, kind, stamp, k, ext);
}

/* ---- area -------------------------------------------------------------------------------------------------------- */

static int select_area(char *geom, size_t n)
{
	char *a[] = { "slurp", "-f", "%x,%y %wx%h", NULL };
	int rc;
	if (!have("slurp")) { fputs("shot: slurp is needed for area selection\n", stderr); return 1; }
	rc = run(a, geom, n, NULL);
	geom[strcspn(geom, "\n")] = 0;
	return rc || !geom[0];
}

/* the geometry/output arguments for MODE, appended to argv at *n. returns 0 on success */
static int target(const char *mode, const char *output, char **argv, int *n, char *store, size_t storen)
{
	static char buf[65536];
	if (output) {
		argv[(*n)++] = "-o";
		argv[(*n)++] = (char *)output;
		return 0;
	}
	if (!strcmp(mode, "full")) return 0;
	if (!strcmp(mode, "area")) {
		if (select_area(store, storen)) return 1;
		argv[(*n)++] = "-g";
		argv[(*n)++] = store;
		return 0;
	}
	if (!strcmp(mode, "output")) {
		struct sel s = {0};
		if (dwl_state(buf, sizeof buf) < 0) { fputs("shot: no selected screen known here; use -o NAME (shot -l lists them)\n", stderr); return 1; }
		each_line(buf, find_cb, &s);
		if (!s.name[0]) { fputs("shot: no screen found\n", stderr); return 1; }
		snprintf(store, storen, "%s", s.name);
		argv[(*n)++] = "-o";
		argv[(*n)++] = store;
		return 0;
	}
	if (!strcmp(mode, "window")) {
		struct sel s = { .want_window = 1 };
		if (dwl_state(buf, sizeof buf) < 0) { fputs("shot: window mode needs dwl's control socket\n", stderr); return 1; }
		each_line(buf, find_cb, &s);
		if (!s.geom[0]) { fputs("shot: no focused window\n", stderr); return 1; }
		snprintf(store, storen, "%s", s.geom);
		argv[(*n)++] = "-g";
		argv[(*n)++] = store;
		return 0;
	}
	fprintf(stderr, "shot: unknown mode '%s' (full, output, area, window)\n", mode);
	return 2;
}

/* ---- screenshot -------------------------------------------------------------------------------------------------- */

static int screenshot(const char *mode, const char *output, const char *file, int delay, int clip_only, int quiet)
{
	char path[600], store[128], tmp[600];
	char *argv[16];
	int n = 0, rc;
	if (!have("grim")) { fputs("shot: grim is needed (pacman -S grim)\n", stderr); return 1; }
	argv[n++] = "grim";
	if (target(mode, output, argv, &n, store, sizeof store)) return 1;   /* the area is picked BEFORE the delay, like a real screenshot tool */
	if (delay > 0) sleep(delay);
	if (file) snprintf(path, sizeof path, "%s", file);
	else if (clip_only) {
		const char *rt = getenv("XDG_RUNTIME_DIR");
		snprintf(tmp, sizeof tmp, "%s/shot-%d.png", rt ? rt : "/tmp", (int)getpid());
		snprintf(path, sizeof path, "%s", tmp);
	} else default_path(path, sizeof path, "shot", "png");
	argv[n++] = path;
	argv[n] = NULL;
	if ((rc = run(argv, NULL, 0, NULL))) {
		fprintf(stderr, "shot: grim failed (%d)\n", rc);
		if (!file) unlink(path);
		return 1;
	}
	if (!quiet && have("wl-copy")) {
		char *c[] = { "wl-copy", "--type", "image/png", NULL };
		run(c, NULL, 0, path);
	}
	if (!quiet) notify("Screenshot", clip_only ? "copied to the clipboard" : path);
	if (clip_only && !file) unlink(path);
	else puts(path);
	return 0;
}

/* ---- recording --------------------------------------------------------------------------------------------------- */

static int recording(const char *mode, const char *output, const char *file, int audio, int quiet)
{
	char sf[300], path[600], store[128], line[700];
	char *argv[20];
	int n = 0;
	FILE *f;
	snprintf(sf, sizeof sf, "%s/shot.rec", state_dir);
	if ((f = fopen(sf, "r"))) {   /* already recording: stop it */
		int pid = 0;
		line[0] = 0;
		if (fscanf(f, "%d %599[^\n]", &pid, path) < 2) path[0] = 0;
		fclose(f);
		unlink(sf);
		if (pid > 1 && kill(pid, 0) == 0) {
			kill(pid, SIGINT);   /* wf-recorder finishes the file on SIGINT */
			for (int i = 0; i < 50 && kill(pid, 0) == 0; i++) usleep(100000);
			if (!quiet) notify("Recording saved", path);
			puts(path);
			return 0;
		}
	}
	if (!have("wf-recorder")) { fputs("shot: wf-recorder is needed (pacman -S wf-recorder)\n", stderr); return 1; }
	argv[n++] = "wf-recorder";
	if (target(mode, output, argv, &n, store, sizeof store)) return 1;
	if (audio) argv[n++] = "-a";
	if (file) snprintf(path, sizeof path, "%s", file);
	else default_path(path, sizeof path, "rec", "mp4");
	argv[n++] = "-f";
	argv[n++] = path;
	argv[n] = NULL;
	{
		pid_t pid = fork();
		if (pid < 0) return 1;
		if (pid == 0) {
			setsid();
			int nul = open("/dev/null", O_RDWR);
			if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
			execvp(argv[0], argv);
			_exit(127);
		}
		if (!(f = fopen(sf, "w"))) { kill(pid, SIGINT); return 1; }
		fprintf(f, "%d %s\n", (int)pid, path);
		fclose(f);
	}
	if (!quiet) notify("Recording started", "run `shot rec` again to stop");
	fprintf(stderr, "recording to %s - run `shot rec` again to stop\n", path);
	return 0;
}

int main(int argc, char **argv)
{
	const char *mode = "full", *output = NULL, *file = NULL, *rt = getenv("XDG_RUNTIME_DIR");
	int delay = 0, clip_only = 0, quiet = 0, audio = 0, rec = 0, i = 1;
	snprintf(state_dir, sizeof state_dir, "%s", rt ? rt : "/tmp");
	if (i < argc && !strcmp(argv[i], "rec")) { rec = 1; i++; }
	else if (i < argc && !strcmp(argv[i], "sel")) {
		char geom[64];
		if (select_area(geom, sizeof geom)) return 1;
		puts(geom);
		return 0;
	}
	for (; i < argc; i++) {
		const char *a = argv[i];
		if (!strcmp(a, "-l")) return list_outputs();
		else if (!strcmp(a, "-o") && i + 1 < argc) { output = argv[++i]; mode = "output"; }
		else if (!strcmp(a, "-f") && i + 1 < argc) file = argv[++i];
		else if (!strcmp(a, "-d") && i + 1 < argc) delay = atoi(argv[++i]);
		else if (!strcmp(a, "-c")) clip_only = 1;
		else if (!strcmp(a, "-n")) quiet = 1;
		else if (!strcmp(a, "-a")) audio = 1;
		else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
			puts("usage: shot [-o SCREEN] [-f FILE] [-d SEC] [-c] [-n] [full|output|area|window]\n"
			     "       shot rec [-o SCREEN] [-f FILE] [-a] [full|output|area|window]   (again = stop)\n"
			     "       shot sel   |   shot -l");
			return 0;
		} else if (a[0] != '-') mode = a;
		else { fprintf(stderr, "shot: unknown option %s\n", a); return 2; }
	}
	return rec ? recording(mode, output, file, audio, quiet) : screenshot(mode, output, file, delay, clip_only, quiet);
}
