/* bar - dwl's status lines in, lemonbar lines out. C, no forks in the hot path: /proc and sysfs are read with pread on
 * descriptors kept open, the GPU comes from NVML through dlopen (no nvidia-smi), only the volume still asks wpctl (every 5 s).
 * dwl -s 'desktop bar | desktop lemonbar -p -b'
 * Input per output: "<out> tags <occ> <seltags> <sel> <urg>", "<out> layout <sym>", "<out> selmon 0|1",
 *   "<out> client <tags> <floating> <focused> <fullscreen> <appid>" per window, closed by "<out> clients-end".
 * Output: one line, a %{Sn<name>} segment per output (lemonbar draws on the first monitor only without it). */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAXOUT 4
#define MAXCL 32
/* S_* are raw lemonbar text, passed as %s arguments; F_* are the same inside a printf format (every % doubled) */
#define S_FOC "%{B#458588}%{F#ebdbb2}"
#define S_DIM "%{F#928374}"
#define S_ACC "%{F#fabd2f}"
#define F_NOR "%%{B-}%%{F-}"
#define F_ACC "%%{F#fabd2f}"

struct client { unsigned tags; int floating, focused, fullscreen; char app[48]; };
struct output {
	char name[32], layout[8];
	unsigned occ, sel;
	int ncl, newcl;
	struct client cl[MAXCL], pend[MAXCL];
};
static struct output outs[MAXOUT];
static int nouts;

static struct output *get(const char *name)
{
	for (int i = 0; i < nouts; i++)
		if (!strcmp(outs[i].name, name)) return &outs[i];
	if (nouts == MAXOUT) return NULL;
	struct output *o = &outs[nouts++];
	snprintf(o->name, sizeof o->name, "%s", name);
	o->sel = 1;
	return o;
}

/* one letter per layout, no frame: s stack, t tabbed, f float, = tile, g grid */
static const char *lname(const char *s)
{
	if (!strcmp(s, "[]=")) return "=";
	if (!strcmp(s, "><>")) return "f";
	if (s[0] == '[' && s[1] && s[2] == ']') return s[1] == 'D' ? "s" : "t"; /* [D] stack, [M] tabbed */
	if (!strcmp(s, "HHH")) return "g";
	return s;
}

/* ---- stats */
static int fd_stat = -1, fd_mem = -1;
static unsigned long long prev_total, prev_idle;
static int cpu;
static long mem_mb, swap_mb;
static char gpu[32] = "-", vol[8], bat[8];
static time_t last_stats, last_slow;
static int (*nvmlInit)(void), (*nvmlDev)(unsigned, void **), (*nvmlUtil)(void *, unsigned *), (*nvmlMem)(void *, unsigned long long *), (*nvmlTemp)(void *, int, unsigned *);
static void *nvdev;

static ssize_t slurp(int *fd, const char *path, char *buf, size_t n)
{
	if (*fd < 0 && (*fd = open(path, O_RDONLY | O_CLOEXEC)) < 0) return -1;
	ssize_t r = pread(*fd, buf, n - 1, 0);
	if (r >= 0) buf[r] = 0;
	return r;
}

static void nvml_open(void)
{
	void *h = dlopen("libnvidia-ml.so.1", RTLD_NOW);
	if (!h) return;
	nvmlInit = dlsym(h, "nvmlInit_v2"); nvmlDev = dlsym(h, "nvmlDeviceGetHandleByIndex_v2");
	nvmlUtil = dlsym(h, "nvmlDeviceGetUtilizationRates"); nvmlMem = NULL; nvmlTemp = dlsym(h, "nvmlDeviceGetTemperature");
	if (!nvmlInit || !nvmlDev || !nvmlUtil || nvmlInit() || nvmlDev(0, &nvdev)) nvmlInit = NULL;
}

static void read_cmd(const char *cmd, char *out, size_t n, const char *fmt)
{
	FILE *p = popen(cmd, "r");
	double v;
	char junk[32];
	out[0] = 0;
	if (!p) return;
	if (fscanf(p, "%31s %lf", junk, &v) == 2) snprintf(out, n, fmt, (int)(v * 100));
	pclose(p);
}

