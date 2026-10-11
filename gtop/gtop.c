/* gtop - system monitor TUI for Linux. C99 + POSIX.1-2008, libc only.
 *
 * Started as a 1:1 port of a Rust/ratatui version (same panels, layout, numbers, keys); the process
 * tree and the PROC columns came after.
 * Reads /proc and /sys directly (no sysinfo-style library).
 *
 *   CPU    aggregate bar (user/sys/iowait), trend graph, per-core %, frequency, temperature
 *   RAM    used / zram / cache / swap / free as one segmented bar, trend graph
 *   GPU    nvidia-smi, then amdgpu sysfs, then intel_gpu_top
 *   TPU/NPU  boxes only when the hardware exists
 *   DISK   aggregate usage bar and graph, per device: usage bar, activity graph, partition tree
 *   NET    aggregate graph, per interface: rates, link speed bar or graph, addresses
 *   PROC   three views (app / raw / user+system), mark, kill, command palette, ghost rows
 *
 * keys:  q quit   left/right view   up/down move   space mark   k SIGKILL marked (or the row)
 *        /  command palette (runs commands/<name> with the marked pids as arguments)
 *        r  record all numbers to gtop-record-<time>.csv   a  anonymous mode   +/-  refresh 0.1..5 s
 *
 * gtop --snapshot [W H] [--keys KEYS] [-f SUBSTR]   one frame as plain text, no terminal needed
 *   KEYS: U D L R = up/down/left/right, E = enter, anything else literally. A refresh happens
 *   after the keys, so a view change is visible in the frame. -f keeps only processes whose
 *   name contains SUBSTR. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <ifaddrs.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ theme */
#define ANSI(n) (0x01000000u | (n)) /* terminal palette colour n, not truecolor */
#define C_PRIMARY  0x00FF41u
#define C_WARN     0xFF0000u
#define C_CAUTION  0xFFFF00u
#define C_ACCENT   0x9944FFu
#define C_NEUTRAL  C_ACCENT
#define C_DISABLED 0x785AA0u
#define C_GHOST    0x787878u
#define C_RAM_USED 0x00D9FFu
#define C_RAM_ZRAM 0xFF2EC4u
#define C_RAM_CACHE 0xFFAE00u
#define C_RAM_SWAP 0xFF3B3Bu
#define C_RAM_FREE C_ACCENT
#define C_CYAN     ANSI(6)
#define C_PLAIN    ANSI(9) /* terminal default foreground */

static unsigned tier_color(double pct) { return pct > 85.0 ? C_WARN : pct > 60.0 ? C_CAUTION : C_PRIMARY; }

/* ------------------------------------------------------------------ util */
static double now_s(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static int read_file(const char *path, char *buf, size_t cap)
{
	FILE *f = fopen(path, "r");
	size_t n;
	if (!f) return -1;
	n = fread(buf, 1, cap - 1, f);
	fclose(f);
	buf[n] = 0;
	return (int)n;
}

static char *trim(char *s)
{
	char *e;
	while (*s == ' ' || *s == '\t' || *s == '\n') s++;
	e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
	return s;
}

static void human_bytes(double v, char *out, size_t n)
{
	static const char *u[] = {"B", "KB", "MB", "GB", "TB"};
	int i = 0;
	while (v >= 1024.0 && i < 4) { v /= 1024.0; i++; }
	if (i == 0) snprintf(out, n, "%.0f%s", v, u[i]);
	else snprintf(out, n, "%.1f%s", v, u[i]);
}

static void human_rate(double v, char *out, size_t n)
{
	char b[32];
	human_bytes(v, b, sizeof b);
	snprintf(out, n, "%.24s/s", b);
}

static int which(const char *bin)
{
	const char *p = getenv("PATH");
	char buf[1024];
	struct stat st;
	while (p && *p) {
		const char *e = strchr(p, ':');
		size_t l = e ? (size_t)(e - p) : strlen(p);
		if (l && l < 700) {
			snprintf(buf, sizeof buf, "%.*s/%s", (int)l, p, bin);
			if (stat(buf, &st) == 0 && S_ISREG(st.st_mode)) return 1;
		}
		p = e ? e + 1 : NULL;
	}
	return 0;
}

#define HIST_LEN 30
typedef struct { double v[HIST_LEN]; int n; } Hist;
static void hpush(Hist *h, double x)
{
	if (h->n == HIST_LEN) { memmove(h->v, h->v + 1, (HIST_LEN - 1) * sizeof(double)); h->n--; }
	h->v[h->n++] = x;
}
static double hlast(const Hist *h) { return h->n ? h->v[h->n - 1] : 0.0; }

/* ------------------------------------------------------------------ collectors */
#define MAXCORES 256
typedef struct {
	int ncores;
	unsigned long long prev[MAXCORES + 1][7];
	int have_prev;
	double pct[MAXCORES], agg, user, sys, iow, load1;
	int freq[MAXCORES], temp[MAXCORES]; /* -1 = unknown */
	Hist core_hist[MAXCORES], agg_hist;
	char core_path[MAXCORES][128];
	char pkg_path[128];
	int paths_done;
} Cpu;

static int parse_cpu_line(const char *l, unsigned long long f[7])
{
	const char *p = strchr(l, ' ');
	return p && sscanf(p, "%llu %llu %llu %llu %llu %llu %llu", &f[0], &f[1], &f[2], &f[3], &f[4], &f[5], &f[6]) == 7;
}

static void resolve_temp_paths(Cpu *c)
{
	DIR *d = opendir("/sys/class/hwmon");
	struct dirent *e;
	char tctl[128] = "", tdie[128] = "";
	c->paths_done = 1;
	if (!d) return;
	while ((e = readdir(d))) {
		char dir[300];
		DIR *h;
		struct dirent *f;
		if (e->d_name[0] == '.') continue;
		snprintf(dir, sizeof dir, "/sys/class/hwmon/%s", e->d_name);
		h = opendir(dir);
		if (!h) continue;
		while ((f = readdir(h))) {
			char lab[128], path[400], in[400], *l;
			int num, core;
			size_t nl = strlen(f->d_name);
			if (strncmp(f->d_name, "temp", 4) || nl < 11 || strcmp(f->d_name + nl - 6, "_label")) continue;
			num = atoi(f->d_name + 4);
			snprintf(path, sizeof path, "%s/%s", dir, f->d_name);
			if (read_file(path, lab, sizeof lab) <= 0) continue;
			l = trim(lab);
			snprintf(in, sizeof in, "%s/temp%d_input", dir, num);
			if (access(in, R_OK) != 0) continue;
			if (!strncmp(l, "Core ", 5) && sscanf(l + 5, "%d", &core) == 1) {
				if (core >= 0 && core < MAXCORES) snprintf(c->core_path[core], sizeof c->core_path[core], "%.127s", in);
			} else if (!strcmp(l, "Tctl")) snprintf(tctl, sizeof tctl, "%.127s", in);
			else if (!strcmp(l, "Tdie") && !tdie[0]) snprintf(tdie, sizeof tdie, "%.127s", in);
		}
		closedir(h);
	}
	closedir(d);
	snprintf(c->pkg_path, sizeof c->pkg_path, "%s", tctl[0] ? tctl : tdie);
}

static int core_temp(Cpu *c, int i)
{
	char b[32];
	const char *p = c->core_path[i][0] ? c->core_path[i] : c->pkg_path;
	if (!p[0] || read_file(p, b, sizeof b) <= 0) return -1;
	return atoi(b) / 1000;
}

static void cpu_refresh(Cpu *c)
{
	char buf[65536], *line, *save = NULL;
	static unsigned long long cur[MAXCORES + 1][7];
	int n = 0, i;
	if (read_file("/proc/stat", buf, sizeof buf) <= 0) return;
	for (line = strtok_r(buf, "\n", &save); line && n <= MAXCORES; line = strtok_r(NULL, "\n", &save)) {
		if (strncmp(line, "cpu", 3)) break;
		if (parse_cpu_line(line, cur[n])) n++;
	}
	if (n == 0) return;
	if (c->have_prev) {
		long long tot = 0, idle, k;
		for (k = 0; k < 7; k++) tot += (long long)(cur[0][k] - c->prev[0][k]);
		idle = (long long)(cur[0][3] - c->prev[0][3]);
		if (tot > 0) {
			c->agg = (double)(tot - idle) / (double)tot * 100.0;
			c->user = (double)((long long)(cur[0][0] - c->prev[0][0]) + (long long)(cur[0][1] - c->prev[0][1])) / (double)tot * 100.0;
			c->sys = (double)((long long)(cur[0][2] - c->prev[0][2]) + (long long)(cur[0][5] - c->prev[0][5]) +
			                  (long long)(cur[0][6] - c->prev[0][6])) / (double)tot * 100.0;
			c->iow = (double)(long long)(cur[0][4] - c->prev[0][4]) / (double)tot * 100.0;
			hpush(&c->agg_hist, c->agg);
		}
		if (!c->paths_done) resolve_temp_paths(c);
		for (i = 1; i < n && i - 1 < MAXCORES; i++) {
			long long t = 0, id = (long long)(cur[i][3] - c->prev[i][3]);
			char path[96], b[32];
			for (k = 0; k < 7; k++) t += (long long)(cur[i][k] - c->prev[i][k]);
			c->pct[i - 1] = t > 0 ? (double)(t - id) / (double)t * 100.0 : 0.0;
			hpush(&c->core_hist[i - 1], c->pct[i - 1]);
			snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", i - 1);
			c->freq[i - 1] = read_file(path, b, sizeof b) > 0 ? atoi(b) / 1000 : -1;
			c->temp[i - 1] = core_temp(c, i - 1);
		}
		c->ncores = n - 1;
	}
	for (i = 0; i < n; i++) memcpy(c->prev[i], cur[i], sizeof c->prev[i]);
	c->have_prev = 1;
	if (read_file("/proc/loadavg", buf, sizeof buf) > 0) c->load1 = atof(buf);
}

typedef struct {
	double total, free_, cached, swap_total, swap_free, zram;
	int has_zram;
	Hist used_hist;
} Mem;

static void mem_refresh(Mem *m)
{
	char buf[8192], *line, *save = NULL, path[96], b[256];
	double buffers = 0, cached = 0;
	DIR *d;
	struct dirent *e;
	m->has_zram = 0;
	m->zram = 0;
	if (read_file("/proc/meminfo", buf, sizeof buf) <= 0) return;
	for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		double kb;
		if (sscanf(line, "MemTotal: %lf", &kb) == 1) m->total = kb * 1024;
		else if (sscanf(line, "MemFree: %lf", &kb) == 1) m->free_ = kb * 1024;
		else if (sscanf(line, "Buffers: %lf", &kb) == 1) buffers = kb * 1024;
		else if (sscanf(line, "Cached: %lf", &kb) == 1) cached = kb * 1024;
		else if (sscanf(line, "SwapTotal: %lf", &kb) == 1) m->swap_total = kb * 1024;
		else if (sscanf(line, "SwapFree: %lf", &kb) == 1) m->swap_free = kb * 1024;
	}
	m->cached = cached + buffers;
	if ((d = opendir("/sys/block"))) {
		while ((e = readdir(d)) && !m->has_zram) {
			unsigned long long a, bb, c;
			if (strncmp(e->d_name, "zram", 4)) continue;
			snprintf(path, sizeof path, "/sys/block/%.60s/mm_stat", e->d_name);
			if (read_file(path, b, sizeof b) > 0 && sscanf(b, "%llu %llu %llu", &a, &bb, &c) == 3) {
				m->zram = (double)c;
				m->has_zram = 1;
			}
		}
		closedir(d);
	}
	if (m->total > 0) {
		double other = m->total - m->free_ - m->cached - m->zram;
		if (other < 0) other = 0;
		hpush(&m->used_hist, other / m->total * 100.0);
	}
}

