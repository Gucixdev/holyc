/* scenario - keyboard and mouse macro recorder and player, one C file, no evemu, no cnee, no shell.
 *
 *   scenario record [NAME] [DEVICE...]   start recording (in the background); one recorder per DEVICE, default: every
 *                                        keyboard and mouse in /proc/bus/input/devices
 *   scenario stop                        stop the recorders of the last `record`
 *   scenario play [-s FACTOR] [-n TIMES] NAME   replay it (FACTOR 2 = twice as fast; TIMES 0 = until Ctrl-C)
 *   scenario list | delete NAME | rename OLD NEW | show NAME
 *
 * Recordings live in $XDG_DATA_HOME/scenario (default ~/.local/share/scenario, mode 0700), one file NAME.N.evemu per device, in
 * the text format of evemu (N: I: P: B: A: header, then "E: sec.usec type code value" lines), so the files are readable and
 * greppable. Times are relative to the start of the recording, which keeps several devices in step when they play together.
 * Playback makes a virtual device (uinput) with the same capabilities as the recorded one and sends the events into it.
 * Reading /dev/input and writing /dev/uinput needs root: the command re-runs itself through sudo when it is not root.
 * NOTE: a recording of a keyboard contains every key you typed, passwords included. Do not record those, and delete it after.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static char dir[512];
static volatile sig_atomic_t stop_flag;
static void on_stop(int s) { (void)s; stop_flag = 1; }

static int die(const char *msg, const char *arg)
{
	fprintf(stderr, "scenario: %s%s%s\n", msg, arg ? ": " : "", arg ? arg : "");
	return 1;
}

/* not root: run the same command line again under sudo, with the data dir pinned so HOME does not matter */
static void need_root(int argc, char **argv)
{
	char *args[64];
	int n = 0;
	if (geteuid() == 0) return;
	setenv("SCENARIO_DIR", dir, 1);
	args[n++] = "sudo";
	args[n++] = "-E";
	for (int i = 0; i < argc && n < 62; i++) args[n++] = argv[i];
	args[n] = NULL;
	execvp("sudo", args);
	_exit(die("root is needed (sudo not found)", NULL));
}

static int valid_name(const char *s)
{
	if (!*s || *s == '.' || strlen(s) > 100) return 0;
	for (; *s; s++) if (*s == '/' || *s == ' ' || *s == '*' || *s == '?') return 0;
	return 1;
}

static void path_of(char *out, size_t n, const char *name, int idx)
{
	if (idx < 0) snprintf(out, n, "%s/%s", dir, name);
	else snprintf(out, n, "%s/%s.%d.evemu", dir, name, idx);
}