static void stats(time_t now)
{
	char b[4096];
	if (now - last_stats < 2) return;
	last_stats = now;
	if (slurp(&fd_stat, "/proc/stat", b, sizeof b) > 0) {
		unsigned long long u, n, s, idle, io, irq, sirq, total;
		if (sscanf(b, "cpu %llu %llu %llu %llu %llu %llu %llu", &u, &n, &s, &idle, &io, &irq, &sirq) == 7) {
			total = u + n + s + idle + io + irq + sirq;
			if (total > prev_total)
				cpu = (int)(100 * ((total - prev_total) - (idle - prev_idle)) / (total - prev_total));
			prev_total = total; prev_idle = idle;
		}
	}
	if (slurp(&fd_mem, "/proc/meminfo", b, sizeof b) > 0) {
		long mt = 0, ma = 0, st = 0, sf = 0;
		char *p;
		if ((p = strstr(b, "MemTotal:"))) mt = atol(p + 9);
		if ((p = strstr(b, "MemAvailable:"))) ma = atol(p + 13);
		if ((p = strstr(b, "SwapTotal:"))) st = atol(p + 10);
		if ((p = strstr(b, "SwapFree:"))) sf = atol(p + 9);
		mem_mb = (mt - ma) / 1024; swap_mb = (st - sf) / 1024;
	}
	if (nvmlInit) {
		struct { unsigned gpu, mem; } u;
		unsigned t = 0;
		if (!nvmlUtil(nvdev, (unsigned *)&u)) {
			if (nvmlTemp) nvmlTemp(nvdev, 0, &t);
			snprintf(gpu, sizeof gpu, "%u%% %uC", u.gpu, t);
		}
	}
	if (now - last_slow >= 5) {   /* the only fork: volume and battery, every 5 s */
		FILE *f;
		last_slow = now;
		read_cmd("wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null", vol, sizeof vol, "%d%%");
		bat[0] = 0;
		if ((f = fopen("/sys/class/power_supply/BAT0/capacity", "r"))) { if (fgets(bat, sizeof bat, f)) bat[strcspn(bat, "\n")] = 0; fclose(f); }
	}
}

/* ---- bar.conf: which segments, in which order, and your own scripts (~/.config/desktop/bar.conf, or $DESKTOP_CONFIG_DIR)
 *   left  = tags layout windows           (default)
 *   right = outputs cpu gpu ram vol bat clock   (default; any order, leave one out to hide it)
 *   segment NAME INTERVAL COMMAND...      a script: its first line of output, re-run every INTERVAL seconds (killed after 2 s);
 *                                         then put NAME in left or right.  Output is lemonbar text, so %{F#ff0000} works.
 * No file = exactly the old bar. Read once at start. */
#define MAXSEG 16
static struct segdef { char name[24], cmd[400], out[160]; int interval; time_t last; } segs[MAXSEG];
static int nsegs;
static char leftmods[160] = "tags layout windows", rightmods[160] = "outputs cpu gpu ram vol bat clock";

static void load_conf(void)
{
	char path[600], line[600];
	const char *d = getenv("DESKTOP_CONFIG_DIR"), *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
	FILE *f;
	if (d && *d) snprintf(path, sizeof path, "%s/bar.conf", d);
	else if (x && *x) snprintf(path, sizeof path, "%s/desktop/bar.conf", x);
	else snprintf(path, sizeof path, "%s/.config/desktop/bar.conf", h ? h : "");
	if (!(f = fopen(path, "r"))) return;
	while (fgets(line, sizeof line, f)) {
		char *p = line + strspn(line, " \t");
		p[strcspn(p, "\n")] = 0;
		if (!*p || *p == '#') continue;
		if (!strncmp(p, "segment ", 8) && nsegs < MAXSEG) {
			struct segdef *s = &segs[nsegs];
			char name[24], cmd[400];
			int iv;
			if (sscanf(p + 8, "%23s %d %399[^\n]", name, &iv, cmd) == 3 && iv > 0) {
				snprintf(s->name, sizeof s->name, "%s", name); snprintf(s->cmd, sizeof s->cmd, "%s", cmd); s->interval = iv; nsegs++;
			}
		} else if (!strncmp(p, "left", 4) || !strncmp(p, "right", 5)) {
			char *eq = strchr(p, '=');
			if (eq) {
				eq++; eq += strspn(eq, " \t");
				snprintf(p[0] == 'l' ? leftmods : rightmods, 160, "%s", eq);
			}
		}
	}
	fclose(f);
}

static void run_segments(time_t now)
{
	for (int i = 0; i < nsegs; i++) {
		struct segdef *s = &segs[i];
		char cmd[520], *q = cmd;
		FILE *p;
		if (now - s->last < s->interval) continue;
		s->last = now;
		q += snprintf(q, sizeof cmd, "timeout 2 sh -c '");
		for (const char *c = s->cmd; *c && q < cmd + sizeof cmd - 12; c++) { if (*c == '\'') { memcpy(q, "'\\''", 4); q += 4; } else *q++ = *c; }
		snprintf(q, (size_t)(cmd + sizeof cmd - q), "' 2>/dev/null");
		s->out[0] = 0;
		if ((p = popen(cmd, "r"))) {
			if (fgets(s->out, sizeof s->out, p)) s->out[strcspn(s->out, "\n")] = 0;
			pclose(p);
		}
	}
}