#define MAXPART 32
#define MAXDISK 24
typedef struct {
	char name[40], mount[160];
	int has_mount, has_usage;
	double size, used, total;
} Part;
typedef struct {
	char name[40], model[80];
	double size;
	int removable;
	unsigned long long prev_r, prev_w;
	Hist hist;
	Part part[MAXPART];
	int npart;
	double dev_used, dev_total;
	int has_usage;
} Disk;
typedef struct {
	Disk d[MAXDISK];
	int n;
	double prev_at;
	Hist agg;
	double used, total;
} Disks;

static int disk_cmp(const void *a, const void *b) { return strcmp(((const Disk *)a)->name, ((const Disk *)b)->name); }
static int part_cmp(const void *a, const void *b) { return strcmp(((const Part *)a)->name, ((const Part *)b)->name); }

typedef struct { char dev[48]; char mp[160]; } Mount;

static int read_mounts(Mount *m, int cap)
{
	FILE *f = fopen("/proc/mounts", "r");
	char dev[256], mp[256];
	int n = 0, i;
	if (!f) return 0;
	while (fscanf(f, "%255s %255s%*[^\n]", dev, mp) == 2) {
		if (strncmp(dev, "/dev/", 5)) continue;
		for (i = 0; i < n; i++) if (!strcmp(m[i].dev, dev + 5)) break; /* a later entry replaces an earlier one, like a map insert */
		if (i == n) { if (n >= cap) continue; n++; }
		snprintf(m[i].dev, sizeof m[i].dev, "%.47s", dev + 5);
		snprintf(m[i].mp, sizeof m[i].mp, "%.159s", mp);
	}
	fclose(f);
	return n;
}

static const char *mount_of(const Mount *m, int n, const char *dev)
{
	int i;
	for (i = 0; i < n; i++) if (!strcmp(m[i].dev, dev)) return m[i].mp;
	return NULL;
}

static int statvfs_usage(const char *path, double *used, double *total)
{
	struct statvfs sv;
	double t, fr;
	if (statvfs(path, &sv) != 0) return 0;
	t = (double)sv.f_blocks * (double)sv.f_frsize;
	fr = (double)sv.f_bfree * (double)sv.f_frsize;
	*total = t;
	*used = t > fr ? t - fr : 0;
	return 1;
}

static Disk ND[MAXDISK];
static Mount MOUNTS[256];

static void disks_refresh(Disks *D)
{
	DIR *d = opendir("/sys/block");
	struct dirent *e;
	double t = now_s(), dt = D->prev_at > 0 ? t - D->prev_at : -1, agg = 0;
	int n = 0, i, nm;
	if (!d) return;
	memset(ND, 0, sizeof ND);
	while ((e = readdir(d)) && n < MAXDISK) {
		char base[300], path[400], b[512], *tok[16], *p, *sv = NULL;
		const char *nm_ = e->d_name;
		Disk *k;
		unsigned long long size_sec, rd, wr;
		int nt = 0;
		double rr = 0, wrate = 0;
		if (nm_[0] == '.' || !strncmp(nm_, "loop", 4) || !strncmp(nm_, "ram", 3) || !strncmp(nm_, "zram", 4)) continue;
		snprintf(base, sizeof base, "/sys/block/%.60s", nm_);
		snprintf(path, sizeof path, "%s/size", base);
		if (read_file(path, b, sizeof b) <= 0) continue;
		size_sec = strtoull(b, NULL, 10);
		if (size_sec == 0) continue;
		k = &ND[n];
		snprintf(k->name, sizeof k->name, "%.39s", nm_);
		k->size = (double)size_sec * 512.0;
		snprintf(path, sizeof path, "%s/device/model", base);
		if (read_file(path, b, sizeof b) > 0) snprintf(k->model, sizeof k->model, "%.79s", trim(b));
		snprintf(path, sizeof path, "%s/removable", base);
		k->removable = read_file(path, b, sizeof b) > 0 && !strcmp(trim(b), "1");
		snprintf(path, sizeof path, "%s/stat", base);
		if (read_file(path, b, sizeof b) <= 0) continue;
		for (p = strtok_r(b, " \t\n", &sv); p && nt < 16; p = strtok_r(NULL, " \t\n", &sv)) tok[nt++] = p;
		if (nt < 7) continue;
		rd = strtoull(tok[2], NULL, 10);
		wr = strtoull(tok[6], NULL, 10);
		for (i = 0; i < D->n; i++)
			if (!strcmp(D->d[i].name, k->name)) {
				k->hist = D->d[i].hist;
				if (dt > 0) {
					rr = rd >= D->d[i].prev_r ? (double)(rd - D->d[i].prev_r) * 512.0 / dt : 0;
					wrate = wr >= D->d[i].prev_w ? (double)(wr - D->d[i].prev_w) * 512.0 / dt : 0;
				}
			}
		agg += rr + wrate;
		hpush(&k->hist, rr + wrate);
		k->prev_r = rd;
		k->prev_w = wr;
		n++;
	}
	closedir(d);
	qsort(ND, (size_t)n, sizeof(Disk), disk_cmp);
	hpush(&D->agg, agg);
	D->prev_at = t;
	nm = read_mounts(MOUNTS, 256);
	D->used = D->total = 0;
	for (i = 0; i < n; i++) {
		DIR *pd;
		char dir[300];
		Disk *k = &ND[i];
		snprintf(dir, sizeof dir, "/sys/block/%.60s", k->name);
		pd = opendir(dir);
		if (pd) {
			struct dirent *pe;
			while ((pe = readdir(pd)) && k->npart < MAXPART) {
				char pp[600], b[64];
				Part *p;
				const char *mp;
				if (pe->d_name[0] == '.') continue;
				snprintf(pp, sizeof pp, "%s/%.60s/partition", dir, pe->d_name);
				if (access(pp, F_OK) != 0) continue;
				p = &k->part[k->npart++];
				snprintf(p->name, sizeof p->name, "%.39s", pe->d_name);
				snprintf(pp, sizeof pp, "%s/%.60s/size", dir, pe->d_name);
				p->size = read_file(pp, b, sizeof b) > 0 ? (double)strtoull(b, NULL, 10) * 512.0 : 0;
				mp = mount_of(MOUNTS, nm, p->name);
				if (mp) {
					p->has_mount = 1;
					snprintf(p->mount, sizeof p->mount, "%s", mp);
					if (statvfs_usage(mp, &p->used, &p->total)) {
						p->has_usage = 1;
						D->used += p->used;
						D->total += p->total;
						k->dev_used += p->used;
						k->dev_total += p->total;
					}
				}
			}
			closedir(pd);
		}
		qsort(k->part, (size_t)k->npart, sizeof(Part), part_cmp);
		k->has_usage = k->dev_total > 0;
	}
	memcpy(D->d, ND, sizeof(Disk) * (size_t)n);
	D->n = n;
}

#define MAXNET 16
#define MAXADDR 8
typedef struct {
	char name[40];
	unsigned long long prev_rx, prev_tx;
	double rx, tx;
	int speed; /* Mbps, 0 = unknown */
	char addr[MAXADDR][64];
	int naddr;
	Hist hist;
} Iface;
typedef struct { Iface i[MAXNET]; int n; double prev_at; Hist agg; } Net;

static Iface NI[MAXNET];
static int iface_cmp(const void *a, const void *b) { return strcmp(((const Iface *)a)->name, ((const Iface *)b)->name); }

static void net_refresh(Net *N)
{
	char buf[16384], *line, *save = NULL;
	double t = now_s(), dt = N->prev_at > 0 ? t - N->prev_at : -1, agg = 0;
	int n = 0, i, a;
	struct ifaddrs *ifa, *p;
	if (read_file("/proc/net/dev", buf, sizeof buf) <= 0) return;
	memset(NI, 0, sizeof NI);
	line = strtok_r(buf, "\n", &save);
	line = strtok_r(NULL, "\n", &save);
	for (line = strtok_r(NULL, "\n", &save); line && n < MAXNET; line = strtok_r(NULL, "\n", &save)) {
		char *colon = strchr(line, ':'), *nm;
		unsigned long long rx, tx, skip;
		Iface *f;
		char path[96], b[32];
		if (!colon) continue;
		*colon = 0;
		nm = trim(line);
		if (!strcmp(nm, "lo")) continue;
		if (sscanf(colon + 1, "%llu %llu %llu %llu %llu %llu %llu %llu %llu", &rx, &skip, &skip, &skip, &skip, &skip, &skip, &skip, &tx) != 9)
			continue;
		f = &NI[n++];
		snprintf(f->name, sizeof f->name, "%.39s", nm);
		for (i = 0; i < N->n; i++)
			if (!strcmp(N->i[i].name, f->name)) {
				f->hist = N->i[i].hist;
				if (dt > 0 && rx >= N->i[i].prev_rx && tx >= N->i[i].prev_tx) {
					f->rx = (double)(rx - N->i[i].prev_rx) / dt;
					f->tx = (double)(tx - N->i[i].prev_tx) / dt;
				}
			}
		agg += f->rx + f->tx;
		hpush(&f->hist, f->rx + f->tx);
		f->prev_rx = rx;
		f->prev_tx = tx;
		snprintf(path, sizeof path, "/sys/class/net/%.40s/speed", f->name);
		if (read_file(path, b, sizeof b) > 0 && atoi(b) > 0) f->speed = atoi(b);
	}
	qsort(NI, (size_t)n, sizeof(Iface), iface_cmp);
	if (getifaddrs(&ifa) == 0) {
		for (p = ifa; p; p = p->ifa_next) {
			char ip[64], out[96];
			int prefix = 0, fam;
			if (!p->ifa_addr) continue;
			fam = p->ifa_addr->sa_family;
			if (fam == AF_INET) {
				const struct sockaddr_in *s = (const struct sockaddr_in *)p->ifa_addr;
				inet_ntop(AF_INET, &s->sin_addr, ip, sizeof ip);
				if (p->ifa_netmask) prefix = __builtin_popcount(ntohl(((const struct sockaddr_in *)p->ifa_netmask)->sin_addr.s_addr));
				else prefix = 32;
			} else if (fam == AF_INET6) {
				const struct sockaddr_in6 *s = (const struct sockaddr_in6 *)p->ifa_addr;
				int k;
				inet_ntop(AF_INET6, &s->sin6_addr, ip, sizeof ip);
				if (p->ifa_netmask) {
					const struct sockaddr_in6 *m = (const struct sockaddr_in6 *)p->ifa_netmask;
					for (k = 0; k < 16; k++) prefix += __builtin_popcount(m->sin6_addr.s6_addr[k]);
				} else prefix = 128;
				if (!strncmp(ip, "fe80:", 5)) continue; /* link-local noise */
			} else continue;
			snprintf(out, sizeof out, "%.60s/%d", ip, prefix);
			for (a = 0; a < n; a++)
				if (!strcmp(NI[a].name, p->ifa_name) && NI[a].naddr < MAXADDR)
					snprintf(NI[a].addr[NI[a].naddr++], 64, "%.63s", out);
		}
		freeifaddrs(ifa);
	}
	hpush(&N->agg, agg);
	memcpy(N->i, NI, sizeof(Iface) * (size_t)n);
	N->n = n;
	N->prev_at = t;
}

