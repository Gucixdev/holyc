/* afkd - zero-polling AFK daemon. Sleeps in poll() on the Wayland fd; the
 * compositor (ext-idle-notify-v1) wakes it only at the configured timeouts.
 * Config: ~/.config/afkd.conf   (see README)
 *   idle   SECONDS SCRIPT   run SCRIPT after SECONDS without input
 *   resume SECONDS SCRIPT   run SCRIPT on activity, if the longest idle
 *                           stage reached was >= SECONDS (0 = always)
 * Usage: afkd [-c CONF] [-s SCRIPTDIR] | --check | --once SCRIPT */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-client.h>
#include "ext-idle-notify-v1-client-protocol.h"

#define MAXR 32
typedef struct { int is_idle; long sec; char name[NAME_MAX + 1]; } Rule;
static Rule rules[MAXR];
static int nrules;
static char confpath[PATH_MAX], scriptdir[PATH_MAX];
static long max_fired; /* longest idle stage reached since last activity */

static void die(const char *m) { fprintf(stderr, "afkd: %s\n", m); exit(1); }

static int name_ok(const char *n)
{
	return *n && !strchr(n, '/') && strcmp(n, ".") && strcmp(n, "..");
}

/* returns 0 ok; prints each problem; checks script existence too */
static int load_conf(int verbose)
{
	FILE *f = fopen(confpath, "r");
	char line[512], kind[16], name[NAME_MAX + 64];
	long sec; int bad = 0, ln = 0;
	if (!f) { fprintf(stderr, "afkd: cannot open %s: %s\n", confpath, strerror(errno)); return -1; }
	nrules = 0;
	while (fgets(line, sizeof line, f)) {
		char *p = line; ln++;
		while (*p == ' ' || *p == '\t') p++;
		if (*p == '#' || *p == '\n' || !*p) continue;
		if (sscanf(p, "%15s %ld %255s", kind, &sec, name) != 3 ||
		    (strcmp(kind, "idle") && strcmp(kind, "resume")) ||
		    sec < 0 || (!strcmp(kind, "idle") && sec == 0) || !name_ok(name)) {
			fprintf(stderr, "%s:%d: bad line: %s", confpath, ln, line); bad = 1; continue;
		}
		char path[PATH_MAX + 8];
		snprintf(path, sizeof path, "%s/%s", scriptdir, name);
		if (access(path, R_OK)) { fprintf(stderr, "%s:%d: no script %s\n", confpath, ln, path); bad = 1; }
		if (nrules == MAXR) { fprintf(stderr, "%s:%d: too many rules (max %d)\n", confpath, ln, MAXR); bad = 1; break; }
		rules[nrules].is_idle = !strcmp(kind, "idle");
		rules[nrules].sec = sec;
		snprintf(rules[nrules].name, sizeof rules[nrules].name, "%s", name);
		nrules++;
	}
	fclose(f);
	if (!nrules && !bad) { fprintf(stderr, "afkd: %s has no rules\n", confpath); bad = 1; }
	if (verbose && !bad) printf("ok: %d rules\n", nrules);
	return bad ? -1 : 0;
}

/* fork+exec, never blocks. Child holds an flock on a per-script lock for as
 * long as the script lives (fd inherited across exec): one instance each.
 * SIGCHLD is SA_NOCLDWAIT so children are reaped by the kernel, no wait(). */
static int run_script(const char *name, int wait_it)
{
	const char *rd = getenv("XDG_RUNTIME_DIR");
	char lock[PATH_MAX], path[PATH_MAX + 8];
	pid_t pid;
	snprintf(path, sizeof path, "%s/%s", scriptdir, name);
	snprintf(lock, sizeof lock, "%s/afkd-%s.lock", rd ? rd : "/tmp", name);
	if (access(path, R_OK)) { fprintf(stderr, "afkd: no script %s\n", path); return 127; }
	pid = fork();
	if (pid < 0) return -1;
	if (pid == 0) {
		int fd = open(lock, O_RDWR | O_CREAT, 0600);
		if (fd >= 0 && flock(fd, LOCK_EX | LOCK_NB)) {
			fprintf(stderr, "afkd: %s already running, skipped\n", name);
			_exit(0);
		}
		setsid();
		signal(SIGCHLD, SIG_DFL);
		if (access(path, X_OK)) execl("/bin/sh", "sh", path, (char *)0);
		else execl(path, name, (char *)0);
		_exit(127);
	}
	fprintf(stderr, "afkd: run %s\n", name);
	if (wait_it) { int st; signal(SIGCHLD, SIG_DFL); waitpid(pid, &st, 0); return st; }
	return 0;
}