static double now_real(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ---- devices ----------------------------------------------------------------------------------------------------- */

/* eventN of every device whose handlers include kbd or mouse; virtual devices made by scenario itself are skipped */
static int detect(char devs[][64], int max)
{
	FILE *f = fopen("/proc/bus/input/devices", "r");
	char line[1024], name[256] = "";
	int n = 0;
	if (!f) return 0;
	while (fgets(line, sizeof line, f)) {
		if (!strncmp(line, "N: Name=", 8)) snprintf(name, sizeof name, "%s", line + 8);
		else if (!strncmp(line, "H: Handlers=", 12) && (strstr(line, "kbd") || strstr(line, "mouse")) && !strstr(name, "scenario-")) {
			char *e = strstr(line, "event");
			if (e && n < max) snprintf(devs[n++], 64, "/dev/input/event%d", atoi(e + 5));
		}
	}
	fclose(f);
	return n;
}

/* ---- recording --------------------------------------------------------------------------------------------------- */

static void write_header(FILE *f, int fd, double start)
{
	char name[256] = "";
	struct input_id id = {0};
	unsigned char bits[KEY_MAX / 8 + 1];
	ioctl(fd, EVIOCGNAME(sizeof name), name);
	ioctl(fd, EVIOCGID, &id);
	fprintf(f, "# EVEMU 1.3\n# scenario recording; times are relative to the start\n# start %.6f\nN: %s\n", start, name);
	fprintf(f, "I: %04x %04x %04x %04x\n", id.bustype, id.vendor, id.product, id.version);
	memset(bits, 0, sizeof bits);
	ioctl(fd, EVIOCGPROP(sizeof bits), bits);
	fputs("P:", f);
	for (int i = 0; i < 8; i++) fprintf(f, " %02x", bits[i]);
	fputc('\n', f);
	unsigned char ev[EV_MAX / 8 + 1] = {0};
	ioctl(fd, EVIOCGBIT(0, sizeof ev), ev);
	for (int t = 0; t <= EV_MAX; t++) {
		int last = -1, size = (t == EV_KEY ? KEY_MAX : t == EV_REL ? REL_MAX : t == EV_ABS ? ABS_MAX : t == EV_MSC ? MSC_MAX :
		                       t == EV_SW ? SW_MAX : t == EV_LED ? LED_MAX : 0) / 8 + 1;
		if (t == 0 || !(ev[t / 8] & (1 << (t % 8))) || t == EV_SYN || t == EV_REP || t == EV_FF || t == EV_PWR || t == EV_FF_STATUS) continue;
		memset(bits, 0, sizeof bits);
		if (ioctl(fd, EVIOCGBIT(t, size), bits) < 0) continue;
		for (int i = 0; i < size; i++) if (bits[i]) last = i;
		for (int row = 0; row * 8 <= last; row++) {
			fprintf(f, "B: %02x", t);
			for (int i = 0; i < 8; i++) fprintf(f, " %02x", row * 8 + i < size ? bits[row * 8 + i] : 0);
			fputc('\n', f);
		}
		if (t == EV_ABS)
			for (int c = 0; c <= ABS_MAX; c++) {
				struct input_absinfo ai;
				if ((bits[c / 8] & (1 << (c % 8))) && ioctl(fd, EVIOCGABS(c), &ai) == 0)
					fprintf(f, "A: %02x %d %d %d %d %d\n", c, ai.minimum, ai.maximum, ai.fuzz, ai.flat, ai.resolution);
			}
	}
	fputs("################################\n#      Waiting for events      #\n################################\n", f);
}

static int record_one(const char *dev, const char *file)
{
	int fd = open(dev, O_RDONLY | O_CLOEXEC);
	FILE *f;
	double start;
	struct pollfd p = { .fd = fd, .events = POLLIN };
	if (fd < 0) return die("cannot open", dev);
	if (!(f = fopen(file, "w"))) return die("cannot write", file);
	chmod(file, 0600);
	{ const char *u = getenv("SUDO_UID"), *g = getenv("SUDO_GID"); if (u && g && chown(file, atoi(u), atoi(g))) {} }
	start = now_real();
	write_header(f, fd, start);
	fflush(f);
	while (!stop_flag) {
		struct input_event e;
		if (poll(&p, 1, 300) <= 0) continue;
		if (read(fd, &e, sizeof e) != sizeof e) break;
		double t = e.time.tv_sec + e.time.tv_usec / 1e6 - start;
		fprintf(f, "E: %.6f %04x %04x %d\n", t < 0 ? 0 : t, e.type, e.code, e.value);
		if (e.type == EV_SYN) fflush(f);
	}
	fclose(f);
	close(fd);
	return 0;
}

static int cmd_record(int argc, char **argv)
{
	char name[128], devs[16][64], pf[600], file[600];
	int nd = 0, i = 2;
	FILE *pids;
	time_t t = time(NULL);
	if (i < argc && strncmp(argv[i], "/dev/", 5)) {
		if (!valid_name(argv[i])) return die("bad name", argv[i]);
		snprintf(name, sizeof name, "%s", argv[i++]);
	} else strftime(name, sizeof name, "%Y%m%d_%H%M%S", localtime(&t));
	for (; i < argc && nd < 16; i++) snprintf(devs[nd++], 64, "%s", argv[i]);
	if (!nd) nd = detect(devs, 16);
	if (!nd) return die("no keyboard or mouse found", NULL);
	snprintf(pf, sizeof pf, "%s/.recording", dir);
	if (!(pids = fopen(pf, "w"))) return die("cannot write", pf);
	fprintf(pids, "%s\n", name);
	for (int d = 0; d < nd; d++) {
		pid_t pid;
		path_of(file, sizeof file, name, d);
		fflush(NULL);
		if ((pid = fork()) == 0) {
			setsid();
			signal(SIGTERM, on_stop);
			signal(SIGINT, on_stop);
			_exit(record_one(devs[d], file));
		}
		fprintf(pids, "%d\n", (int)pid);
		printf("recording %s -> %s\n", devs[d], file);
	}
	fclose(pids);
	printf("scenario '%s' is recording; `scenario stop` ends it\n", name);
	return 0;
}

static int cmd_stop(void)
{
	char pf[600], line[64], name[128] = "";
	FILE *f;
	int n = 0;
	snprintf(pf, sizeof pf, "%s/.recording", dir);
	if (!(f = fopen(pf, "r"))) return die("nothing is recording", NULL);
	if (fgets(name, sizeof name, f)) name[strcspn(name, "\n")] = 0;
	while (fgets(line, sizeof line, f)) {
		int pid = atoi(line);
		if (pid > 1 && kill(pid, SIGTERM) == 0) n++;
	}
	fclose(f);
	usleep(500000);
	unlink(pf);
	printf("stopped %d recorder(s) of '%s'\n", n, name);
	return 0;
}

/* ---- playback ---------------------------------------------------------------------------------------------------- */

struct ev { double t; unsigned short type, code; int value; };

static int play_file(const char *file, double speed, int times)
{
	FILE *f = fopen(file, "r");
	char line[512], name[128] = "scenario";
	struct uinput_setup us = {0};
	struct ev *evs = NULL;
	size_t n = 0, cap = 0;
	unsigned char bits[16][KEY_MAX / 8 + 1];
	struct { int code, mn, mx, fz, fl, rs; } abs_[ABS_MAX + 1];
	int nabs = 0, fd;
	unsigned prop = 0;
	if (!f) return die("cannot read", file);
	memset(bits, 0, sizeof bits);
	while (fgets(line, sizeof line, f)) {
		if (line[0] == 'N' && line[1] == ':') { snprintf(name, sizeof name, "%s", line + 3); name[strcspn(name, "\n")] = 0; }
		else if (line[0] == 'I' && line[1] == ':') {
			unsigned a, b, c, d;
			if (sscanf(line + 2, "%x %x %x %x", &a, &b, &c, &d) == 4) { us.id.bustype = a; us.id.vendor = b; us.id.product = c; us.id.version = d; }
		} else if (line[0] == 'P' && line[1] == ':') sscanf(line + 2, "%x", &prop);
		else if (line[0] == 'B' && line[1] == ':') {
			static int rowc[16];
			unsigned t, v[8];
			if (sscanf(line + 2, "%x %x %x %x %x %x %x %x %x", &t, v, v + 1, v + 2, v + 3, v + 4, v + 5, v + 6, v + 7) == 9 && t < 16) {
				for (int i = 0; i < 8; i++) if (rowc[t] * 8 + i < (int)sizeof bits[0]) bits[t][rowc[t] * 8 + i] = v[i];
				rowc[t]++;
			}
		} else if (line[0] == 'A' && line[1] == ':') {
			unsigned c;
			int mn, mx, fz, fl, rs;
			if (sscanf(line + 2, "%x %d %d %d %d %d", &c, &mn, &mx, &fz, &fl, &rs) == 6 && nabs <= ABS_MAX)
				abs_[nabs++] = (typeof(abs_[0])){ c, mn, mx, fz, fl, rs };
		} else if (line[0] == 'E' && line[1] == ':') {
			double t;
			unsigned ty, co;
			int v;
			if (sscanf(line + 2, "%lf %x %x %d", &t, &ty, &co, &v) == 4) {
				if (n == cap && !(evs = realloc(evs, (cap = cap ? cap * 2 : 4096) * sizeof *evs))) return die("out of memory", NULL);
				evs[n++] = (struct ev){ t, ty, co, v };
			}
		}
	}
	fclose(f);
	if ((fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC)) < 0) return die("cannot open /dev/uinput", strerror(errno));
	/* the virtual device: the same event types and codes as the recorded one */
	for (int t = 1; t < 16; t++) {
		int any = 0, req = t == EV_KEY ? UI_SET_KEYBIT : t == EV_REL ? UI_SET_RELBIT : t == EV_ABS ? UI_SET_ABSBIT :
		          t == EV_MSC ? UI_SET_MSCBIT : t == EV_SW ? UI_SET_SWBIT : t == EV_LED ? UI_SET_LEDBIT : 0;
		for (int i = 0; req && i < (int)sizeof bits[0]; i++)
			for (int b = 0; b < 8; b++)
				if (bits[t][i] & (1 << b)) { ioctl(fd, UI_SET_EVBIT, t); ioctl(fd, req, i * 8 + b); any = 1; }
		(void)any;
	}
	for (int i = 0; i < 8; i++) if (prop & (1u << i)) ioctl(fd, UI_SET_PROPBIT, i);
	snprintf(us.name, UINPUT_MAX_NAME_SIZE, "scenario-%s", name);
	ioctl(fd, UI_DEV_SETUP, &us);
	for (int i = 0; i < nabs; i++) {
		struct uinput_abs_setup as = { .code = abs_[i].code, .absinfo = { .minimum = abs_[i].mn, .maximum = abs_[i].mx,
			.fuzz = abs_[i].fz, .flat = abs_[i].fl, .resolution = abs_[i].rs } };
		ioctl(fd, UI_ABS_SETUP, &as);
	}
	if (ioctl(fd, UI_DEV_CREATE) < 0) return die("cannot create the virtual device", strerror(errno));
	usleep(400000);   /* let the compositor notice the new device before the first event */
	signal(SIGINT, on_stop);
	signal(SIGTERM, on_stop);
	for (int round = 0; (times == 0 || round < times) && !stop_flag; round++) {
		struct timespec t0;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		for (size_t i = 0; i < n && !stop_flag; i++) {
			struct input_event e = {0};
			double at = evs[i].t / speed;
			struct timespec tgt = { t0.tv_sec + (time_t)at, t0.tv_nsec + (long)((at - (time_t)at) * 1e9) };
			if (tgt.tv_nsec >= 1000000000L) { tgt.tv_sec++; tgt.tv_nsec -= 1000000000L; }
			clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &tgt, NULL);
			e.type = evs[i].type; e.code = evs[i].code; e.value = evs[i].value;
			if (write(fd, &e, sizeof e) < 0) break;
		}
	}
	ioctl(fd, UI_DEV_DESTROY);
	close(fd);
	free(evs);
	return 0;
}