typedef struct {
	int found;
	double util;
	unsigned long long used_mb, total_mb;
	Hist hist;
} Gpu;

static int run_capture(const char *cmd, char *out, size_t cap)
{
	FILE *p = popen(cmd, "r");
	size_t n;
	if (!p) return -1;
	n = fread(out, 1, cap - 1, p);
	out[n] = 0;
	return pclose(p) == 0 ? (int)n : -1;
}

static unsigned long long drm_mb(const char *dev, const char *name)
{
	char path[400], b[64];
	snprintf(path, sizeof path, "%s/%s", dev, name);
	return read_file(path, b, sizeof b) > 0 ? strtoull(b, NULL, 10) / 1024 / 1024 : 0;
}

static void gpu_refresh(Gpu *g)
{
	char out[16384];
	g->found = 0;
	g->util = 0;
	g->used_mb = g->total_mb = 0;
	if (which("nvidia-smi") &&
	    run_capture("nvidia-smi --query-gpu=utilization.gpu,memory.used,memory.total --format=csv,noheader,nounits 2>/dev/null", out, sizeof out) > 0) {
		double u;
		unsigned long long us, to;
		if (sscanf(out, "%lf , %llu , %llu", &u, &us, &to) == 3) {
			g->found = 1;
			g->util = u;
			g->used_mb = us;
			g->total_mb = to;
		}
	}
	if (!g->found) {
		DIR *d = opendir("/sys/class/drm");
		struct dirent *e;
		while (d && (e = readdir(d)) && !g->found) {
			char dev[300], path[400], b[64];
			if (strncmp(e->d_name, "card", 4) || strchr(e->d_name, '-')) continue;
			snprintf(dev, sizeof dev, "/sys/class/drm/%.60s/device", e->d_name);
			snprintf(path, sizeof path, "%s/gpu_busy_percent", dev);
			if (read_file(path, b, sizeof b) <= 0) continue;
			g->found = 1;
			g->util = atof(b);
			g->used_mb = drm_mb(dev, "mem_info_vram_used");
			g->total_mb = drm_mb(dev, "mem_info_vram_total");
		}
		if (d) closedir(d);
	}
	if (!g->found && which("intel_gpu_top") &&
	    run_capture("intel_gpu_top -J -s 100 -o - 2>/dev/null", out, sizeof out) > 0) {
		const char *p = out;
		double mx = 0;
		int any = 0;
		while ((p = strstr(p, "\"busy\":"))) {
			double v;
			p += 7;
			if (sscanf(p, "%lf", &v) == 1) { any = 1; if (v > mx) mx = v; }
		}
		if (any) { g->found = 1; g->util = mx; }
	}
	if (g->found) hpush(&g->hist, g->util);
}

/* ------------------------------------------------------------------ processes */
typedef struct {
	int pid;
	char comm[48];
	double cpu, disk_bps, gpu_pct;
	unsigned long long mem_mb, gpu_mb, jiff, io;
	int kernel;
} Ent;

typedef struct {
	int pid;
	double cpu, disk_bps, gpu_pct;
	unsigned long long mem_mb, gpu_mb;
} Child;

typedef struct {
	char name[80];
	int count;
	double cpu, mem_pct, disk_pct, disk_bps, gpu_pct;
	unsigned long long mem_mb, gpu_mb;
	int *pids, npids;
	Child *kids;
	int nkids;
	int divider, ghost;
} Group;

/* activity history of a row: longer than the 30 samples of the panel graphs, so the ACTIVITY column
 * can use the whole width it is given */
#define PROC_HIST 120
typedef struct { char name[96]; double v[PROC_HIST]; int n; } NamedHist;

/* what is on screen: a group row (kid = -1) or one process of an expanded group */
typedef struct { int g, kid; } Row;
typedef struct { Group g; double since; } GhostG;

typedef struct {
	int mode, cursor;
	Group *g;
	int ng;
	char (*marked)[80];
	int nmarked, cmarked;
	NamedHist *hist;
	int nhist;
	Row *rows;
	int nrows;
	char (*expanded)[80];
	int nexp, cexp;
	GhostG *ghosts;
	int nghost;
	int *pj_pid;
	unsigned long long *pj_j, *pj_io;
	int npj;
	double prev_at, last_sort;
	char filter[64];
} Procs;

static void group_free(Group *g) { free(g->pids); free(g->kids); g->pids = NULL; g->kids = NULL; }

static Group group_clone(const Group *s)
{
	Group c = *s;
	c.pids = malloc((size_t)(s->npids > 0 ? s->npids : 1) * sizeof(int));
	memcpy(c.pids, s->pids, (size_t)s->npids * sizeof(int));
	c.kids = malloc((size_t)(s->nkids > 0 ? s->nkids : 1) * sizeof(Child));
	memcpy(c.kids, s->kids, (size_t)s->nkids * sizeof(Child));
	return c;
}

typedef struct { Group *g; int n, cap; } GVec;
static Group *gv_push(GVec *v)
{
	if (v->n == v->cap) { v->cap = v->cap ? v->cap * 2 : 64; v->g = realloc(v->g, (size_t)v->cap * sizeof(Group)); }
	memset(&v->g[v->n], 0, sizeof(Group));
	return &v->g[v->n++];
}

static void group_add(Group *g, const Ent *e)
{
	g->count++;
	g->cpu += e->cpu;
	g->mem_mb += e->mem_mb;
	g->disk_bps += e->disk_bps;
	g->gpu_pct += e->gpu_pct;
	g->gpu_mb += e->gpu_mb;
	g->pids = realloc(g->pids, (size_t)(g->npids + 1) * sizeof(int));
	g->pids[g->npids++] = e->pid;
	g->kids = realloc(g->kids, (size_t)(g->nkids + 1) * sizeof(Child));
	g->kids[g->nkids].pid = e->pid;
	g->kids[g->nkids].cpu = e->cpu;
	g->kids[g->nkids].disk_bps = e->disk_bps;
	g->kids[g->nkids].gpu_pct = e->gpu_pct;
	g->kids[g->nkids].mem_mb = e->mem_mb;
	g->kids[g->nkids].gpu_mb = e->gpu_mb;
	g->nkids++;
}

static int kid_cmp(const void *a, const void *b)
{
	const Child *x = a, *y = b;
	if (x->cpu != y->cpu) return x->cpu < y->cpu ? 1 : -1;
	return x->pid - y->pid;
}

typedef struct { double cpu; int idx; } SortKey;
static int sk_cmp(const void *a, const void *b)
{
	const SortKey *x = a, *y = b;
	if (x->cpu != y->cpu) return x->cpu < y->cpu ? 1 : -1;
	return x->idx - y->idx; /* stable: ties keep insertion order */
}

static void sort_groups(Group *g, int n)
{
	SortKey *k = malloc((size_t)(n ? n : 1) * sizeof *k);
	Group *tmp = malloc((size_t)(n ? n : 1) * sizeof *tmp);
	int i;
	for (i = 0; i < n; i++) { k[i].cpu = g[i].cpu; k[i].idx = i; }
	qsort(k, (size_t)n, sizeof *k, sk_cmp);
	for (i = 0; i < n; i++) tmp[i] = g[k[i].idx];
	memcpy(g, tmp, (size_t)n * sizeof *g);
	free(k);
	free(tmp);
}

static void apply_pct(Group *g, int n, double mem_total, double disk_bps)
{
	int i;
	for (i = 0; i < n; i++) {
		if (mem_total > 0) g[i].mem_pct = (double)g[i].mem_mb / (mem_total / 1024.0 / 1024.0) * 100.0;
		if (disk_bps > 0) g[i].disk_pct = g[i].disk_bps / disk_bps * 100.0;
	}
}

static void by_name(GVec *out, const Ent *e, int n, int want_kernel, double mem_total, double disk_bps)
{
	int start = out->n, i, j;
	for (i = 0; i < n; i++) {
		Group *g = NULL;
		if (want_kernel >= 0 && e[i].kernel != want_kernel) continue;
		for (j = start; j < out->n; j++) if (!strcmp(out->g[j].name, e[i].comm)) { g = &out->g[j]; break; }
		if (!g) {
			g = gv_push(out);
			snprintf(g->name, sizeof g->name, "%.79s", e[i].comm);
		}
		group_add(g, &e[i]);
	}
	apply_pct(out->g + start, out->n - start, mem_total, disk_bps);
	for (i = start; i < out->n; i++) qsort(out->g[i].kids, (size_t)out->g[i].nkids, sizeof(Child), kid_cmp);
	sort_groups(out->g + start, out->n - start);
}

static void raw_groups(GVec *out, const Ent *e, int n, double mem_total, double disk_bps)
{
	int i, start = out->n;
	for (i = 0; i < n; i++) {
		Group *g = gv_push(out);
		snprintf(g->name, sizeof g->name, "%.60s [%d]", e[i].comm, e[i].pid);
		group_add(g, &e[i]);
	}
	apply_pct(out->g + start, out->n - start, mem_total, disk_bps);
	sort_groups(out->g + start, out->n - start);
}

static int ent_cmp_pid(const void *a, const void *b) { return ((const Ent *)a)->pid - ((const Ent *)b)->pid; }

static void nvidia_pmon(Ent *e, int n)
{
	FILE *p;
	char line[256];
	int i;
	if (!which("nvidia-smi")) return;
	p = popen("nvidia-smi pmon -c 1 -s um 2>/dev/null", "r");
	if (!p) return;
	while (fgets(line, sizeof line, p)) {
		char *f[16], *tok, *sv = NULL;
		int nf = 0, pid;
		char *s = trim(line);
		if (!*s || *s == '#') continue;
		for (tok = strtok_r(s, " \t", &sv); tok && nf < 16; tok = strtok_r(NULL, " \t", &sv)) f[nf++] = tok;
		if (nf < 10) continue;
		pid = atoi(f[1]);
		for (i = 0; i < n; i++)
			if (e[i].pid == pid) {
				e[i].gpu_pct = atof(f[3]);
				e[i].gpu_mb = strtoull(f[9], NULL, 10);
			}
	}
	pclose(p);
}

static int is_expanded(const Procs *P, const char *name)
{
	int i;
	for (i = 0; i < P->nexp; i++) if (!strcmp(P->expanded[i], name)) return 1;
	return 0;
}

/* a group of two or more processes can be opened into a tree (raw view is already one row per pid) */
static int expandable(const Procs *P, const Group *g) { return P->mode != 1 && !g->divider && !g->ghost && g->nkids >= 2; }

static void build_rows(Procs *P)
{
	int i, k, n = 0;
	free(P->rows);
	P->rows = malloc((size_t)(P->ng * 2 + 1) * sizeof(Row));
	for (i = 0; i < P->ng; i++) {
		const Group *g = &P->g[i];
		int open = expandable(P, g) && is_expanded(P, g->name);
		if (open) {
			size_t need = (size_t)(n + 1 + g->nkids + (P->ng - i)) * sizeof(Row);
			P->rows = realloc(P->rows, need + (size_t)P->ng * sizeof(Row));
		}
		P->rows[n].g = i; P->rows[n].kid = -1; n++;
		if (open) for (k = 0; k < g->nkids; k++) { P->rows[n].g = i; P->rows[n].kid = k; n++; }
	}
	P->nrows = n;
	if (P->cursor >= P->nrows) P->cursor = P->nrows ? P->nrows - 1 : 0;
}

/* the name a row is marked and tracked under */
static void row_key(const Procs *P, const Row *r, char *out, size_t n)
{
	const Group *g = &P->g[r->g];
	if (r->kid < 0) snprintf(out, n, "%s", g->name);
	else snprintf(out, n, "%.60s [%d]", g->name, g->kids[r->kid].pid);
}

