/* autostart - the ONE place that says what starts with the desktop.
 *   desktop autostart          run the list (the session does this)
 *   desktop autostart -l       print the list in the order it would run, start nothing
 * List: ~/.config/desktop/autostart (a default is written on first run). One entry per line:
 *     PRIORITY[,flag...] COMMAND          # comment
 * Lower PRIORITY starts first; entries with the same number start together. Flags:
 *     wait      the next priority group starts only after this entry has exited (env setup, dbus-update-environment, ...)
 *     respawn   restart when it dies (at most 5 restarts per minute, then given up and logged)
 *     delay=N   start N seconds late (heavy things that need not slow the first frame)
 * COMMAND goes through /bin/sh -c. Output goes to ~/.cache/autostart.log. Emacs is deliberately not on the default list. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAXE 128
struct entry { int prio, wait, respawn, delay; char *cmd; pid_t pid; int restarts; time_t window; };
static struct entry e[MAXE];
static int ne;

static const char *DEFAULT =
"# desktop autostart list.  PRIORITY[,wait|respawn|delay=N] COMMAND   (lower priority first, equal ones together)\n"
"# 10  = environment / services everything else needs\n"
"# 20  = daemons (clipboard history, notifications, polkit)\n"
"# 30  = nice-to-have, delayed so the first frame is not slowed\n"
"10,wait dbus-update-activation-environment --systemd WAYLAND_DISPLAY XDG_CURRENT_DESKTOP\n"
"20,respawn wl-paste --type text --watch cliphist store\n"
"20,respawn wl-paste --type image --watch cliphist store\n"
"20,respawn dunst\n"
"20 kbnotify\n"
"20,respawn /usr/lib/xfce-polkit/xfce-polkit\n"
"# Emacs is NOT started here on purpose - start it by hand.\n";

static char *path(char *buf, size_t n, const char *rel)
{
	const char *h = getenv("HOME");
	snprintf(buf, n, "%s/%s", h ? h : "", rel);
	return buf;
}

static int cmp(const void *a, const void *b)
{
	const struct entry *x = a, *y = b;
	return x->prio != y->prio ? (x->prio > y->prio) - (x->prio < y->prio) : (int)(x - y); /* qsort is not stable: tie by index */
}

static int load(void)
{
	char p[512], line[1024];
	FILE *f;
	path(p, sizeof p, ".config/desktop/autostart");
	if (!(f = fopen(p, "r"))) {
		char d[512];
		path(d, sizeof d, ".config/desktop");
		mkdir(d, 0755);
		if ((f = fopen(p, "w"))) { fputs(DEFAULT, f); fclose(f); }
		if (!(f = fopen(p, "r"))) { fprintf(stderr, "autostart: cannot read %s\n", p); return -1; }
	}
	while (ne < MAXE && fgets(line, sizeof line, f)) {
		char *s = line, *c, *flags;
		struct entry *x = &e[ne];
		while (*s == ' ' || *s == '\t') s++;
		if (!*s || *s == '#' || *s == '\n') continue;
		line[strcspn(line, "\n")] = 0;
		x->prio = (int)strtol(s, &c, 10);
		if (c == s) { fprintf(stderr, "autostart: bad line (no priority): %s\n", s); continue; }
		if (*c == ',') {
			flags = c + 1;
			while (*c && *c != ' ' && *c != '\t') c++;
			if (*c) *c++ = 0;
			for (char *t = strtok(flags, ","); t; t = strtok(NULL, ",")) {
				if (!strcmp(t, "wait")) x->wait = 1;
				else if (!strcmp(t, "respawn")) x->respawn = 1;
				else if (!strncmp(t, "delay=", 6)) x->delay = atoi(t + 6);
				else fprintf(stderr, "autostart: unknown flag '%s'\n", t);
			}
		}
		while (*c == ' ' || *c == '\t') c++;
		if (!*c) continue;
		x->cmd = strdup(c);
		ne++;
	}
	fclose(f);
	qsort(e, ne, sizeof *e, cmp);
	return 0;
}

static void start(struct entry *x)
{
	pid_t p = fork();
	if (p == 0) {
		setsid();
		if (x->delay) sleep(x->delay);
		execl("/bin/sh", "sh", "-c", x->cmd, (char *)NULL);
		_exit(127);
	}
	x->pid = p;
}

int main(int argc, char **argv)
{
	char lp[512];
	int listonly = argc > 1 && !strcmp(argv[1], "-l");
	if (load()) return 1;
	if (listonly) {
		for (int i = 0; i < ne; i++)
			printf("%3d%s%s%s  %s\n", e[i].prio, e[i].wait ? " wait" : "", e[i].respawn ? " respawn" : "",
			       e[i].delay ? " delayed" : "", e[i].cmd);
		return 0;
	}
	path(lp, sizeof lp, ".cache/autostart.log");
	if (freopen(lp, "a", stderr)) setvbuf(stderr, NULL, _IOLBF, 0);
	if (freopen("/dev/null", "r", stdin)) {}
	{ FILE *o = freopen(lp, "a", stdout); (void)o; }
	signal(SIGPIPE, SIG_IGN);

	for (int i = 0; i < ne;) {
		int j = i, waits = 0;
		for (; j < ne && e[j].prio == e[i].prio; j++) { start(&e[j]); if (e[j].wait) waits++; }
		for (int k = i; k < j && waits; k++)
			if (e[k].wait) { waitpid(e[k].pid, NULL, 0); e[k].pid = 0; waits--; }
		i = j;
	}
	/* supervise: reap everything, restart the respawn ones (rate limited); leave when nothing is left to watch */
	for (;;) {
		int live = 0, st;
		pid_t p = waitpid(-1, &st, 0);
		if (p < 0 && errno == ECHILD) break;
		if (p < 0) continue;
		for (int i = 0; i < ne; i++) {
			if (e[i].pid != p) continue;
			e[i].pid = 0;
			if (!e[i].respawn) break;
			time_t now = time(NULL);
			if (now - e[i].window > 60) { e[i].window = now; e[i].restarts = 0; }
			if (++e[i].restarts > 5) { fprintf(stderr, "autostart: giving up on '%s' (dies too fast)\n", e[i].cmd); break; }
			start(&e[i]);
		}
		for (int i = 0; i < ne; i++) live += e[i].pid > 0;
		if (!live) break;
	}
	return 0;
}