static int files_of(const char *name, char out[][600], int max)
{
	int n = 0;
	for (int i = 0; i < max; i++) {
		path_of(out[n], 600, name, i);
		if (access(out[n], R_OK) == 0) n++;
	}
	return n;
}

static int cmd_play(int argc, char **argv)
{
	double speed = 1;
	int times = 1, i = 2, nf, rc = 0;
	static char files[16][600];
	for (; i < argc && argv[i][0] == '-'; i++) {
		if (!strcmp(argv[i], "-s") && i + 1 < argc) speed = atof(argv[++i]);
		else if (!strcmp(argv[i], "-n") && i + 1 < argc) times = atoi(argv[++i]);
		else return die("unknown option", argv[i]);
	}
	if (i >= argc || !valid_name(argv[i])) return die("usage: scenario play [-s FACTOR] [-n TIMES] NAME", NULL);
	if (speed <= 0) return die("speed must be positive", NULL);
	if (!(nf = files_of(argv[i], files, 16))) return die("no recording named", argv[i]);
	for (int k = 0; k < nf; k++) {
		fflush(NULL);
		if (fork() == 0) _exit(play_file(files[k], speed, times));
	}
	for (int st; wait(&st) > 0;) rc |= WIFEXITED(st) ? WEXITSTATUS(st) : 1;
	return rc;
}