static int row_is_divider(const Procs *P, int i) { return P->rows[i].kid < 0 && P->g[P->rows[i].g].divider; }

static int find_prev(const Procs *P, int pid)
{
	int lo = 0, hi = P->npj - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		if (P->pj_pid[mid] == pid) return mid;
		if (P->pj_pid[mid] < pid) lo = mid + 1; else hi = mid - 1;
	}
	return -1;
}

static void procs_refresh(Procs *P, double mem_total, double total_disk_bps)
{
	DIR *d = opendir("/proc");
	struct dirent *de;
	Ent *cur = NULL;
	int n = 0, cap = 0, i, j;
	double t = now_s(), dt = P->prev_at > 0 ? t - P->prev_at : -1;
	long hz = sysconf(_SC_CLK_TCK);
	GVec groups = {0, 0, 0};
	const double GHOST_GRACE = 3.0, RESORT = 4.0;
	if (!d) return;
	if (hz <= 0) hz = 100;
	while ((de = readdir(d))) {
		char path[64], buf[4096], *rp, *lp;
		Ent e;
		int pid, idx = 0;
		unsigned long long ut = 0, st = 0;
		char *tok, *sv = NULL, *copy;
		if (!isdigit((unsigned char)de->d_name[0])) continue;
		pid = atoi(de->d_name);
		snprintf(path, sizeof path, "/proc/%d/stat", pid);
		if (read_file(path, buf, sizeof buf) <= 0) continue;
		lp = strchr(buf, '(');
		rp = strrchr(buf, ')');
		if (!lp || !rp || rp < lp) continue;
		memset(&e, 0, sizeof e);
		e.pid = pid;
		{
			size_t l = (size_t)(rp - lp - 1);
			if (l >= sizeof e.comm) l = sizeof e.comm - 1;
			memcpy(e.comm, lp + 1, l);
			e.comm[l] = 0;
		}
		/* fields after ") ": index 0 = state; utime = 11, stime = 12 */
		copy = strdup(rp + 2);
		for (tok = strtok_r(copy, " ", &sv); tok; tok = strtok_r(NULL, " ", &sv), idx++) {
			if (idx == 11) ut = strtoull(tok, NULL, 10);
			if (idx == 12) { st = strtoull(tok, NULL, 10); break; }
		}
		free(copy);
		if (idx < 12) continue;
		e.jiff = ut + st;
		snprintf(path, sizeof path, "/proc/%d/status", pid);
		if (read_file(path, buf, sizeof buf) > 0) {
			char *v = strstr(buf, "VmRSS:");
			if (v) e.mem_mb = strtoull(v + 6, NULL, 10) / 1024;
		}
		snprintf(path, sizeof path, "/proc/%d/io", pid);
		if (read_file(path, buf, sizeof buf) > 0) {
			char *v = strstr(buf, "read_bytes:");
			unsigned long long rb = v ? strtoull(v + 11, NULL, 10) : 0, wb = 0;
			v = strstr(buf, "write_bytes:");
			if (v) wb = strtoull(v + 12, NULL, 10);
			e.io = rb + wb;
		}
		{
			char exe[64], tgt[8];
			snprintf(exe, sizeof exe, "/proc/%d/exe", pid);
			/* a kernel thread has no exe link; a long target only truncates into tgt, which is fine */
			e.kernel = readlink(exe, tgt, sizeof tgt) < 0;
		}
		if (P->filter[0] && !strstr(e.comm, P->filter)) continue;
		if (n == cap) { cap = cap ? cap * 2 : 512; cur = realloc(cur, (size_t)cap * sizeof(Ent)); }
		cur[n++] = e;
	}
	closedir(d);
	qsort(cur, (size_t)n, sizeof(Ent), ent_cmp_pid);
	for (i = 0; i < n; i++) {
		int pi = find_prev(P, cur[i].pid);
		if (pi >= 0 && dt > 0) {
			if (cur[i].jiff >= P->pj_j[pi]) cur[i].cpu = (double)(cur[i].jiff - P->pj_j[pi]) / (double)hz / dt * 100.0;
			if (cur[i].io > P->pj_io[pi]) cur[i].disk_bps = (double)(cur[i].io - P->pj_io[pi]) / dt;
		}
	}
	nvidia_pmon(cur, n);

	if (P->mode == 0) by_name(&groups, cur, n, -1, mem_total, total_disk_bps);
	else if (P->mode == 1) raw_groups(&groups, cur, n, mem_total, total_disk_bps);
	else {
		Group *dv = gv_push(&groups);
		snprintf(dv->name, sizeof dv->name, "\xE2\x94\x80\xE2\x94\x80 user \xE2\x94\x80\xE2\x94\x80");
		dv->divider = 1;
		by_name(&groups, cur, n, 0, mem_total, total_disk_bps);
		dv = gv_push(&groups);
		snprintf(dv->name, sizeof dv->name, "\xE2\x94\x80\xE2\x94\x80 system/kernel \xE2\x94\x80\xE2\x94\x80");
		dv->divider = 1;
		by_name(&groups, cur, n, 1, mem_total, total_disk_bps);
	}

	/* ghosts: rows of the previous display that are gone now */
	for (i = 0; i < P->ng; i++) {
		const Group *pg = &P->g[i];
		int present = 0, have = 0;
		if (pg->divider || pg->ghost) continue;
		for (j = 0; j < groups.n; j++) if (!groups.g[j].divider && !strcmp(groups.g[j].name, pg->name)) { present = 1; break; }
		if (present) continue;
		for (j = 0; j < P->nghost; j++) if (!strcmp(P->ghosts[j].g.name, pg->name)) have = 1;
		if (!have) {
			P->ghosts = realloc(P->ghosts, (size_t)(P->nghost + 1) * sizeof(GhostG));
			P->ghosts[P->nghost].g = group_clone(pg);
			P->ghosts[P->nghost].since = t;
			P->nghost++;
		}
	}
	for (i = 0; i < P->nghost;) {
		int present = 0;
		for (j = 0; j < groups.n; j++) if (!groups.g[j].divider && !strcmp(groups.g[j].name, P->ghosts[i].g.name)) present = 1;
		if (present || t - P->ghosts[i].since >= GHOST_GRACE) {
			group_free(&P->ghosts[i].g);
			P->ghosts[i] = P->ghosts[--P->nghost];
			continue;
		}
		i++;
	}

	/* anti-jitter: keep the previous row order unless the resort interval elapsed */
	if (P->last_sort > 0 && t - P->last_sort < RESORT) {
		GVec re = {0, 0, 0};
		char *seen = calloc((size_t)(groups.n ? groups.n : 1), 1);
		for (i = 0; i < P->ng; i++) {
			if (P->g[i].ghost) continue;
			for (j = 0; j < groups.n; j++)
				if (!seen[j] && !strcmp(groups.g[j].name, P->g[i].name)) {
					*gv_push(&re) = groups.g[j];
					seen[j] = 1;
					break;
				}
		}
		for (j = 0; j < groups.n; j++) if (!seen[j]) *gv_push(&re) = groups.g[j];
		free(seen);
		free(groups.g);
		groups = re;
	} else {
		P->last_sort = t;
	}
	if (P->mode != 2) {
		for (i = 0; i < P->nghost; i++) {
			Group *gg = gv_push(&groups);
			*gg = group_clone(&P->ghosts[i].g);
			gg->ghost = 1;
		}
	}

	for (i = 0; i < P->ng; i++) group_free(&P->g[i]);
	free(P->g);
	P->g = groups.g;
	P->ng = groups.n;
	build_rows(P);

	/* per-row activity history from the rows currently on screen only */
	{
		NamedHist *nh = calloc((size_t)(P->nrows ? P->nrows : 1), sizeof *nh);
		int nn = 0;
		for (i = 0; i < P->nrows; i++) {
			const Group *g = &P->g[P->rows[i].g];
			char key[96];
			double val;
			NamedHist *dst;
			if (row_is_divider(P, i)) continue;
			row_key(P, &P->rows[i], key, sizeof key);
			val = g->ghost ? 0.0 : (P->rows[i].kid < 0 ? g->cpu : g->kids[P->rows[i].kid].cpu);
			dst = &nh[nn++];
			snprintf(dst->name, sizeof dst->name, "%s", key);
			for (j = 0; j < P->nhist; j++)
				if (!strcmp(P->hist[j].name, key)) { memcpy(dst->v, P->hist[j].v, sizeof dst->v); dst->n = P->hist[j].n; break; }
			if (dst->n == PROC_HIST) { memmove(dst->v, dst->v + 1, (PROC_HIST - 1) * sizeof(double)); dst->n--; }
			dst->v[dst->n++] = val;
		}
		free(P->hist);
		P->hist = nh;
		P->nhist = nn;
	}

	if (P->cursor >= P->nrows) P->cursor = P->nrows ? P->nrows - 1 : 0;
	if (P->nrows && row_is_divider(P, P->cursor)) {
		for (i = P->cursor + 1; i < P->nrows; i++) if (!row_is_divider(P, i)) { P->cursor = i; break; }
		if (row_is_divider(P, P->cursor))
			for (i = P->cursor - 1; i >= 0; i--) if (!row_is_divider(P, i)) { P->cursor = i; break; }
	}

	free(P->pj_pid); free(P->pj_j); free(P->pj_io);
	P->pj_pid = malloc((size_t)(n ? n : 1) * sizeof(int));
	P->pj_j = malloc((size_t)(n ? n : 1) * sizeof(unsigned long long));
	P->pj_io = malloc((size_t)(n ? n : 1) * sizeof(unsigned long long));
	for (i = 0; i < n; i++) { P->pj_pid[i] = cur[i].pid; P->pj_j[i] = cur[i].jiff; P->pj_io[i] = cur[i].io; }
	P->npj = n;
	P->prev_at = t;
	free(cur);
}

static int is_marked(const Procs *P, const char *name)
{
	int i;
	for (i = 0; i < P->nmarked; i++) if (!strcmp(P->marked[i], name)) return 1;
	return 0;
}

static void toggle_mark(Procs *P)
{
	int i;
	char key[96];
	if (P->cursor >= P->nrows || row_is_divider(P, P->cursor)) return;
	row_key(P, &P->rows[P->cursor], key, sizeof key);
	for (i = 0; i < P->nmarked; i++)
		if (!strcmp(P->marked[i], key)) { memcpy(P->marked[i], P->marked[P->nmarked - 1], 80); P->nmarked--; return; }
	if (P->nmarked == P->cmarked) { P->cmarked = P->cmarked ? P->cmarked * 2 : 16; P->marked = realloc(P->marked, (size_t)P->cmarked * 80); }
	snprintf(P->marked[P->nmarked++], 80, "%.79s", key);
}

static void cursor_move(Procs *P, int dir)
{
	int i = P->cursor + dir;
	while (i >= 0 && i < P->nrows && row_is_divider(P, i)) i += dir;
	if (i >= 0 && i < P->nrows) P->cursor = i;
}