/* ---- output */
static void seg(const struct output *o, char **p, char *end)
{
	char *w = *p;
	char tm[32], mods[160], *save = NULL, *m;
	time_t now = time(NULL);
	struct tm t;
	int first;
	localtime_r(&now, &t);
	strftime(tm, sizeof tm, "%a %d %b %H:%M", &t);
#define PR(...) w += snprintf(w, (size_t)(end - w), __VA_ARGS__)
	PR("%%{Sn%s}%%{l}", o->name);
	for (int side = 0; side < 2; side++) {
		snprintf(mods, sizeof mods, "%s", side ? rightmods : leftmods);
		if (side) PR("%%{r}");
		first = 1;
		for (m = strtok_r(mods, " \t,", &save); m; m = strtok_r(NULL, " \t,", &save)) {
			if (!strcmp(m, "tags")) {
				for (int i = 1; i <= 9; i++) {
					unsigned bit = 1u << (i - 1);
					PR("%s %d " F_NOR, (o->sel & bit) ? S_FOC : (o->occ & bit) ? S_ACC : S_DIM, i);
				}
			} else if (!strcmp(m, "layout")) PR(" " F_ACC "%s" F_NOR " ", lname(o->layout));
			else if (!strcmp(m, "windows")) {
				for (int i = 0; i < o->ncl; i++) {
					const struct client *c = &o->cl[i];
					if (!(c->tags & o->sel)) continue;
					const char *a = strrchr(c->app, '.');
					PR("%s %s%s%.*s " F_NOR, c->focused ? S_FOC : S_DIM, c->fullscreen ? "*" : "", c->floating ? "~" : "", 14, a ? a + 1 : c->app);
				}
			} else if (side) { /* right side modules are joined with " | " */
				char buf[200] = "";
				if (!strcmp(m, "outputs")) {
					for (int i = 0; i < nouts; i++) PR("%s %s " F_NOR, &outs[i] == o ? S_FOC : S_DIM, outs[i].name);
					PR(" "); first = 1; continue;
				} else if (!strcmp(m, "cpu")) snprintf(buf, sizeof buf, "cpu %d%%", cpu);
				else if (!strcmp(m, "gpu")) snprintf(buf, sizeof buf, "gpu %s", gpu);
				else if (!strcmp(m, "ram")) snprintf(buf, sizeof buf, "ram %ldM swap %ldM", mem_mb, swap_mb);
				else if (!strcmp(m, "vol")) { if (vol[0]) snprintf(buf, sizeof buf, "vol %s", vol); }
				else if (!strcmp(m, "bat")) { if (bat[0]) snprintf(buf, sizeof buf, "bat %s%%", bat); }
				else if (!strcmp(m, "clock")) snprintf(buf, sizeof buf, "%s", tm);
				else for (int i = 0; i < nsegs; i++) if (!strcmp(segs[i].name, m)) snprintf(buf, sizeof buf, "%s", segs[i].out);
				if (!buf[0]) continue;
				PR("%s%s", first ? "" : " | ", buf);
				first = 0;
			} else for (int i = 0; i < nsegs; i++)
				if (!strcmp(segs[i].name, m) && segs[i].out[0]) PR(" %s ", segs[i].out);
		}
		if (side) PR(" ");
	}
	*p = w;
}

static void render(void)
{
	static char buf[16384];
	char *p = buf;
	stats(time(NULL));
	run_segments(time(NULL));
	for (int i = 0; i < nouts; i++) seg(&outs[i], &p, buf + sizeof buf - 1);
	if (p > buf) { *p++ = '\n'; fwrite(buf, 1, (size_t)(p - buf), stdout); fflush(stdout); }
}

static void line(char *l)
{
	char *name = strtok(l, " "), *kind = strtok(NULL, " "), *rest;
	struct output *o;
	if (!name || !kind || !(o = get(name))) return;
	rest = strtok(NULL, "");
	if (!rest) rest = "";
	if (!strcmp(kind, "tags")) {
		unsigned occ, sel;
		if (sscanf(rest, "%u %u", &occ, &sel) == 2) { o->occ = occ; o->sel = sel; }
		render();
	} else if (!strcmp(kind, "layout")) {
		snprintf(o->layout, sizeof o->layout, "%s", rest);
		render();
	} else if (!strcmp(kind, "client")) {
		if (o->newcl < MAXCL) {
			struct client *c = &o->pend[o->newcl];
			char app[64] = "?";
			if (sscanf(rest, "%u %d %d %d %63s", &c->tags, &c->floating, &c->focused, &c->fullscreen, app) >= 4) {
				snprintf(c->app, sizeof c->app, "%s", app);
				o->newcl++;
			}
		}
	} else if (!strcmp(kind, "clients-end")) {
		memcpy(o->cl, o->pend, sizeof o->cl); o->ncl = o->newcl; o->newcl = 0;
	}
}

int bar_main(int argc, char **argv)
{
	static char buf[8192];
	size_t have = 0;
	struct pollfd pf = { 0, POLLIN, 0 };
	(void)argc; (void)argv;
	load_conf();
	nvml_open();
	for (;;) {
		int r = poll(&pf, 1, 2000);
		if (r == 0) { render(); continue; }   /* quiet: redraw for the stats and the clock */
		if (r < 0) continue;
		ssize_t n = read(0, buf + have, sizeof buf - have - 1);
		if (n <= 0) return 0;   /* dwl is gone */
		have += (size_t)n;
		char *nl, *s = buf;
		while ((nl = memchr(s, '\n', have - (size_t)(s - buf)))) { *nl = 0; line(s); s = nl + 1; }
		have -= (size_t)(s - buf);
		memmove(buf, s, have);
	}
}