/* ---- housekeeping ------------------------------------------------------------------------------------------------ */

static int cmp(const void *a, const void *b) { return strcmp(a, b); }

static int cmd_list(void)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	static char names[512][128];
	int n = 0;
	if (!d) { puts("(no recordings yet)"); return 0; }
	while ((e = readdir(d)) && n < 512) {
		char *dot;
		if (e->d_name[0] == '.' || !(dot = strstr(e->d_name, ".evemu"))) continue;
		snprintf(names[n], sizeof names[n], "%.*s", (int)(dot - e->d_name), e->d_name);
		if ((dot = strrchr(names[n], '.'))) *dot = 0;
		n++;
	}
	closedir(d);
	qsort(names, n, sizeof names[0], cmp);
	for (int i = 0; i < n; i++) {
		if (i && !strcmp(names[i], names[i - 1])) continue;
		static char fs[16][600];
		int nf = files_of(names[i], fs, 16);
		long events = 0;
		for (int k = 0; k < nf; k++) {
			FILE *f = fopen(fs[k], "r");
			char line[256];
			while (f && fgets(line, sizeof line, f)) events += line[0] == 'E';
			if (f) fclose(f);
		}
		printf("%-30s %d device(s) %ld events\n", names[i], nf, events);
	}
	return 0;
}