/* Enter: open or close the process tree under a program; on a child row it closes its parent */
static void toggle_expand(Procs *P)
{
	const Row *r;
	const Group *g;
	int i;
	if (P->cursor >= P->nrows) return;
	r = &P->rows[P->cursor];
	g = &P->g[r->g];
	if (!expandable(P, g)) return;
	for (i = 0; i < P->nexp; i++)
		if (!strcmp(P->expanded[i], g->name)) {
			memcpy(P->expanded[i], P->expanded[P->nexp - 1], 80);
			P->nexp--;
			build_rows(P);
			for (i = 0; i < P->nrows; i++) if (P->rows[i].g == r->g && P->rows[i].kid < 0) { P->cursor = i; break; }
			return;
		}
	if (P->nexp == P->cexp) { P->cexp = P->cexp ? P->cexp * 2 : 16; P->expanded = realloc(P->expanded, (size_t)P->cexp * 80); }
	snprintf(P->expanded[P->nexp++], 80, "%.79s", g->name);
	build_rows(P);
}

/* the pids a kill or a palette command acts on: marked rows, or the row under the cursor */
static int collect_targets(const Procs *P, int *pids, int cap, char *names, size_t ncap)
{
	int n = 0, i, j;
	for (i = 0; i < P->nrows; i++) {
		const Row *r = &P->rows[i];
		const Group *g = &P->g[r->g];
		char key[96];
		int take;
		if (row_is_divider(P, i)) continue;
		row_key(P, r, key, sizeof key);
		take = P->nmarked ? is_marked(P, key) : (i == P->cursor);
		if (!take) continue;
		if (names) {
			if (names[0] && strlen(names) + 3 < ncap) strcat(names, ", ");
			if (strlen(names) + strlen(key) + 1 < ncap) strcat(names, key);
		}
		if (r->kid >= 0) { if (n < cap) pids[n++] = g->kids[r->kid].pid; }
		else for (j = 0; j < g->npids && n < cap; j++) pids[n++] = g->pids[j];
	}
	return n;
}

/* ------------------------------------------------------------------ app */
typedef struct {
	Cpu cpu;
	Mem mem;
	Disks disk;
	Net net;
	Gpu gpu;
	Procs procs;
	char tpu[80], npu[160];
	int has_tpu, has_npu;
	int interval_ms;
	int recording, anonymous, palette, quit;
	char palette_in[128], palette_out[512], msg[512];
	double msg_at;
	int has_msg, has_palette_out;
	FILE *rec;
	char rec_path[96];
} App;

static void app_tick(App *A)
{
	cpu_refresh(&A->cpu);
	mem_refresh(&A->mem);
	disks_refresh(&A->disk);
	net_refresh(&A->net);
	gpu_refresh(&A->gpu);
	procs_refresh(&A->procs, A->mem.total, hlast(&A->disk.agg));
	if (A->rec) {
		if (fprintf(A->rec, "%ld,%.2f,%.2f,%.2f,%.2f,%.2f\n", (long)time(NULL), A->cpu.agg, hlast(&A->mem.used_hist),
		            A->gpu.util, hlast(&A->disk.agg), hlast(&A->net.agg)) < 0) {
			fclose(A->rec);
			A->rec = NULL;
			A->recording = 0;
		} else fflush(A->rec);
	}
}

static void set_msg(App *A, const char *s)
{
	snprintf(A->msg, sizeof A->msg, "%.500s", s);
	A->msg_at = now_s();
	A->has_msg = 1;
}

static void toggle_record(App *A)
{
	if (A->recording) {
		if (A->rec) fclose(A->rec);
		A->rec = NULL;
		A->recording = 0;
		return;
	}
	snprintf(A->rec_path, sizeof A->rec_path, "gtop-record-%ld.csv", (long)time(NULL));
	A->rec = fopen(A->rec_path, "w");
	if (!A->rec) {
		char m[400];
		snprintf(m, sizeof m, "record: could not create %.80s: %.80s", A->rec_path, strerror(errno));
		set_msg(A, m);
		return;
	}
	fprintf(A->rec, "unix_ts,cpu_pct,mem_used_pct,gpu_util_pct,disk_rate_bytes_s,net_rate_bytes_s\n");
	A->recording = 1;
}

static void kill_selected(App *A)
{
	Procs *P = &A->procs;
	int killed = 0, total, i;
	static int pids[65536];
	char names[300] = "", m[400];
	total = collect_targets(P, pids, 65536, names, sizeof names);
	if (!total) return;
	for (i = 0; i < total; i++) if (kill(pids[i], SIGKILL) == 0) killed++;
	P->nmarked = 0;
	snprintf(m, sizeof m, "killed %d/%d (%.300s)", killed, total, names);
	set_msg(A, m);
}

static int find_command(const char *name, char *out, size_t n)
{
	const char *env = getenv("GTOP_COMMANDS");
	char exe[512];
	ssize_t k;
	if (strchr(name, '/') || name[0] == '.') return 0;
	if (env) { snprintf(out, n, "%s/%s", env, name); if (access(out, F_OK) == 0) return 1; }
	snprintf(out, n, "commands/%s", name);
	if (access(out, F_OK) == 0) return 1;
	k = readlink("/proc/self/exe", exe, sizeof exe - 1);
	if (k > 0) {
		char *s;
		exe[k] = 0;
		s = strrchr(exe, '/');
		if (s) {
			*s = 0;
			snprintf(out, n, "%s/commands/%s", exe, name);
			if (access(out, F_OK) == 0) return 1;
			snprintf(out, n, "%s/../share/gtop/commands/%s", exe, name);
			if (access(out, F_OK) == 0) return 1;
		}
	}
	return 0;
}

static void run_palette(App *A)
{
	Procs *P = &A->procs;
	char *argv[600], line[128], path[900], name[64], *tok, *save = NULL, out[1024];
	char pidbufs[400][16];
	int argc = 0, npb = 0, i, j, pipefd[2];
	pid_t pid;
	snprintf(line, sizeof line, "%s", A->palette_in);
	A->palette_in[0] = 0;
	tok = strtok_r(trim(line), " \t", &save);
	if (!tok) return;
	snprintf(name, sizeof name, "%s", tok);
	if (!find_command(name, path, sizeof path)) {
		snprintf(A->palette_out, sizeof A->palette_out, "no such command: commands/%.60s", name);
		A->has_palette_out = 1;
		return;
	}
	argv[argc++] = "sh";
	argv[argc++] = path;
	while ((tok = strtok_r(NULL, " \t", &save)) && argc < 60) argv[argc++] = tok;
	{
		static int tp[65536];
		int nt = collect_targets(P, tp, 65536, NULL, 0);
		for (j = 0; j < nt && npb < 400 && argc < 598; j++) {
			snprintf(pidbufs[npb], 16, "%d", tp[j]);
			argv[argc++] = pidbufs[npb++];
		}
	}
	argv[argc] = NULL;
	P->nmarked = 0;
	if (pipe(pipefd) != 0) return;
	pid = fork();
	if (pid == 0) {
		dup2(pipefd[1], 1);
		dup2(pipefd[1], 2);
		close(pipefd[0]);
		close(pipefd[1]);
		execvp("sh", argv);
		_exit(127);
	}
	close(pipefd[1]);
	{
		ssize_t r, tot = 0;
		int status = 0;
		char *o;
		while ((r = read(pipefd[0], out + tot, sizeof out - 1 - (size_t)tot)) > 0 && (size_t)tot < sizeof out - 1) tot += r;
		out[tot] = 0;
		close(pipefd[0]);
		waitpid(pid, &status, 0);
		o = trim(out);
		for (i = 0; o[i]; i++) if (o[i] == '\n') o[i] = ' ';
		if (!*o && status != 0) { snprintf(out, sizeof out, "exit status: %d", WIFEXITED(status) ? WEXITSTATUS(status) : -1); o = out; }
		snprintf(A->palette_out, sizeof A->palette_out, "%s: %.400s", name, *o ? o : "ok");
		A->has_palette_out = 1;
	}
}

enum { K_UP = 1000, K_DOWN, K_LEFT, K_RIGHT, K_ENTER, K_ESC, K_BACKSPACE };

static void handle_key(App *A, int k)
{
	Procs *P = &A->procs;
	if (A->palette) {
		size_t n = strlen(A->palette_in);
		if (k == K_ESC) { A->palette = 0; A->palette_in[0] = 0; }
		else if (k == K_ENTER) { run_palette(A); A->palette = 0; }
		else if (k == K_BACKSPACE) { if (n) A->palette_in[n - 1] = 0; }
		else if (k >= 32 && k < 127 && n < sizeof A->palette_in - 1) { A->palette_in[n] = (char)k; A->palette_in[n + 1] = 0; }
		return;
	}
	switch (k) {
	case 'q': A->quit = 1; break;
	case K_LEFT: P->mode = (P->mode + 2) % 3; P->cursor = 0; P->nmarked = 0; build_rows(P); break;
	case K_RIGHT: P->mode = (P->mode + 1) % 3; P->cursor = 0; P->nmarked = 0; build_rows(P); break;
	case K_UP: cursor_move(P, -1); break;
	case K_DOWN: cursor_move(P, 1); break;
	case ' ': toggle_mark(P); break;
	case K_ENTER: toggle_expand(P); break;
	case 'k': kill_selected(A); break;
	case '/': A->palette = 1; A->palette_in[0] = 0; break;
	case 'r': toggle_record(A); break;
	case 'a': A->anonymous = !A->anonymous; break;
	case '+': case '=': A->interval_ms += 100; if (A->interval_ms > 5000) A->interval_ms = 5000; break;
	case '-': A->interval_ms -= 100; if (A->interval_ms < 100) A->interval_ms = 100; break;
	}
}

/* ------------------------------------------------------------------ drawing */
#define F_REV 1
#define F_BOLD 2
typedef struct { char s[5]; unsigned fg; unsigned char fl; } Cell;
static Cell *GRID;
static int GW, GH;

static void grid_init(int w, int h)
{
	int i;
	free(GRID);
	GW = w;
	GH = h;
	GRID = malloc((size_t)w * (size_t)h * sizeof(Cell));
	for (i = 0; i < w * h; i++) { GRID[i].s[0] = ' '; GRID[i].s[1] = 0; GRID[i].fg = C_PLAIN; GRID[i].fl = 0; }
}

static int u8len(unsigned char c) { return c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1; }

static int ncells(const char *s)
{
	int n = 0;
	while (*s) { s += u8len((unsigned char)*s); n++; }
	return n;
}

/* a clipped rectangle that lines are drawn into, like a ratatui Paragraph inside a Block */
typedef struct { int x, y, w, h; } Rect;

static void cell_put(int x, int y, unsigned fg, int fl, const char *s, int l)
{
	Cell *c;
	int i;
	if (x < 0 || y < 0 || x >= GW || y >= GH) return;
	c = &GRID[y * GW + x];
	for (i = 0; i < l && i < 4; i++) c->s[i] = s[i];
	c->s[l < 4 ? l : 4] = 0;
	c->fg = fg;
	c->fl = (unsigned char)fl;
}

/* draw string s on row `row` of area r starting at column *col, clipped; advances *col */
static void span(const Rect *r, int row, int *col, unsigned fg, int fl, const char *s)
{
	if (row < 0 || row >= r->h) return;
	while (*s) {
		int l = u8len((unsigned char)*s);
		if (*col >= r->w) return;
		cell_put(r->x + *col, r->y + row, fg, fl, s, l);
		s += l;
		(*col)++;
	}
}

static void spanf(const Rect *r, int row, int *col, unsigned fg, int fl, const char *fmt, ...) __attribute__((format(printf, 6, 7)));
static void spanf(const Rect *r, int row, int *col, unsigned fg, int fl, const char *fmt, ...)
{
	char b[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof b, fmt, ap);
	va_end(ap);
	span(r, row, col, fg, fl, b);
}

static void repeat(const Rect *r, int row, int *col, unsigned fg, const char *glyph, int n)
{
	while (n-- > 0) span(r, row, col, fg, 0, glyph);
}