static void idle_idled(void *d, struct ext_idle_notification_v1 *n)
{
	Rule *r = d; (void)n;
	if (r->sec > max_fired) max_fired = r->sec;
	run_script(r->name, 0);
}
static void idle_resumed(void *d, struct ext_idle_notification_v1 *n)
{
	(void)d; (void)n;
	for (int i = 0; i < nrules; i++)
		if (!rules[i].is_idle && rules[i].sec <= max_fired) run_script(rules[i].name, 0);
	max_fired = 0;
}
static const struct ext_idle_notification_v1_listener idle_l = { idle_idled, idle_resumed };

static struct wl_seat *seat;
static struct ext_idle_notifier_v1 *notifier;
static void reg_add(void *d, struct wl_registry *r, uint32_t id, const char *i, uint32_t v)
{
	(void)d;
	if (!strcmp(i, "wl_seat") && !seat) seat = wl_registry_bind(r, id, &wl_seat_interface, 1);
	else if (!strcmp(i, "ext_idle_notifier_v1"))
		notifier = wl_registry_bind(r, id, &ext_idle_notifier_v1_interface, v < 1 ? v : 1);
}
static void reg_del(void *d, struct wl_registry *r, uint32_t id) { (void)d; (void)r; (void)id; }
static const struct wl_registry_listener reg_l = { reg_add, reg_del };

static int daemon_main(void)
{
	struct wl_display *dpy = wl_display_connect(NULL);
	if (!dpy) die("cannot connect to Wayland (WAYLAND_DISPLAY unset?). X11-only sessions are not supported");
	struct wl_registry *reg = wl_display_get_registry(dpy);
	wl_registry_add_listener(reg, &reg_l, NULL);
	wl_display_roundtrip(dpy);
	if (!notifier) die("compositor does not advertise ext_idle_notifier_v1; cannot detect idle (no fallback)");
	if (!seat) die("no wl_seat");
	for (int i = 0; i < nrules; i++) {
		if (!rules[i].is_idle) continue;
		struct ext_idle_notification_v1 *n =
			ext_idle_notifier_v1_get_idle_notification(notifier, (uint32_t)(rules[i].sec * 1000), seat);
		ext_idle_notification_v1_add_listener(n, &idle_l, &rules[i]);
	}
	wl_display_roundtrip(dpy);
	struct sigaction sa = { .sa_handler = SIG_IGN, .sa_flags = SA_NOCLDWAIT };
	sigaction(SIGCHLD, &sa, NULL);
	fprintf(stderr, "afkd: watching, %d rules\n", nrules);
	for (;;) {
		while (wl_display_prepare_read(dpy) != 0) wl_display_dispatch_pending(dpy);
		if (wl_display_flush(dpy) < 0 && errno != EAGAIN) { wl_display_cancel_read(dpy); break; }
		struct pollfd p = { wl_display_get_fd(dpy), POLLIN, 0 };
		if (poll(&p, 1, -1) < 0) { wl_display_cancel_read(dpy); if (errno == EINTR) continue; break; }
		if (wl_display_read_events(dpy) < 0) break;
		if (wl_display_dispatch_pending(dpy) < 0) break;
	}
	fprintf(stderr, "afkd: compositor connection lost, exiting\n");
	return 1;
}

int main(int argc, char **argv)
{
	const char *home = getenv("HOME"), *xdg = getenv("XDG_CONFIG_HOME"), *sd = getenv("AFKD_SCRIPTS");
	const char *once = NULL; int check = 0;
	if (xdg && *xdg) snprintf(confpath, sizeof confpath, "%s/afkd.conf", xdg);
	else snprintf(confpath, sizeof confpath, "%s/.config/afkd.conf", home ? home : "");
	if (sd) snprintf(scriptdir, sizeof scriptdir, "%s", sd);
	else {
		char exe[PATH_MAX]; ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
		if (n > 0) { exe[n] = 0; char *s = strrchr(exe, '/'); if (s) *s = 0; }
		else strcpy(exe, ".");
		snprintf(scriptdir, sizeof scriptdir, "%s/../scripts", exe);
		if (access(scriptdir, R_OK) && home) snprintf(scriptdir, sizeof scriptdir, "%s/.config/afkd/scripts", home);
	}
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-c") && i + 1 < argc) snprintf(confpath, sizeof confpath, "%s", argv[++i]);
		else if (!strcmp(argv[i], "-s") && i + 1 < argc) snprintf(scriptdir, sizeof scriptdir, "%s", argv[++i]);
		else if (!strcmp(argv[i], "--check")) check = 1;
		else if (!strcmp(argv[i], "--once") && i + 1 < argc) once = argv[++i];
		else { fprintf(stderr, "usage: afkd [-c CONF] [-s SCRIPTDIR] [--check | --once SCRIPT]\n"); return 2; }
	}
	if (once) {
		if (!name_ok(once)) die("bad script name");
		int st = run_script(once, 1);
		return st == 0 ? 0 : 1;
	}
	if (load_conf(check)) return 1;
	if (check) return 0;
	return daemon_main();
}