static int cmd_delete(const char *name)
{
	static char fs[16][600];
	int nf;
	if (!valid_name(name)) return die("bad name", name);
	if (!(nf = files_of(name, fs, 16))) return die("no recording named", name);
	for (int i = 0; i < nf; i++) unlink(fs[i]);
	printf("deleted '%s' (%d file(s))\n", name, nf);
	return 0;
}

static int cmd_rename(const char *from, const char *to)
{
	static char fs[16][600];
	int nf;
	if (!valid_name(from) || !valid_name(to)) return die("bad name", NULL);
	if (!(nf = files_of(from, fs, 16))) return die("no recording named", from);
	{ static char t[16][600]; if (files_of(to, t, 16)) return die("already exists", to); }
	for (int i = 0; i < nf; i++) { char dst[600]; path_of(dst, sizeof dst, to, i); if (rename(fs[i], dst)) return die("rename failed", strerror(errno)); }
	return 0;
}

static int cmd_show(const char *name)
{
	static char fs[16][600];
	int nf = valid_name(name) ? files_of(name, fs, 16) : 0;
	if (!nf) return die("no recording named", name);
	for (int i = 0; i < nf; i++) {
		FILE *f = fopen(fs[i], "r");
		char line[256], dev[256] = "?";
		long ev = 0;
		double last = 0;
		while (f && fgets(line, sizeof line, f)) {
			if (line[0] == 'N') { snprintf(dev, sizeof dev, "%s", line + 3); dev[strcspn(dev, "\n")] = 0; }
			if (line[0] == 'E') { ev++; sscanf(line + 2, "%lf", &last); }
		}
		if (f) fclose(f);
		printf("%s: %s, %ld events, %.1f s\n", fs[i], dev, ev, last);
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *env = getenv("SCENARIO_DIR"), *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
	const char *c = argc > 1 ? argv[1] : "";
	if (env && *env) snprintf(dir, sizeof dir, "%s", env);
	else if (xdg && *xdg) snprintf(dir, sizeof dir, "%s/scenario", xdg);
	else snprintf(dir, sizeof dir, "%s/.local/share/scenario", home ? home : ".");
	if (!strcmp(c, "-h") || !strcmp(c, "--help") || !*c) {
		puts("usage: scenario record [NAME] [DEVICE...] | stop | play [-s FACTOR] [-n TIMES] NAME | list | show NAME | delete NAME | rename OLD NEW");
		return *c ? 0 : (cmd_list(), 0);
	}
	{
		char parent[512];
		snprintf(parent, sizeof parent, "%s", dir);
		for (char *s = parent + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(parent, 0755); *s = '/'; }
		mkdir(dir, 0700);
	}
	if (!strcmp(c, "record")) { need_root(argc, argv); return cmd_record(argc, argv); }
	if (!strcmp(c, "stop")) { need_root(argc, argv); return cmd_stop(); }
	if (!strcmp(c, "play")) { need_root(argc, argv); return cmd_play(argc, argv); }
	if (!strcmp(c, "list")) return cmd_list();
	if (!strcmp(c, "show") && argc > 2) return cmd_show(argv[2]);
	if (!strcmp(c, "delete") && argc > 2) return cmd_delete(argv[2]);
	if (!strcmp(c, "rename") && argc > 3) return cmd_rename(argv[2], argv[3]);
	return die("unknown command (scenario --help)", c);
}