/* ratatui Block with all borders and a bold accent title; returns the inner rect */
static Rect block(int x, int y, int w, int h, const char *title)
{
	Rect in;
	int i, col = 0, j;
	in.x = x + 1; in.y = y + 1; in.w = w - 2 > 0 ? w - 2 : 0; in.h = h - 2 > 0 ? h - 2 : 0;
	if (w < 2 || h < 2) { in.w = in.h = 0; return in; }
	for (i = 1; i < w - 1; i++) {
		cell_put(x + i, y, C_ACCENT, 0, "\xE2\x94\x80", 3);
		cell_put(x + i, y + h - 1, C_ACCENT, 0, "\xE2\x94\x80", 3);
	}
	for (i = 1; i < h - 1; i++) {
		cell_put(x, y + i, C_ACCENT, 0, "\xE2\x94\x82", 3);
		cell_put(x + w - 1, y + i, C_ACCENT, 0, "\xE2\x94\x82", 3);
		for (j = 1; j < w - 1; j++) cell_put(x + j, y + i, C_PLAIN, 0, " ", 1); /* clears what a popup covers */
	}
	cell_put(x, y, C_ACCENT, 0, "\xE2\x94\x8C", 3);
	cell_put(x + w - 1, y, C_ACCENT, 0, "\xE2\x94\x90", 3);
	cell_put(x, y + h - 1, C_ACCENT, 0, "\xE2\x94\x94", 3);
	cell_put(x + w - 1, y + h - 1, C_ACCENT, 0, "\xE2\x94\x98", 3);
	{
		Rect top;
		top.x = x + 1; top.y = y; top.w = w - 2; top.h = 1;
		spanf(&top, 0, &col, C_ACCENT, F_BOLD, " %s ", title);
	}
	return in;
}

static int inner_width(int w) { int v = w - 4; return v < 10 ? 10 : v; }

/* segmented bar exactly as in the Rust version: round(width*val/total) per segment, the LAST segment
 * takes whatever is left, "░" in NEUTRAL when something is still unfilled */
static void segmented_bar(const Rect *r, int row, int *col, const double *val, const unsigned *colr, int n, double total, int width)
{
	int used = 0, i;
	if (total <= 0.0 || width == 0) { repeat(r, row, col, C_PLAIN, " ", width); return; }
	for (i = 0; i < n; i++) {
		int w = (int)floor((double)width * val[i] / total + 0.5);
		if (i == n - 1) w = width > used ? width - used : 0;
		if (used + w > width) w = width > used ? width - used : 0;
		if (w > 0) { repeat(r, row, col, colr[i], "\xE2\x96\x88", w); used += w; }
	}
	if (used < width) repeat(r, row, col, C_NEUTRAL, "\xE2\x96\x91", width - used);
}

static const char *GLY[9] = {" ", "\xE2\x96\x81", "\xE2\x96\x82", "\xE2\x96\x83", "\xE2\x96\x84", "\xE2\x96\x85", "\xE2\x96\x86", "\xE2\x96\x87", "\xE2\x96\x88"};

/* raw_percent_graph (fixed 0..100) and raw_rate_graph_line (scaled to its own max): linear
 * interpolation across the width, any nonzero value shows at least the lowest glyph. History
 * values are rounded to whole numbers first, as the original did (as_u64). */
static void graph_v(const Rect *r, int row, int *col, const double *vals, int n, int width, unsigned colr, int fixed)
{
	double hv[PROC_HIST], mx = 1;
	int x, i;
	if (n > PROC_HIST) { vals += n - PROC_HIST; n = PROC_HIST; }
	if (n < 2) { repeat(r, row, col, C_PLAIN, " ", width); return; }
	for (i = 0; i < n; i++) { double v = vals[i]; hv[i] = v < 0 ? 0 : floor(v + 0.5); }
	if (fixed) mx = 100.0;
	else for (i = 0; i < n; i++) if (hv[i] > mx) mx = hv[i];
	for (x = 0; x < width; x++) {
		double tt = width > 1 ? (double)x / (double)(width - 1) * (double)(n - 1) : 0.0, v, lvl, frac;
		int i0 = (int)tt, i1, idx;
		if (i0 > n - 1) i0 = n - 1;
		i1 = i0 + 1 > n - 1 ? n - 1 : i0 + 1;
		frac = tt - (double)i0;
		v = hv[i0] * (1.0 - frac) + hv[i1] * frac;
		lvl = v / mx;
		if (lvl < 0) lvl = 0;
		if (lvl > 1) lvl = 1;
		idx = v > 0.0 ? (int)ceil(lvl * 8.0) : 0;
		if (idx > 8) idx = 8;
		if (v > 0.0 && idx < 1) idx = 1;
		span(r, row, col, colr, 0, GLY[idx]);
	}
}

static void graph(const Rect *r, int row, int *col, const Hist *h, int width, unsigned colr, int fixed)
{
	graph_v(r, row, col, h->v, h->n, width, colr, fixed);
}

static int cpu_box_lines(const App *A) { return 3 + A->cpu.ncores + 2; }
static int gpu_box_lines(const App *A) { return (A->gpu.found ? 3 : 1) + 2; }
static int ram_box_lines(void) { return 8 + 2; }
static int ram_col_height(const App *A)
{
	int n = ram_box_lines() + gpu_box_lines(A);
	if (A->has_tpu) n += 3;
	if (A->has_npu) n += 3;
	return n;
}
static int disk_content_lines(const App *A)
{
	int n = 2, i;
	for (i = 0; i < A->disk.n; i++) n += 3 + A->disk.d[i].npart;
	return n + 2;
}
static int net_content_lines(const App *A) { return 2 + A->net.n * 2 + 2; }

static void draw_cpu(const App *A, int x, int y, int w, int h)
{
	const Cpu *c = &A->cpu;
	Rect r = block(x, y, w, h, "CPU");
	int iw = inner_width(w), row = 0, col, i, bar_w;
	char suffix[48];
	double segv[3];
	unsigned segc[3];
	segc[0] = C_PRIMARY; segc[1] = C_WARN; segc[2] = C_CAUTION;
	snprintf(suffix, sizeof suffix, " %.0f%% L%.1f", c->agg, c->load1);
	bar_w = iw - ncells(suffix);
	if (bar_w < 10) bar_w = 10;
	segv[0] = c->user; segv[1] = c->sys; segv[2] = c->iow;
	col = 0;
	segmented_bar(&r, row, &col, segv, segc, 3, 100.0, bar_w);
	span(&r, row, &col, C_PRIMARY, 0, suffix);
	row++;
	col = 0;
	graph(&r, row++, &col, &c->agg_hist, iw, C_ACCENT, 1);
	col = 0;
	spanf(&r, row++, &col, C_PRIMARY, 0, "user:%.1f%% sys:%.1f%% io:%.1f%%", c->user, c->sys, c->iow);
	for (i = 0; i < c->ncores; i++) {
		char fr[32], tp[32], fixed[96];
		int gw;
		if (c->freq[i] >= 0) snprintf(fr, sizeof fr, "%dMHz", c->freq[i]); else snprintf(fr, sizeof fr, "--MHz");
		if (c->temp[i] >= 0) snprintf(tp, sizeof tp, "%dC", c->temp[i]); else snprintf(tp, sizeof tp, "--C");
		snprintf(fixed, sizeof fixed, "c%-2d %5.1f%% %-7s %-4s ", i, c->pct[i], fr, tp);
		gw = iw - ncells(fixed);
		if (gw < 4) gw = 4;
		if (gw > 16) gw = 16;
		col = 0;
		span(&r, row, &col, tier_color(c->pct[i]), 0, fixed);
		graph(&r, row++, &col, &c->core_hist[i], gw, C_ACCENT, 1);
	}
}

static void draw_gpu(const App *A, int x, int y, int w, int h)
{
	const Gpu *g = &A->gpu;
	Rect r = block(x, y, w, h, "GPU");
	int iw = inner_width(w), row = 0, col = 0, bar_w;
	char suffix[64];
	double v[1];
	unsigned c[1];
	c[0] = C_PRIMARY;
	if (!g->found) {
		span(&r, 0, &col, C_DISABLED, 0, "x GPU (unavailable)");
		return;
	}
	snprintf(suffix, sizeof suffix, " %.0f%%", g->util);
	bar_w = iw - ncells(suffix);
	if (bar_w < 10) bar_w = 10;
	v[0] = g->util;
	segmented_bar(&r, row, &col, v, c, 1, 100.0, bar_w);
	span(&r, row, &col, C_PRIMARY, 0, suffix);
	row++;
	col = 0;
	graph(&r, row++, &col, &g->hist, iw, C_ACCENT, 1);
	if (g->total_mb > 0) {
		double pct = (double)g->used_mb / (double)g->total_mb * 100.0;
		int vw;
		snprintf(suffix, sizeof suffix, " %.0f%% %lluM/%lluM", pct, g->used_mb, g->total_mb);
		vw = iw - ncells(suffix) - 5;
		if (vw < 10) vw = 10;
		col = 0;
		span(&r, row, &col, C_PRIMARY, 0, "vram ");
		v[0] = pct;
		segmented_bar(&r, row, &col, v, c, 1, 100.0, vw);
		span(&r, row, &col, C_PRIMARY, 0, suffix);
	}
}

static void draw_ram(const App *A, int x, int y, int w, int h)
{
	const Mem *m = &A->mem;
	Rect r = block(x, y, w, h, "RAM");
	int iw = inner_width(w), row = 0, col = 0, bw;
	double other = m->total - m->free_ - m->cached - m->zram, swap_used = m->swap_total - m->swap_free;
	double free_comb = m->free_ + m->swap_free, grand = m->total + m->swap_total, used_total;
	double seg[5];
	unsigned sc[5];
	char s1[32], s2[32], summary[80];
	sc[0] = C_RAM_USED; sc[1] = C_RAM_ZRAM; sc[2] = C_RAM_CACHE; sc[3] = C_RAM_SWAP; sc[4] = C_RAM_FREE;
	if (other < 0) other = 0;
	if (swap_used < 0) swap_used = 0;
	seg[0] = other; seg[1] = m->zram; seg[2] = m->cached; seg[3] = swap_used; seg[4] = free_comb;
	used_total = grand - free_comb;
	human_bytes(used_total, s1, sizeof s1);
	human_bytes(free_comb, s2, sizeof s2);
	snprintf(summary, sizeof summary, " %s/%s", s1, s2);
	bw = iw - ncells(summary);
	if (bw < 10) bw = 10;
	segmented_bar(&r, row, &col, seg, sc, 5, grand, bw);
	span(&r, row, &col, C_PRIMARY, 0, summary);
	row++;
	col = 0;
	graph(&r, row++, &col, &m->used_hist, iw, C_ACCENT, 1);
	human_bytes(other, s1, sizeof s1);
	human_bytes(m->total, s2, sizeof s2);
	col = 0;
	spanf(&r, row++, &col, C_RAM_USED, 0, "used  %s/%s", s1, s2);
	if (m->has_zram) {
		human_bytes(m->zram, s1, sizeof s1);
		col = 0;
		spanf(&r, row++, &col, C_RAM_ZRAM, 0, "zram  %s/%s", s1, s2);
	}
	human_bytes(m->cached, s1, sizeof s1);
	col = 0;
	spanf(&r, row++, &col, C_RAM_CACHE, 0, "cache %s/%s", s1, s2);
	if (m->swap_total > 0) {
		char sw[32], sc2[32];
		human_bytes(swap_used, sw, sizeof sw);
		human_bytes(m->swap_total, sc2, sizeof sc2);
		col = 0;
		spanf(&r, row++, &col, C_RAM_SWAP, 0, "swap  %s/%s", sw, sc2);
		human_bytes(grand, s1, sizeof s1);
		col = 0;
		spanf(&r, row++, &col, C_PRIMARY, 0, "RAM+swap capacity: %s", s1);
	} else {
		col = 0;
		spanf(&r, row++, &col, C_PRIMARY, 0, "RAM capacity: %s", s2);
	}
	human_bytes(free_comb, s1, sizeof s1);
	human_bytes(grand, s2, sizeof s2);
	col = 0;
	spanf(&r, row++, &col, C_RAM_FREE, 0, "free  %s/%s", s1, s2);
}

static void usage_bar(const Rect *r, int row, int iw, double used, double total)
{
	double v[2];
	unsigned c[2];
	char s1[32], s2[32], sum[80];
	int col = 0, bw;
	c[0] = C_PRIMARY; c[1] = C_NEUTRAL;
	v[0] = used; v[1] = total - used;
	human_bytes(v[0], s1, sizeof s1);
	human_bytes(v[1], s2, sizeof s2);
	snprintf(sum, sizeof sum, " %s/%s", s1, s2);
	bw = iw - ncells(sum);
	if (bw < 10) bw = 10;
	segmented_bar(r, row, &col, v, c, 2, total, bw);
	span(r, row, &col, C_PRIMARY, 0, sum);
}

static void draw_disk(const App *A, int x, int y, int w, int h)
{
	const Disks *D = &A->disk;
	Rect r = block(x, y, w, h, "DISK");
	int iw = inner_width(w), row = 0, col, i, j;
	if (D->total > 0) {
		usage_bar(&r, row++, iw, D->used, D->total);
		col = 0;
		graph(&r, row++, &col, &D->agg, iw, C_ACCENT, 0);
	}
	if (D->n == 0) {
		col = 0;
		span(&r, row, &col, C_PRIMARY, 0, "(sampling devices...)");
		return;
	}
	for (i = 0; i < D->n; i++) {
		const Disk *d = &D->d[i];
		char ident[160], sz[32];
		col = 0;
		if (d->model[0] && !A->anonymous) snprintf(ident, sizeof ident, "%s (%s)", d->name, d->model);
		else snprintf(ident, sizeof ident, "%s", d->name);
		if (d->removable) strncat(ident, " [rm]", sizeof ident - strlen(ident) - 1);
		human_bytes(d->size, sz, sizeof sz);
		spanf(&r, row++, &col, d->removable ? C_CAUTION : C_PRIMARY, 0, "%s %s", ident, sz);
		if (d->has_usage) usage_bar(&r, row++, iw, d->dev_used, d->dev_total);
		col = 0;
		graph(&r, row++, &col, &d->hist, iw, C_ACCENT, 0);
		for (j = 0; j < d->npart; j++) {
			const Part *p = &d->part[j];
			int last = j == d->npart - 1;
			char ps[32];
			human_bytes(p->size, ps, sizeof ps);
			col = 0;
			spanf(&r, row, &col, C_PRIMARY, 0, "%s%s %s \xE2\x86\x92 %s",
			      last ? "\xE2\x94\x94\xE2\x94\x80\xE2\x94\x80 " : "\xE2\x94\x9C\xE2\x94\x80\xE2\x94\x80 ", p->name, ps,
			      p->has_mount ? p->mount : "not mounted");
			if (p->has_usage && p->total > 0) {
				double pct = p->used / p->total * 100.0, v[2];
				unsigned c[2];
				v[0] = pct; v[1] = 100.0 - pct;
				c[0] = tier_color(pct); c[1] = C_NEUTRAL;
				span(&r, row, &col, C_PLAIN, 0, " ");
				segmented_bar(&r, row, &col, v, c, 2, 100.0, 8);
				spanf(&r, row, &col, C_PRIMARY, 0, " %.0f%%", pct);
			}
			row++;
		}
	}
}

static void draw_net(const App *A, int x, int y, int w, int h)
{
	const Net *N = &A->net;
	Rect r = block(x, y, w, h, "NET");
	int iw = inner_width(w), row = 0, col = 0, i, a;
	char s1[32], s2[32];
	human_rate(hlast(&N->agg) / 2.0, s1, sizeof s1);
	human_rate(hlast(&N->agg) / 2.0, s2, sizeof s2);
	spanf(&r, row++, &col, C_PRIMARY, 0, "agg rx:%s tx:%s", s1, s2);
	col = 0;
	graph(&r, row++, &col, &N->agg, iw, C_ACCENT, 0);
	if (N->n == 0) {
		col = 0;
		span(&r, row++, &col, C_PRIMARY, 0, "(sampling interfaces...)");
	}
	for (i = 0; i < N->n; i++) {
		const Iface *f = &N->i[i];
		char speed[32];
		if (f->speed) snprintf(speed, sizeof speed, "%dMbps", f->speed); else snprintf(speed, sizeof speed, "?");
		human_rate(f->rx, s1, sizeof s1);
		human_rate(f->tx, s2, sizeof s2);
		col = 0;
		spanf(&r, row++, &col, C_PRIMARY, 0, "%s  rx:%s tx:%s  link:%s", f->name, s1, s2, speed);
		col = 0;
		if (f->speed) {
			double cap = (double)f->speed * 1e6 / 8.0, util = (f->rx + f->tx) / cap * 100.0, v[2];
			unsigned c[2];
			char suf[32];
			int bw;
			v[0] = f->rx; v[1] = f->tx;
			c[0] = C_PRIMARY; c[1] = C_CYAN;
			snprintf(suf, sizeof suf, " %3.0f%%", util);
			bw = iw - ncells(suf);
			if (bw < 8) bw = 8;
			segmented_bar(&r, row, &col, v, c, 2, cap, bw);
			span(&r, row, &col, C_PRIMARY, 0, suf);
			row++;
		} else graph(&r, row++, &col, &f->hist, iw, C_ACCENT, 0);
		col = 0;
		if (A->anonymous) span(&r, row++, &col, C_NEUTRAL, 0, "  [hidden - anonymous mode]");
		else if (f->naddr) {
			char b[600] = "  ";
			for (a = 0; a < f->naddr; a++) {
				if (a) strncat(b, ", ", sizeof b - strlen(b) - 1);
				strncat(b, f->addr[a], sizeof b - strlen(b) - 1);
			}
			span(&r, row++, &col, C_ACCENT, 0, b);
		}
	}
}

static void draw_proc(const App *A, int x, int y, int w, int h)
{
	const Procs *P = &A->procs;
	static const char *labels[] = {"app", "raw", "user/system"};
	char title[64], hdr[256];
	Rect r;
	int show_gpu = A->gpu.found, visible = h - 4, total = P->nrows, start = 0, end, i, row = 0, col;
	if (visible < 1) visible = 1;
	snprintf(title, sizeof title, "PROC [%s]", labels[P->mode]);
	r = block(x, y, w, h, title);
	if (total > visible) {
		start = P->cursor - (visible - 1);
		if (start < 0) start = 0;
		if (start > total - visible) start = total - visible;
	}
	end = start + visible < total ? start + visible : total;
	snprintf(hdr, sizeof hdr, "%-3s%-24s%6s %6s %9s %6s %10s", "", "NAME", "CPU%", "MEM%", "MEM", "DISK%", "DISK");
	if (show_gpu) { size_t l = strlen(hdr); snprintf(hdr + l, sizeof hdr - l, "%6s %8s", "GPU%", "GPU"); }
	{ size_t l = strlen(hdr); snprintf(hdr + l, sizeof hdr - l, " %s", "ACTIVITY"); }
	col = 0;
	span(&r, row++, &col, C_ACCENT, 0, hdr);
	for (i = start; i < end; i++) {
		const Row *rw = &P->rows[i];
		const Group *g = &P->g[rw->g];
		const Child *kd = rw->kid >= 0 ? &g->kids[rw->kid] : NULL;
		char label[200], cell[200], key[96], rate[40], fixed[400];
		unsigned style;
		int fl = 0, hi, pad, k = 0, bytes = 0, maxc;
		const char *s;
		const NamedHist *hist = NULL;
		double cpu, mem_pct, disk_pct, disk_bps, gpu_pct;
		unsigned long long mem_mb, gpu_mb;
		col = 0;
		if (g->divider) { span(&r, row++, &col, C_ACCENT, 0, g->name); continue; }
		if (kd) {
			int last = rw->kid == g->nkids - 1;
			snprintf(label, sizeof label, "%s%.60s [%d]", last ? "\xE2\x94\x94\xE2\x94\x80\xE2\x94\x80 " : "\xE2\x94\x9C\xE2\x94\x80\xE2\x94\x80 ", g->name, kd->pid);
			maxc = 24;
			cpu = kd->cpu; mem_mb = kd->mem_mb; disk_bps = kd->disk_bps; gpu_pct = kd->gpu_pct; gpu_mb = kd->gpu_mb;
			mem_pct = g->mem_mb ? g->mem_pct * (double)kd->mem_mb / (double)g->mem_mb : 0;
			disk_pct = g->disk_bps > 0 ? g->disk_pct * kd->disk_bps / g->disk_bps : 0;
		} else {
			const char *glyph = expandable(P, g) ? (is_expanded(P, g->name) ? "\xE2\x96\xBE " : "\xE2\x96\xB8 ") : "  ";
			if (g->count > 1 && P->mode != 1) snprintf(label, sizeof label, "%s%s (%d)", glyph, g->name, g->count);
			else snprintf(label, sizeof label, "%s%s", glyph, g->name);
			maxc = 24;
			cpu = g->cpu; mem_mb = g->mem_mb; disk_bps = g->disk_bps; gpu_pct = g->gpu_pct; gpu_mb = g->gpu_mb;
			mem_pct = g->mem_pct; disk_pct = g->disk_pct;
		}
		s = label;
		while (*s && k < maxc) { int l = u8len((unsigned char)*s); bytes += l; s += l; k++; }
		snprintf(cell, sizeof cell, "%.*s", bytes, label);
		pad = 24 - ncells(cell);
		row_key(P, rw, key, sizeof key);
		human_rate(disk_bps, rate, sizeof rate);
		snprintf(fixed, sizeof fixed, "%-2s %s%*s%5.1f%% %5.1f%% %8lluM %5.1f%% %10s", is_marked(P, key) ? "*" : " ", cell,
		         pad > 0 ? pad : 0, "", cpu, mem_pct, mem_mb, disk_pct, rate);
		if (g->ghost) style = C_GHOST;
		else {
			style = tier_color(cpu);
			if (i == P->cursor) fl = F_REV | F_BOLD;
		}
		span(&r, row, &col, style, fl, fixed);
		if (show_gpu) spanf(&r, row, &col, style, fl, "%5.1f%% %8lluM", gpu_pct, gpu_mb);
		span(&r, row, &col, style, fl, " ");
		for (hi = 0; hi < P->nhist; hi++) if (!strcmp(P->hist[hi].name, key)) { hist = &P->hist[hi]; break; }
		{
			int gw = r.w - col;
			if (gw < 4) gw = 4;
			if (hist) graph_v(&r, row, &col, hist->v, hist->n, gw, g->ghost ? C_GHOST : C_ACCENT, 1);
			else repeat(&r, row, &col, C_PLAIN, " ", gw);
		}
		row++;
	}
	{
		char scroll[48] = "", hint[400];
		if (total > visible) snprintf(scroll, sizeof scroll, " [%d-%d/%d]", start + 1, end, total);
		snprintf(hint, sizeof hint,
		         "\xE2\x86\x91/\xE2\x86\x93 select%s  \xE2\x86\x90\xE2\x86\x92 mode  space: mark  enter: expand  k: kill  /: command  +/-: refresh %dms  r: record  a: anonymous",
		         scroll, A->interval_ms);
		col = 0;
		span(&r, row++, &col, C_ACCENT, 0, hint);
	}
	if (A->anonymous) {
		col = 0;
		span(&r, row++, &col, C_ACCENT, F_BOLD, "\xE2\x97\x8F anonymous mode - IPs and disk model/serial hidden");
	}
	if (A->recording) {
		col = 0;
		spanf(&r, row++, &col, C_WARN, F_BOLD, "\xE2\x97\x8F recording to %s", A->rec_path);
	}
	if (A->has_msg && now_s() - A->msg_at < 5.0) {
		col = 0;
		span(&r, row++, &col, C_CAUTION, 0, A->msg);
	}
	if (A->has_palette_out) {
		col = 0;
		span(&r, row++, &col, C_CAUTION, 0, A->palette_out);
	}
}

static void draw_palette(const App *A, int W, int H)
{
	int w = W * 3 / 4, px, py, col = 0;
	Rect r;
	if (w < 20) w = 20;
	px = (W - w) / 2 > 0 ? (W - w) / 2 : 0;
	py = H / 2;
	r = block(px, py, w, 3, "command (Enter: run, Esc: cancel)");
	spanf(&r, 0, &col, C_PRIMARY, 0, "/%s", A->palette_in);
}

static void draw_all(const App *A, int W, int H)
{
	int col_a_h = cpu_box_lines(A) < H ? cpu_box_lines(A) : H;
	int col_b_h = ram_col_height(A) < H ? ram_col_height(A) : H;
	int disk_h = disk_content_lines(A) < H ? disk_content_lines(A) : H;
	int net_h = net_content_lines(A) < H ? net_content_lines(A) : H;
	int top_h = col_a_h, wa = W * 20 / 100, wb = W * 20 / 100, wc = W * 30 / 100, wd;
	int ry, rem, hh;
	if (col_b_h > top_h) top_h = col_b_h;
	if (disk_h > top_h) top_h = disk_h;
	if (net_h > top_h) top_h = net_h;
	if (top_h < 10) top_h = 10;
	wd = W - wa - wb - wc;
	if (wd < 0) wd = 0;
	draw_cpu(A, 0, 0, wa, col_a_h);
	/* column b: RAM, GPU, optional TPU / NPU, each a fixed length clipped to what is left */
	ry = 0;
	rem = col_b_h;
	hh = ram_box_lines() < rem ? ram_box_lines() : rem;
	draw_ram(A, wa, ry, wb, hh);
	ry += hh; rem -= hh;
	hh = gpu_box_lines(A) < rem ? gpu_box_lines(A) : rem;
	draw_gpu(A, wa, ry, wb, hh);
	ry += hh; rem -= hh;
	if (A->has_tpu) {
		Rect r;
		int c = 0;
		hh = 3 < rem ? 3 : rem;
		r = block(wa, ry, wb, hh, "TPU");
		span(&r, 0, &c, C_PRIMARY, 0, A->tpu);
		ry += hh; rem -= hh;
	}
	if (A->has_npu) {
		Rect r;
		int c = 0;
		hh = 3 < rem ? 3 : rem;
		r = block(wa, ry, wb, hh, "NPU");
		span(&r, 0, &c, C_PRIMARY, 0, A->npu);
	}
	draw_disk(A, wa + wb, 0, wc, disk_h);
	draw_net(A, wa + wb + wc, 0, wd, net_h);
	if (H > top_h) draw_proc(A, 0, top_h, W, H - top_h);
	if (A->palette) draw_palette(A, W, H);
}

static void emit_ansi(FILE *o)
{
	int y, x;
	unsigned last_fg = 0xFFFFFFFFu;
	int last_fl = -1;
	fputs("\x1b[H", o);
	for (y = 0; y < GH; y++) {
		for (x = 0; x < GW; x++) {
			const Cell *c = &GRID[y * GW + x];
			if (c->fg != last_fg || c->fl != last_fl) {
				fputs("\x1b[0m", o);
				if (c->fg & 0x01000000u) {
					unsigned n = c->fg & 0xFFu;
					if (n == 9) fputs("\x1b[39m", o);
					else fprintf(o, "\x1b[3%um", n);
				} else fprintf(o, "\x1b[38;2;%u;%u;%um", (c->fg >> 16) & 255, (c->fg >> 8) & 255, c->fg & 255);
				if (c->fl & F_BOLD) fputs("\x1b[1m", o);
				if (c->fl & F_REV) fputs("\x1b[7m", o);
				last_fg = c->fg;
				last_fl = c->fl;
			}
			fputs(c->s, o);
		}
		if (y + 1 < GH) fputs("\r\n", o);
	}
	fputs("\x1b[0m", o);
	fflush(o);
}

static void emit_plain(FILE *o)
{
	int y, x;
	for (y = 0; y < GH; y++) {
		int last = GW - 1;
		while (last >= 0 && GRID[y * GW + last].s[0] == ' ') last--;
		for (x = 0; x <= last; x++) fputs(GRID[y * GW + x].s, o);
		fputc('\n', o);
	}
}

/* ------------------------------------------------------------------ terminal + main */
static struct termios ORIG;
static int RAW;
static void term_restore(void)
{
	if (RAW) {
		tcsetattr(0, TCSAFLUSH, &ORIG);
		fputs("\x1b[?25h\x1b[?1049l", stdout);
		fflush(stdout);
		RAW = 0;
	}
}
static void on_signal(int s) { (void)s; term_restore(); _exit(130); }

static int read_key(void)
{
	unsigned char c;
	if (read(0, &c, 1) != 1) return 0;
	if (c == 27) {
		struct pollfd p = {0, POLLIN, 0};
		unsigned char s[2];
		if (poll(&p, 1, 20) <= 0) return K_ESC;
		if (read(0, &s[0], 1) != 1) return K_ESC;
		if (s[0] != '[' && s[0] != 'O') return K_ESC;
		if (read(0, &s[1], 1) != 1) return K_ESC;
		switch (s[1]) {
		case 'A': return K_UP;
		case 'B': return K_DOWN;
		case 'C': return K_RIGHT;
		case 'D': return K_LEFT;
		}
		return -1;
	}
	if (c == '\r' || c == '\n') return K_ENTER;
	if (c == 127 || c == 8) return K_BACKSPACE;
	return c;
}

static int key_from_char(char c)
{
	switch (c) {
	case 'U': return K_UP;
	case 'D': return K_DOWN;
	case 'L': return K_LEFT;
	case 'R': return K_RIGHT;
	case 'E': return K_ENTER;
	}
	return (unsigned char)c;
}

static void usage(void)
{
	puts("usage: gtop [-f SUBSTR] [--snapshot [W H]] [--keys KEYS]\n"
	     "  q quit   left/right view (app / raw / user+system)   up/down move   space mark\n"
	     "  k kill (SIGKILL) marked or current row   / command palette   r record to CSV\n"
	     "  a anonymous mode   +/- refresh interval (100 ms .. 5 s)\n"
	     "  --snapshot  draw one frame as plain text and exit\n"
	     "  --keys      feed keys before the snapshot: U D L R = arrows, E = enter, others literal\n"
	     "  -f          keep only processes whose name contains SUBSTR\n"
	     "  palette commands: $GTOP_COMMANDS, ./commands, <binary dir>/commands");
}

int main(int argc, char **argv)
{
	App *A = calloc(1, sizeof *A);
	int snapshot = 0, W = 120, H = 40, i;
	const char *keys = NULL;

	A->interval_ms = 1000;
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(); return 0; }
		else if (!strcmp(argv[i], "--snapshot")) {
			snapshot = 1;
			if (i + 2 < argc && atoi(argv[i + 1]) > 0 && atoi(argv[i + 2]) > 0) { W = atoi(argv[i + 1]); H = atoi(argv[i + 2]); i += 2; }
		} else if (!strcmp(argv[i], "--keys") && i + 1 < argc) keys = argv[++i];
		else if (!strcmp(argv[i], "-f") && i + 1 < argc) snprintf(A->procs.filter, sizeof A->procs.filter, "%s", argv[++i]);
		else { fprintf(stderr, "gtop: unknown option %s\n", argv[i]); return 2; }
	}
	if (W > 500) W = 500;
	if (H > 300) H = 300;
	if (access("/dev/apex_0", F_OK) == 0) { A->has_tpu = 1; snprintf(A->tpu, sizeof A->tpu, "/dev/apex_0 (PCIe)"); }
	else if (which("lsusb")) {
		char out[16384];
		if (run_capture("lsusb 2>/dev/null", out, sizeof out) > 0 && (strstr(out, "1a6e:089a") || strstr(out, "18d1:9302"))) {
			A->has_tpu = 1;
			snprintf(A->tpu, sizeof A->tpu, "USB Accelerator (Coral)");
		}
	}
	{
		DIR *d = opendir("/dev/accel");
		struct dirent *e;
		while (d && (e = readdir(d)))
			if (!strncmp(e->d_name, "accel", 5)) { A->has_npu = 1; snprintf(A->npu, sizeof A->npu, "/dev/accel/%.100s", e->d_name); break; }
		if (d) closedir(d);
	}

	app_tick(A);
	if (snapshot) {
		struct timespec ts = {0, 300000000};
		nanosleep(&ts, NULL);
		app_tick(A);
		if (keys) {
			for (; *keys; keys++) handle_key(A, key_from_char(*keys));
			nanosleep(&ts, NULL);
			app_tick(A);
		}
		if (A->rec) { fclose(A->rec); A->rec = NULL; }
		grid_init(W, H);
		draw_all(A, W, H);
		emit_plain(stdout);
		return 0;
	}

	if (!isatty(0) || !isatty(1)) { fprintf(stderr, "gtop: needs a terminal (use --snapshot for a text frame)\n"); return 1; }
	tcgetattr(0, &ORIG);
	{
		struct termios t = ORIG;
		cfmakeraw(&t);
		t.c_cc[VMIN] = 0;
		t.c_cc[VTIME] = 0;
		tcsetattr(0, TCSAFLUSH, &t);
		RAW = 1;
		atexit(term_restore);
		signal(SIGINT, on_signal);
		signal(SIGTERM, on_signal);
		fputs("\x1b[?1049h\x1b[?25l", stdout);
	}
	{
		double last = now_s();
		struct winsize ws;
		while (!A->quit) {
			struct pollfd p = {0, POLLIN, 0};
			double wait = (double)A->interval_ms / 1000.0 - (now_s() - last);
			int ms = wait > 0 ? (int)(wait * 1000.0) : 0;
			if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) { W = ws.ws_col; H = ws.ws_row; }
			if (W > 500) W = 500;
			if (H > 300) H = 300;
			grid_init(W, H);
			draw_all(A, W, H);
			emit_ansi(stdout);
			if (poll(&p, 1, ms) > 0 && (p.revents & POLLIN)) {
				int k;
				while ((k = read_key()) != 0) {
					struct pollfd q = {0, POLLIN, 0};
					if (k > 0) handle_key(A, k);
					if (poll(&q, 1, 0) <= 0) break;
				}
			}
			if (now_s() - last >= (double)A->interval_ms / 1000.0) { app_tick(A); last = now_s(); }
		}
	}
	term_restore();
	return 0;
}
