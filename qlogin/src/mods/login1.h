/* login1.h - the org.freedesktop.login1 service model of qlogind. stb-style, one header (needs qdbus.h first).
 *
 *   #define LOGIN1_IMPLEMENTATION   in exactly one .c file
 *
 * One table per interface (methods + properties + signals) drives dispatch, Properties.Get/GetAll and the generated introspection
 * XML, so documentation and behaviour cannot drift. Scope of what exists: docs/login1-surface.md.
 *
 * Env (tests and packaging): QLOGIND_RUN (default /run), QLOGIND_CONF (default /etc/qlogind.conf), QLOGIND_VTFILE (default
 * /sys/class/tty/tty0/active), QLOGIND_DEVDIR (mock device dir: "<major>:<minor>" files; unset = /dev/char), QLOGIND_NAME
 * (bus name, default org.freedesktop.login1 - tests only).
 */
#ifndef LOGIN1_H
#define LOGIN1_H
#include <stdint.h>
#include <sys/types.h>
#include "qdbus.h"

#define L1_MAXDEV 16
typedef struct { unsigned maj, min; int fd; int paused; } l1_dev;

typedef struct {
	char id[16], name[48], seat[16], type[16], class_[16], tty[32], display[16], service[32], desktop[32];
	char remote_host[64], remote_user[48], controller[48];
	uid_t uid; pid_t leader; unsigned vtnr; int remote, idle, locked, active, closing, fifo;   /* fifo: our read end */
	uint64_t created, idle_since;
	l1_dev dev[L1_MAXDEV]; int ndev;
} l1_session;

typedef struct { char what[96], who[64], why[128], mode[8]; uid_t uid; pid_t pid; int fd; uint64_t created; } l1_inhibit;

typedef struct {
	int real_power, delay_max_s;
	char cmd[8][256];
} l1_config;

typedef struct { int kind; l1_session *s; uid_t uid; } l1_obj;
enum { L1_MANAGER = 1, L1_SESSION, L1_SEAT, L1_USER, L1_NODE };

typedef struct l1_ctx {
	qd_conn *c; const qd_msg *m; qd_rd in; qd_buf out; const char *osig;
	int fds[2]; int nfds; l1_obj obj; uid_t cuid; pid_t cpid; int have_creds;
	const char *errname; char errtext[200];
} l1_ctx;

int l1_init(void);                               /* reads env + config, creates dirs; -1 on error */
int l1_run(qd_conn *c, volatile int *stop);      /* serve until *stop; c must already own the bus name */
int l1_handle(qd_conn *c, const qd_msg *m);      /* one incoming message (exposed for the loop) */
const char *l1_bus_name(void);
#endif /* LOGIN1_H */

#ifdef LOGIN1_IMPLEMENTATION
#ifndef LOGIN1_IMPLEMENTATION_DONE
#define LOGIN1_IMPLEMENTATION_DONE
#ifndef _GNU_SOURCE
#error "define _GNU_SOURCE before the first system include when using LOGIN1_IMPLEMENTATION"
#endif
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <pwd.h>

#define L1_PATH "/org/freedesktop/login1"
#define L1_MAXSESS 64
#define L1_MAXINH 128
#define L1_MAXPEND 8

static struct {
	l1_config cfg;
	char run[160], vtfile[200], devdir[200], name[80];
	l1_session sess[L1_MAXSESS]; int nsess;
	l1_inhibit inh[L1_MAXINH]; int ninh;
	unsigned next_id;
	int curvt, preparing_shutdown, preparing_sleep;
	struct { char who[48]; uid_t uid; pid_t pid; } creds[64]; int ncreds;
	struct { int kind; uint64_t deadline; int on; } pend;
	qd_conn *c;
} L1;

static uint64_t l1_now_us(clockid_t id) { struct timespec t; clock_gettime(id, &t); return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000; }
const char *l1_bus_name(void) { return L1.name; }
static void l1_log(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("qlogind: ", stderr); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
	va_end(ap);
}

/* ---- config ---- */
static void l1_cfg_set(const char *k, const char *v)
{
	static const char *const names[] = { "cmd_poweroff", "cmd_reboot", "cmd_halt", "cmd_suspend", "cmd_hibernate", "cmd_hybrid", "cmd_suspendhibernate" };
	if (!strcmp(k, "real_power")) L1.cfg.real_power = atoi(v) != 0;
	else if (!strcmp(k, "inhibit_delay_max")) { L1.cfg.delay_max_s = atoi(v); if (L1.cfg.delay_max_s < 0 || L1.cfg.delay_max_s > 300) L1.cfg.delay_max_s = 5; }
	else for (unsigned i = 0; i < sizeof names / sizeof *names; i++) if (!strcmp(k, names[i])) snprintf(L1.cfg.cmd[i], sizeof L1.cfg.cmd[i], "%s", v);
}

static void l1_cfg_load(const char *path)
{
	FILE *f = fopen(path, "r");
	char line[400];
	if (!f) return;
	while (fgets(line, sizeof line, f)) {
		char *eq, *e;
		if (line[0] == '#' || !(eq = strchr(line, '='))) continue;
		*eq++ = 0;
		for (e = eq + strlen(eq); e > eq && isspace((unsigned char)e[-1]); e--) e[-1] = 0;
		l1_cfg_set(line, eq);
	}
	fclose(f);
}

static void l1_mkdirs(const char *p)
{
	char t[256];
	snprintf(t, sizeof t, "%s", p);
	for (char *s = t + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(t, 0755); *s = '/'; }
	mkdir(t, 0755);
}

int l1_init(void)
{
	const char *e;
	memset(&L1, 0, sizeof L1);
	L1.cfg.delay_max_s = 5;
	snprintf(L1.run, sizeof L1.run, "%s", (e = getenv("QLOGIND_RUN")) && *e ? e : "/run");
	snprintf(L1.vtfile, sizeof L1.vtfile, "%s", (e = getenv("QLOGIND_VTFILE")) && *e ? e : "/sys/class/tty/tty0/active");
	snprintf(L1.devdir, sizeof L1.devdir, "%s", (e = getenv("QLOGIND_DEVDIR")) ? e : "");
	snprintf(L1.name, sizeof L1.name, "%s", (e = getenv("QLOGIND_NAME")) && *e ? e : "org.freedesktop.login1");
	l1_cfg_load((e = getenv("QLOGIND_CONF")) && *e ? e : "/etc/qlogind.conf");
	L1.next_id = 1;
	{ char d[260]; for (const char *s = "systemd/sessions"; s; s = NULL) { snprintf(d, sizeof d, "%s/%s", L1.run, s); l1_mkdirs(d); }
	  snprintf(d, sizeof d, "%s/systemd/seats", L1.run); l1_mkdirs(d);
	  snprintf(d, sizeof d, "%s/systemd/users", L1.run); l1_mkdirs(d);
	  snprintf(d, sizeof d, "%s/user", L1.run); l1_mkdirs(d);
	  snprintf(d, sizeof d, "%s/qlogind", L1.run); l1_mkdirs(d); }
	return 0;
}

/* ---- VT / activity ---- */
static int l1_read_vt(void)
{
	char b[32] = "";
	FILE *f = fopen(L1.vtfile, "r");
	if (!f) return 0;
	if (!fgets(b, sizeof b, f)) b[0] = 0;
	fclose(f);
	return strncmp(b, "tty", 3) == 0 ? atoi(b + 3) : 0;
}

static l1_session *l1_find(const char *id)
{
	for (int i = 0; i < L1.nsess; i++) if (!strcmp(L1.sess[i].id, id)) return &L1.sess[i];
	return NULL;
}

/* which session on the seat is the active one: the one on the current VT, else the newest VT-less one */
static l1_session *l1_seat_active(const char *seat)
{
	l1_session *best = NULL;
	for (int i = 0; i < L1.nsess; i++) {
		l1_session *s = &L1.sess[i];
		if (strcmp(s->seat, seat) || s->closing) continue;
		if (s->vtnr && (int)s->vtnr == L1.curvt) return s;
		if (!s->vtnr) best = s;
	}
	return best;
}

/* ---- paths ---- */
static void l1_escape(const char *in, char *out, size_t n)
{
	size_t o = 0;
	if (!*in) { snprintf(out, n, "_"); return; }
	for (const char *s = in; *s && o + 4 < n; s++)
		if (isalnum((unsigned char)*s) && !(s == in && isdigit((unsigned char)*s))) out[o++] = *s;
		else o += snprintf(out + o, n - o, "_%02x", (unsigned char)*s);
	out[o] = 0;
}
static int l1_unescape(const char *in, char *out, size_t n)
{
	size_t o = 0;
	if (!strcmp(in, "_")) { out[0] = 0; return 0; }
	for (const char *s = in; *s; s++) {
		if (o + 1 >= n) return -1;
		if (*s == '_') {
			unsigned v;
			if (!isxdigit((unsigned char)s[1]) || !isxdigit((unsigned char)s[2]) || sscanf(s + 1, "%2x", &v) != 1) return -1;
			out[o++] = v; s += 2;
		} else out[o++] = *s;
	}
	out[o] = 0;
	return 0;
}
static void l1_sess_path(const l1_session *s, char *o, size_t n) { char e[64]; l1_escape(s->id, e, sizeof e); snprintf(o, n, L1_PATH "/session/%s", e); }
static void l1_user_path(uid_t u, char *o, size_t n) { char e[24], t[24]; snprintf(t, sizeof t, "%u", (unsigned)u); l1_escape(t, e, sizeof e); snprintf(o, n, L1_PATH "/user/%s", e); }
static void l1_seat_path(const char *seat, char *o, size_t n) { char e[40]; l1_escape(seat, e, sizeof e); snprintf(o, n, L1_PATH "/seat/%s", e); }

/* ---- user / pid lookup ---- */
static const char *l1_user_name(uid_t uid, char *buf, size_t n)
{
	struct passwd *p = getpwuid(uid);
	snprintf(buf, n, "%s", p ? p->pw_name : "?");
	return buf;
}
static gid_t l1_user_gid(uid_t uid) { struct passwd *p = getpwuid(uid); return p ? p->pw_gid : (gid_t)-1; }

static pid_t l1_ppid(pid_t pid)
{
	char p[48], b[512];
	FILE *f;
	char *r;
	int ppid = 0;
	snprintf(p, sizeof p, "/proc/%d/stat", (int)pid);
	if (!(f = fopen(p, "r"))) return 0;
	if (!fgets(b, sizeof b, f)) b[0] = 0;
	fclose(f);
	r = strrchr(b, ')');
	if (r && sscanf(r + 1, " %*c %d", &ppid) != 1) ppid = 0;
	return ppid;
}

/* cgroup-less membership: the session whose leader is pid or an ancestor of pid */
static l1_session *l1_by_pid(pid_t pid)
{
	for (int depth = 0; pid > 1 && depth < 64; depth++) {
		for (int i = 0; i < L1.nsess; i++) if (L1.sess[i].leader == pid && !L1.sess[i].closing) return &L1.sess[i];
		pid = l1_ppid(pid);
	}
	return NULL;
}

/* ---- sd_login state files ---- */
static void l1_write_file(const char *path, const char *text)
{
	char tmp[300];
	FILE *f;
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	if (!(f = fopen(tmp, "w"))) return;
	fputs(text, f);
	if (fclose(f) == 0) rename(tmp, path); else unlink(tmp);
}

static void l1_user_runtime(uid_t uid, char *o, size_t n) { snprintf(o, n, "%s/user/%u", L1.run, (unsigned)uid); }

static void l1_sync_files(void)
{
	char path[300], txt[2048], nm[48];
	DIR *d;
	struct dirent *de;
	/* sessions */
	for (int i = 0; i < L1.nsess; i++) {
		l1_session *s = &L1.sess[i];
		l1_session *act = l1_seat_active(s->seat);
		snprintf(txt, sizeof txt, "# This is private data. Do not parse.\nUID=%u\nUSER=%s\nACTIVE=%d\nIS_DISPLAY=1\nSTATE=%s\nREMOTE=%d\nSERVICE=%s\nDESKTOP=%s\nSEAT=%s\nVTNR=%u\nTTY=%s\nTYPE=%s\nCLASS=%s\nLEADER=%d\n",
		         (unsigned)s->uid, s->name, s->active, s->closing ? "closing" : s->active ? "active" : "online", s->remote, s->service, s->desktop, s->seat, s->vtnr, s->tty, s->type, s->class_, (int)s->leader);
		(void)act;
		snprintf(path, sizeof path, "%s/systemd/sessions/%s", L1.run, s->id);
		l1_write_file(path, txt);
	}
	snprintf(path, sizeof path, "%s/systemd/sessions", L1.run);
	if ((d = opendir(path))) {   /* drop files of sessions that are gone */
		while ((de = readdir(d))) {
			if (de->d_name[0] == '.') continue;
			if (!l1_find(de->d_name) && !strstr(de->d_name, ".tmp")) { snprintf(txt, sizeof txt, "%s/%s", path, de->d_name); unlink(txt); }
		}
		closedir(d);
	}
	/* seat0 */
	{
		l1_session *a = l1_seat_active("seat0");
		size_t o = 0;
		o += snprintf(txt + o, sizeof txt - o, "# This is private data. Do not parse.\nIS_SEAT0=1\nCAN_MULTI_SESSION=1\nCAN_TTY=1\nCAN_GRAPHICAL=1\n");
		if (a) o += snprintf(txt + o, sizeof txt - o, "ACTIVE=%s\nACTIVE_UID=%u\n", a->id, (unsigned)a->uid);
		o += snprintf(txt + o, sizeof txt - o, "SESSIONS=");
		for (int i = 0; i < L1.nsess; i++) if (!strcmp(L1.sess[i].seat, "seat0")) o += snprintf(txt + o, sizeof txt - o, "%s ", L1.sess[i].id);
		o += snprintf(txt + o, sizeof txt - o, "\nUIDS=");
		for (int i = 0; i < L1.nsess; i++) if (!strcmp(L1.sess[i].seat, "seat0")) o += snprintf(txt + o, sizeof txt - o, "%u ", (unsigned)L1.sess[i].uid);
		snprintf(txt + o, sizeof txt - o, "\n");
		snprintf(path, sizeof path, "%s/systemd/seats/seat0", L1.run);
		l1_write_file(path, txt);
	}
	/* users: one file per uid with a session */
	snprintf(path, sizeof path, "%s/systemd/users", L1.run);
	if ((d = opendir(path))) {
		while ((de = readdir(d))) {
			unsigned u;
			int have = 0;
			if (de->d_name[0] == '.' || sscanf(de->d_name, "%u", &u) != 1) continue;
			for (int i = 0; i < L1.nsess; i++) if (L1.sess[i].uid == u) have = 1;
			if (!have) { snprintf(txt, sizeof txt, "%s/%s", path, de->d_name); unlink(txt); }
		}
		closedir(d);
	}
	for (int i = 0; i < L1.nsess; i++) {
		uid_t u = L1.sess[i].uid;
		size_t o;
		int first = 1, act = 0;
		char rt[200];
		for (int j = 0; j < i; j++) if (L1.sess[j].uid == u) first = 0;
		if (!first) continue;
		for (int j = 0; j < L1.nsess; j++) if (L1.sess[j].uid == u && L1.sess[j].active) act = 1;
		l1_user_runtime(u, rt, sizeof rt);
		o = snprintf(txt, sizeof txt, "# This is private data. Do not parse.\nNAME=%s\nSTATE=%s\nRUNTIME_PATH=%s\nSESSIONS=", l1_user_name(u, nm, sizeof nm), act ? "active" : "online", rt);
		for (int j = 0; j < L1.nsess; j++) if (L1.sess[j].uid == u) o += snprintf(txt + o, sizeof txt - o, "%s ", L1.sess[j].id);
		snprintf(txt + o, sizeof txt - o, "\nDISPLAY=%s\n", L1.sess[i].id);
		snprintf(path, sizeof path, "%s/systemd/users/%u", L1.run, (unsigned)u);
		l1_write_file(path, txt);
	}
}

/* ---- signals ---- */
static void l1_sig_so(const char *member, const char *id, const char *path)
{
	qd_buf b = {0};
	qd_put_str(&b, id); qd_put_str(&b, path);
	qd_send_signal(L1.c, NULL, L1_PATH, "org.freedesktop.login1.Manager", member, "so", &b);
	free(b.b);
}

static void l1_sig_uo(const char *member, uid_t uid)
{
	qd_buf b = {0};
	char p[80];
	l1_user_path(uid, p, sizeof p);
	qd_put_u32(&b, uid); qd_put_str(&b, p);
	qd_send_signal(L1.c, NULL, L1_PATH, "org.freedesktop.login1.Manager", member, "uo", &b);
	free(b.b);
}

static void l1_sig_bool(const char *member, int v)
{
	qd_buf b = {0};
	qd_put_u32(&b, v);
	qd_send_signal(L1.c, NULL, L1_PATH, "org.freedesktop.login1.Manager", member, "b", &b);
	free(b.b);
}

static void l1_session_changed(const l1_session *s, int active, int state, int idle, int locked)
{
	qd_buf b = {0};
	char p[100];
	qd_arr a;
	l1_sess_path(s, p, sizeof p);
	qd_put_str(&b, "org.freedesktop.login1.Session");
	a = qd_arr_begin(&b, 8);
	if (active) { qd_align(&b, 8); qd_put_str(&b, "Active"); qd_variant_bool(&b, s->active); }
	if (state) { qd_align(&b, 8); qd_put_str(&b, "State"); qd_variant_str(&b, s->closing ? "closing" : s->active ? "active" : "online"); }
	if (idle) { qd_align(&b, 8); qd_put_str(&b, "IdleHint"); qd_variant_bool(&b, s->idle); }
	if (locked) { qd_align(&b, 8); qd_put_str(&b, "LockedHint"); qd_variant_bool(&b, s->locked); }
	qd_arr_end(&b, a);
	a = qd_arr_begin(&b, 4);   /* invalidated: none */
	qd_arr_end(&b, a);
	qd_send_signal(L1.c, NULL, p, "org.freedesktop.DBus.Properties", "PropertiesChanged", "sa{sv}as", &b);
	free(b.b);
}

/* ---- devices (mock device layer: QLOGIND_DEVDIR) ---- */
static int l1_dev_open(unsigned maj, unsigned min)
{
	char p[300];
	if (L1.devdir[0]) snprintf(p, sizeof p, "%s/%u:%u", L1.devdir, maj, min);
	else {
		if (maj != 13 && maj != 226) { errno = EPERM; return -1; }   /* input (evdev) and DRM only */
		snprintf(p, sizeof p, "/dev/char/%u:%u", maj, min);
	}
	return open(p, O_RDWR | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
}

/* PauseDevice(uus) is a signal addressed to the controller; ResumeDevice(uuh) carries an fd (signals with fds need a raw send) */
static void l1_send_pause(const l1_session *s, const l1_dev *d)
{
	qd_buf b = {0};
	char p[100];
	l1_sess_path(s, p, sizeof p);
	qd_put_u32(&b, d->maj); qd_put_u32(&b, d->min); qd_put_str(&b, "pause");
	qd_send_signal(L1.c, s->controller, p, "org.freedesktop.login1.Session", "PauseDevice", "uus", &b);
	free(b.b);
}


static void l1_release_control(l1_session *s)
{
	for (int i = 0; i < s->ndev; i++) if (s->dev[i].fd >= 0) close(s->dev[i].fd);
	s->ndev = 0;
	s->controller[0] = 0;
}

static void l1_apply_active(l1_session *s, int active)
{
	if (s->active == active) return;
	s->active = active;
	if (s->controller[0]) for (int i = 0; i < s->ndev; i++) {
		l1_dev *d = &s->dev[i];
		char p[100];
		l1_sess_path(s, p, sizeof p);
		if (!active) { d->paused = 1; l1_send_pause(s, d); }
		else {   /* ResumeDevice(uuh): the fd travels with the signal, addressed to the controller only */
			qd_buf b = {0};
			d->paused = 0;
			qd_put_u32(&b, d->maj); qd_put_u32(&b, d->min); qd_put_u32(&b, 0);
			qd_send_signal_fds(L1.c, s->controller, p, "org.freedesktop.login1.Session", "ResumeDevice", "uuh", &b, &d->fd, 1);
			free(b.b);
		}
	}
	l1_session_changed(s, 1, 1, 0, 0);
}

static void l1_refresh_active(void)
{
	int ch = 0;
	for (int i = 0; i < L1.nsess; i++) {
		l1_session *s = &L1.sess[i];
		int want = s->seat[0] && !s->closing && l1_seat_active(s->seat) == s;
		if (want != s->active) { l1_apply_active(s, want); ch = 1; }
	}
	if (ch) l1_sync_files();
}

static int l1_user_count(uid_t uid) { int n = 0; for (int i = 0; i < L1.nsess; i++) if (L1.sess[i].uid == uid) n++; return n; }

static void l1_drop_session(int idx)
{
	l1_session s = L1.sess[idx];
	char p[100];
	l1_sess_path(&s, p, sizeof p);
	l1_release_control(&L1.sess[idx]);
	if (s.fifo >= 0) close(s.fifo);
	memmove(&L1.sess[idx], &L1.sess[idx + 1], (L1.nsess - idx - 1) * sizeof L1.sess[0]);
	L1.nsess--;
	l1_sig_so("SessionRemoved", s.id, p);
	if (!l1_user_count(s.uid)) {
		char rt[200];
		l1_sig_uo("UserRemoved", s.uid);
		l1_user_runtime(s.uid, rt, sizeof rt);
		rmdir(rt);   /* only an empty dir goes: contents are never deleted recursively */
	}
	l1_refresh_active();
	l1_sync_files();
}

/* ---- inhibitors / power ---- */
static const char *const l1_what_names[] = { "shutdown", "sleep", "idle", "handle-power-key", "handle-suspend-key", "handle-hibernate-key", "handle-lid-switch", "handle-reboot-key", NULL };

static int l1_what_has(const char *what, const char *tok)
{
	size_t n = strlen(tok);
	for (const char *p = what; *p;) {
		size_t l = strcspn(p, ":");
		if (l == n && !strncmp(p, tok, n)) return 1;
		p += l + (p[l] == ':');
	}
	return 0;
}

static void l1_inh_mask(const char *mode, char *out, size_t n)
{
	size_t o = 0;
	out[0] = 0;
	for (int k = 0; l1_what_names[k]; k++)
		for (int i = 0; i < L1.ninh; i++)
			if (!strcmp(L1.inh[i].mode, mode) && l1_what_has(L1.inh[i].what, l1_what_names[k])) {
				o += snprintf(out + o, n - o, "%s%s", o ? ":" : "", l1_what_names[k]);
				break;
			}
}

static int l1_inh_has(const char *mode, const char *tok)
{
	for (int i = 0; i < L1.ninh; i++) if (!strcmp(L1.inh[i].mode, mode) && l1_what_has(L1.inh[i].what, tok)) return 1;
	return 0;
}

enum { P_POWEROFF, P_REBOOT, P_HALT, P_SUSPEND, P_HIBERNATE, P_HYBRID, P_SUSPENDHIB, P_N };
static const char *const l1_pnames[P_N] = { "PowerOff", "Reboot", "Halt", "Suspend", "Hibernate", "HybridSleep", "SuspendThenHibernate" };
static int l1_is_sleep(int k) { return k >= P_SUSPEND; }

static void l1_do_action(int k)
{
	const char *cmd = L1.cfg.cmd[k];
	char path[300];
	FILE *f;
	if (!L1.cfg.real_power) {
		l1_log("DRY RUN: would %s", l1_pnames[k]);
		snprintf(path, sizeof path, "%s/qlogind/actions.log", L1.run);
		if ((f = fopen(path, "a"))) { fprintf(f, "DRYRUN %s\n", l1_pnames[k]); fclose(f); }
		return;
	}
	if (!*cmd) { l1_log("real_power=1 but no cmd_ configured for %s: nothing done", l1_pnames[k]); return; }
	l1_log("running configured command for %s", l1_pnames[k]);
	{
		pid_t p = fork();
		if (p == 0) { execl("/bin/sh", "sh", "-c", cmd, (char *)NULL); _exit(127); }
		if (p > 0 && l1_is_sleep(k)) waitpid(p, NULL, 0);
	}
}

static void l1_tick_action(void)
{
	int k;
	const char *cat;
	if (!L1.pend.on) return;
	k = L1.pend.kind;
	cat = l1_is_sleep(k) ? "sleep" : "shutdown";
	if (l1_inh_has("delay", cat) && l1_now_us(CLOCK_MONOTONIC) < L1.pend.deadline) return;
	L1.pend.on = 0;
	l1_do_action(k);
	if (l1_is_sleep(k)) { L1.preparing_sleep = 0; l1_sig_bool("PrepareForSleep", 0); }
	else if (!L1.cfg.real_power) { L1.preparing_shutdown = 0; l1_sig_bool("PrepareForShutdown", 0); }
}

/* ---- request context helpers ---- */
static int l1_err(l1_ctx *x, const char *name, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(x->errtext, sizeof x->errtext, fmt, ap);
	va_end(ap);
	x->errname = name;
	return -1;
}
#define E_ARGS "org.freedesktop.DBus.Error.InvalidArgs"
#define E_DENIED "org.freedesktop.DBus.Error.AccessDenied"
#define E_NOSESS "org.freedesktop.login1.NoSuchSession"
#define E_NOSEAT "org.freedesktop.login1.NoSuchSeat"
#define E_NOUSER "org.freedesktop.login1.NoSuchUser"

static void l1_creds(l1_ctx *x)
{
	qd_msg r;
	qd_buf b = {0};
	if (x->have_creds) return;
	for (int i = 0; i < L1.ncreds; i++) if (!strcmp(L1.creds[i].who, x->m->sender)) { x->cuid = L1.creds[i].uid; x->cpid = L1.creds[i].pid; x->have_creds = 1; return; }
	x->cuid = (uid_t)-1; x->cpid = 0;
	qd_put_str(&b, x->m->sender);
	if (qd_call(x->c, &r, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetConnectionCredentials", "s", &b, NULL, 0, 2000) == 0) {
		qd_rd rd = { r.body, 0, r.blen, 0 };
		size_t end = qd_r_array(&rd, 8);
		while (!rd.bad && rd.pos < end) {
			const char *k, *vs;
			rd.pos = (rd.pos + 7) & ~7u;
			k = qd_r_str(&rd);
			vs = qd_r_sig(&rd);
			if (*vs == 'u' && !strcmp(k, "UnixUserID")) x->cuid = qd_r_u32(&rd);
			else if (*vs == 'u' && !strcmp(k, "ProcessID")) x->cpid = qd_r_u32(&rd);
			else qd_r_skip(&rd, &vs);
		}
		qd_msg_free(&r);
	}
	free(b.b);
	if (x->cuid != (uid_t)-1) {
		x->have_creds = 1;
		if (L1.ncreds < 64) { snprintf(L1.creds[L1.ncreds].who, 48, "%s", x->m->sender); L1.creds[L1.ncreds].uid = x->cuid; L1.creds[L1.ncreds].pid = x->cpid; L1.ncreds++; }
	}
}

static int l1_authorized(l1_ctx *x)
{
	l1_creds(x);
	if (!x->have_creds) return 0;
	if (x->cuid == 0) return 1;
	for (int i = 0; i < L1.nsess; i++) if (L1.sess[i].uid == x->cuid && !L1.sess[i].remote) return 1;
	return 0;
}
static int l1_owns(l1_ctx *x, const l1_session *s) { l1_creds(x); return x->have_creds && (x->cuid == 0 || x->cuid == s->uid); }

static void l1_put_so(qd_buf *b, const char *s, const char *path) { qd_align(b, 8); qd_put_str(b, s); qd_put_str(b, path); }
static void l1_put_empty_so(qd_buf *b) { l1_put_so(b, "", "/"); }
static void l1_put_sess_so(qd_buf *b, const l1_session *s) { char p[100]; l1_sess_path(s, p, sizeof p); l1_put_so(b, s->id, p); }

/* ---- property getters ---- */
typedef void (*l1_get)(const l1_obj *o, qd_buf *b);
typedef struct { const char *name, *sig; l1_get get; } l1_prop;

#define G(fn) static void fn(const l1_obj *o, qd_buf *b)
#define S (o->s)
G(g_s_id) { qd_put_str(b, S->id); }
G(g_s_user) { char p[80]; l1_user_path(S->uid, p, sizeof p); qd_align(b, 8); qd_put_u32(b, S->uid); qd_put_str(b, p); }
G(g_s_name) { qd_put_str(b, S->name); }
G(g_s_vtnr) { qd_put_u32(b, S->vtnr); }
G(g_s_seat) { if (S->seat[0]) { char p[80]; l1_seat_path(S->seat, p, sizeof p); l1_put_so(b, S->seat, p); } else l1_put_empty_so(b); }
G(g_s_tty) { qd_put_str(b, S->tty); }
G(g_s_display) { qd_put_str(b, S->display); }
G(g_s_remote) { qd_put_u32(b, S->remote); }
G(g_s_rhost) { qd_put_str(b, S->remote_host); }
G(g_s_ruser) { qd_put_str(b, S->remote_user); }
G(g_s_service) { qd_put_str(b, S->service); }
G(g_s_desktop) { qd_put_str(b, S->desktop); }
G(g_s_scope) { (void)o; qd_put_str(b, ""); }
G(g_s_leader) { qd_put_u32(b, S->leader); }
G(g_s_type) { qd_put_str(b, S->type); }
G(g_s_class) { qd_put_str(b, S->class_); }
G(g_s_active) { qd_put_u32(b, S->active); }
G(g_s_state) { qd_put_str(b, S->closing ? "closing" : S->active ? "active" : "online"); }
G(g_s_idle) { qd_put_u32(b, S->idle); }
G(g_s_idlesince) { qd_put_u64(b, S->idle ? S->idle_since : 0); }
G(g_s_true) { (void)o; qd_put_u32(b, 1); }
G(g_s_locked) { qd_put_u32(b, S->locked); }

static const l1_prop l1_sess_props[] = {
	{ "Id", "s", g_s_id }, { "User", "(uo)", g_s_user }, { "Name", "s", g_s_name }, { "VTNr", "u", g_s_vtnr }, { "Seat", "(so)", g_s_seat },
	{ "TTY", "s", g_s_tty }, { "Display", "s", g_s_display }, { "Remote", "b", g_s_remote }, { "RemoteHost", "s", g_s_rhost },
	{ "RemoteUser", "s", g_s_ruser }, { "Service", "s", g_s_service }, { "Desktop", "s", g_s_desktop }, { "Scope", "s", g_s_scope },
	{ "Leader", "u", g_s_leader }, { "Type", "s", g_s_type }, { "Class", "s", g_s_class }, { "Active", "b", g_s_active },
	{ "State", "s", g_s_state }, { "IdleHint", "b", g_s_idle }, { "IdleSinceHint", "t", g_s_idlesince },
	{ "IdleSinceHintMonotonic", "t", g_s_idlesince }, { "CanIdle", "b", g_s_true }, { "CanLock", "b", g_s_true },
	{ "LockedHint", "b", g_s_locked }, { NULL, NULL, NULL }
};

static int l1_all_idle(uid_t uid, int any_uid)
{
	int n = 0;
	for (int i = 0; i < L1.nsess; i++) if (any_uid || L1.sess[i].uid == uid) { n++; if (!L1.sess[i].idle) return 0; }
	return n >= 0;
}

G(g_m_idle) { (void)o; qd_put_u32(b, l1_all_idle(0, 1)); }
G(g_m_zero64) { (void)o; qd_put_u64(b, 0); }
G(g_m_block) { char t[200]; (void)o; l1_inh_mask("block", t, sizeof t); qd_put_str(b, t); }
G(g_m_empty) { (void)o; qd_put_str(b, ""); }
G(g_m_delay) { char t[200]; (void)o; l1_inh_mask("delay", t, sizeof t); qd_put_str(b, t); }
G(g_m_delaymax) { (void)o; qd_put_u64(b, (uint64_t)L1.cfg.delay_max_s * 1000000); }
G(g_m_nautovts) { (void)o; qd_put_u32(b, 6); }
G(g_m_false) { (void)o; qd_put_u32(b, 0); }
G(g_m_prep_sd) { (void)o; qd_put_u32(b, L1.preparing_shutdown); }
G(g_m_prep_sl) { (void)o; qd_put_u32(b, L1.preparing_sleep); }
G(g_m_nsess) { (void)o; qd_put_u64(b, L1.nsess); }
G(g_m_ninh) { (void)o; qd_put_u64(b, L1.ninh); }
G(g_m_max) { (void)o; qd_put_u64(b, 8192); }
G(g_m_poweroff) { (void)o; qd_put_str(b, "poweroff"); }
G(g_m_suspend) { (void)o; qd_put_str(b, "suspend"); }
G(g_m_hibernate) { (void)o; qd_put_str(b, "hibernate"); }
G(g_m_ignore) { (void)o; qd_put_str(b, "ignore"); }

static const l1_prop l1_mgr_props[] = {
	{ "NAutoVTs", "u", g_m_nautovts }, { "KillUserProcesses", "b", g_m_false }, { "IdleHint", "b", g_m_idle },
	{ "IdleSinceHint", "t", g_m_zero64 }, { "IdleSinceHintMonotonic", "t", g_m_zero64 }, { "BlockInhibited", "s", g_m_block },
	{ "BlockWeakInhibited", "s", g_m_empty }, { "DelayInhibited", "s", g_m_delay }, { "InhibitDelayMaxUSec", "t", g_m_delaymax },
	{ "UserStopDelayUSec", "t", g_m_zero64 }, { "HandlePowerKey", "s", g_m_poweroff }, { "HandleSuspendKey", "s", g_m_suspend },
	{ "HandleHibernateKey", "s", g_m_hibernate }, { "HandleLidSwitch", "s", g_m_suspend }, { "HandleLidSwitchExternalPower", "s", g_m_suspend },
	{ "HandleLidSwitchDocked", "s", g_m_ignore }, { "PreparingForShutdown", "b", g_m_prep_sd }, { "PreparingForSleep", "b", g_m_prep_sl },
	{ "InhibitorsMax", "t", g_m_max }, { "NCurrentInhibitors", "t", g_m_ninh }, { "SessionsMax", "t", g_m_max }, { "NCurrentSessions", "t", g_m_nsess },
	{ NULL, NULL, NULL }
};

G(g_seat_id) { (void)o; qd_put_str(b, "seat0"); }
G(g_seat_active) { l1_session *a = l1_seat_active("seat0"); (void)o; if (a) l1_put_sess_so(b, a); else l1_put_empty_so(b); }
G(g_seat_sessions) {
	qd_arr a = qd_arr_begin(b, 8);
	(void)o;
	for (int i = 0; i < L1.nsess; i++) if (!strcmp(L1.sess[i].seat, "seat0")) l1_put_sess_so(b, &L1.sess[i]);
	qd_arr_end(b, a);
}
static const l1_prop l1_seat_props[] = {
	{ "Id", "s", g_seat_id }, { "ActiveSession", "(so)", g_seat_active }, { "CanMultiSession", "b", g_s_true }, { "CanTTY", "b", g_s_true },
	{ "CanGraphical", "b", g_s_true }, { "Sessions", "a(so)", g_seat_sessions }, { "IdleHint", "b", g_m_idle },
	{ "IdleSinceHint", "t", g_m_zero64 }, { "IdleSinceHintMonotonic", "t", g_m_zero64 }, { NULL, NULL, NULL }
};

G(g_u_uid) { qd_put_u32(b, o->uid); }
G(g_u_gid) { qd_put_u32(b, l1_user_gid(o->uid)); }
G(g_u_name) { char n[48]; qd_put_str(b, l1_user_name(o->uid, n, sizeof n)); }
G(g_u_rt) { char p[200]; l1_user_runtime(o->uid, p, sizeof p); qd_put_str(b, p); }
G(g_u_display) {
	for (int i = 0; i < L1.nsess; i++) if (L1.sess[i].uid == o->uid) { l1_put_sess_so(b, &L1.sess[i]); return; }
	l1_put_empty_so(b);
}
G(g_u_state) {
	int act = 0;
	for (int i = 0; i < L1.nsess; i++) if (L1.sess[i].uid == o->uid && L1.sess[i].active) act = 1;
	qd_put_str(b, act ? "active" : "online");
}
G(g_u_sessions) {
	qd_arr a = qd_arr_begin(b, 8);
	for (int i = 0; i < L1.nsess; i++) if (L1.sess[i].uid == o->uid) l1_put_sess_so(b, &L1.sess[i]);
	qd_arr_end(b, a);
}
G(g_u_idle) { qd_put_u32(b, l1_all_idle(o->uid, 0)); }
static const l1_prop l1_user_props[] = {
	{ "UID", "u", g_u_uid }, { "GID", "u", g_u_gid }, { "Name", "s", g_u_name }, { "RuntimePath", "s", g_u_rt }, { "Service", "s", g_m_empty },
	{ "Slice", "s", g_m_empty }, { "Display", "(so)", g_u_display }, { "State", "s", g_u_state }, { "Sessions", "a(so)", g_u_sessions },
	{ "IdleHint", "b", g_u_idle }, { "IdleSinceHint", "t", g_m_zero64 }, { "IdleSinceHintMonotonic", "t", g_m_zero64 }, { "Linger", "b", g_m_false },
	{ NULL, NULL, NULL }
};
#undef G
#undef S

/* ---- method handlers ---- */
typedef int (*l1_mfn)(l1_ctx *x);
typedef struct { const char *name, *in, *out; l1_mfn fn; } l1_method;
typedef struct { const char *name, *sig; } l1_signal;
typedef struct { const char *name; const l1_method *methods; const l1_prop *props; const l1_signal *sigs; } l1_iface;

static int l1_ret_path(l1_ctx *x, const char *p) { x->osig = "o"; qd_put_str(&x->out, p); return 0; }

static int m_getsession(l1_ctx *x)
{
	const char *id = qd_r_str(&x->in);
	l1_session *s = l1_find(id);
	char p[100];
	if (!s) return l1_err(x, E_NOSESS, "No session '%s' known", id);
	l1_sess_path(s, p, sizeof p);
	return l1_ret_path(x, p);
}
static int m_getseat(l1_ctx *x)
{
	const char *id = qd_r_str(&x->in);
	char p[80];
	if (strcmp(id, "seat0")) return l1_err(x, E_NOSEAT, "No seat '%s' known", id);
	l1_seat_path(id, p, sizeof p);
	return l1_ret_path(x, p);
}
static int m_getsessionbypid(l1_ctx *x)
{
	pid_t pid = qd_r_u32(&x->in);
	l1_session *s = l1_by_pid(pid);
	char p[100];
	if (!s) return l1_err(x, "org.freedesktop.login1.NoSessionForPID", "PID %d does not belong to any known session", (int)pid);
	l1_sess_path(s, p, sizeof p);
	return l1_ret_path(x, p);
}
static int m_getuser(l1_ctx *x)
{
	uid_t uid = qd_r_u32(&x->in);
	char p[80];
	if (!l1_user_count(uid)) return l1_err(x, E_NOUSER, "User ID %u is not logged in", (unsigned)uid);
	l1_user_path(uid, p, sizeof p);
	return l1_ret_path(x, p);
}
static int m_getuserbypid(l1_ctx *x)
{
	pid_t pid = qd_r_u32(&x->in);
	l1_session *s = l1_by_pid(pid);
	char p[80];
	if (!s) return l1_err(x, E_NOUSER, "PID %d does not belong to any known user", (int)pid);
	l1_user_path(s->uid, p, sizeof p);
	return l1_ret_path(x, p);
}
static int m_listsessions(l1_ctx *x)
{
	qd_arr a = qd_arr_begin(&x->out, 8);
	x->osig = "a(susso)";
	for (int i = 0; i < L1.nsess; i++) {
		l1_session *s = &L1.sess[i];
		char p[100];
		l1_sess_path(s, p, sizeof p);
		qd_align(&x->out, 8);
		qd_put_str(&x->out, s->id); qd_put_u32(&x->out, s->uid); qd_put_str(&x->out, s->name); qd_put_str(&x->out, s->seat); qd_put_str(&x->out, p);
	}
	qd_arr_end(&x->out, a);
	return 0;
}
static int m_listusers(l1_ctx *x)
{
	qd_arr a = qd_arr_begin(&x->out, 8);
	x->osig = "a(uso)";
	for (int i = 0; i < L1.nsess; i++) {
		char p[80], n[48];
		int dup = 0;
		for (int j = 0; j < i; j++) if (L1.sess[j].uid == L1.sess[i].uid) dup = 1;
		if (dup) continue;
		l1_user_path(L1.sess[i].uid, p, sizeof p);
		qd_align(&x->out, 8);
		qd_put_u32(&x->out, L1.sess[i].uid); qd_put_str(&x->out, l1_user_name(L1.sess[i].uid, n, sizeof n)); qd_put_str(&x->out, p);
	}
	qd_arr_end(&x->out, a);
	return 0;
}
static int m_listseats(l1_ctx *x)
{
	qd_arr a = qd_arr_begin(&x->out, 8);
	char p[80];
	x->osig = "a(so)";
	l1_seat_path("seat0", p, sizeof p);
	l1_put_so(&x->out, "seat0", p);
	qd_arr_end(&x->out, a);
	return 0;
}
static int m_listinhibitors(l1_ctx *x)
{
	qd_arr a = qd_arr_begin(&x->out, 8);
	x->osig = "a(ssssuu)";
	for (int i = 0; i < L1.ninh; i++) {
		l1_inhibit *n = &L1.inh[i];
		qd_align(&x->out, 8);
		qd_put_str(&x->out, n->what); qd_put_str(&x->out, n->who); qd_put_str(&x->out, n->why); qd_put_str(&x->out, n->mode);
		qd_put_u32(&x->out, n->uid); qd_put_u32(&x->out, n->pid);
	}
	qd_arr_end(&x->out, a);
	return 0;
}

static int m_createsession(l1_ctx *x)
{
	uid_t uid = qd_r_u32(&x->in);
	pid_t pid = qd_r_u32(&x->in);
	const char *service = qd_r_str(&x->in), *type = qd_r_str(&x->in), *cls = qd_r_str(&x->in), *desktop = qd_r_str(&x->in);
	const char *seat = qd_r_str(&x->in);
	unsigned vtnr = qd_r_u32(&x->in);
	const char *tty = qd_r_str(&x->in), *display = qd_r_str(&x->in);
	int remote = qd_r_u32(&x->in);
	const char *ruser = qd_r_str(&x->in), *rhost = qd_r_str(&x->in);
	static const char *const types[] = { "unspecified", "tty", "x11", "wayland", "mir", "web", NULL };
	static const char *const classes[] = { "user", "greeter", "lock-screen", "background", NULL };
	l1_session *s = NULL;
	char p[100], rt[200], nm[48];
	int ok = 0, fd[2], existing = 0;
	struct passwd *pw;
	if (x->in.bad) return l1_err(x, E_ARGS, "Malformed arguments");
	if (!l1_owns(x, &(l1_session){ .uid = uid })) return l1_err(x, E_DENIED, "Only root or the user itself may create a session for UID %u", (unsigned)uid);
	for (int i = 0; types[i]; i++) if (!strcmp(type, types[i])) ok = 1;
	if (!ok && *type) return l1_err(x, E_ARGS, "Invalid session type '%s'", type);
	ok = !*cls;
	for (int i = 0; classes[i]; i++) if (!strcmp(cls, classes[i])) ok = 1;
	if (!ok) return l1_err(x, E_ARGS, "Invalid session class '%s'", cls);
	if (*seat && strcmp(seat, "seat0")) return l1_err(x, E_NOSEAT, "No seat '%s' known", seat);
	if (!*seat && vtnr) return l1_err(x, E_ARGS, "Session without seat cannot have a VT");
	if (pid <= 0) return l1_err(x, E_ARGS, "Invalid leader PID");
	if (!(pw = getpwuid(uid))) return l1_err(x, E_NOUSER, "Unknown UID %u", (unsigned)uid);
	for (int i = 0; i < L1.nsess; i++) if (L1.sess[i].leader == pid && L1.sess[i].uid == uid) { s = &L1.sess[i]; existing = 1; }
	if (!s) {
		int first;
		if (L1.nsess >= L1_MAXSESS) return l1_err(x, "org.freedesktop.login1.TooManySessions", "Too many sessions");
		s = &L1.sess[L1.nsess];
		memset(s, 0, sizeof *s);
		snprintf(s->id, sizeof s->id, "c%u", L1.next_id++);
		snprintf(s->name, sizeof s->name, "%s", pw->pw_name);
		s->uid = uid; s->leader = pid; s->vtnr = vtnr; s->remote = remote != 0; s->fifo = -1;
		snprintf(s->seat, sizeof s->seat, "%s", seat);
		snprintf(s->type, sizeof s->type, "%s", *type ? type : "unspecified");
		snprintf(s->class_, sizeof s->class_, "%s", *cls ? cls : "user");
		snprintf(s->tty, sizeof s->tty, "%s", tty); snprintf(s->display, sizeof s->display, "%s", display);
		snprintf(s->service, sizeof s->service, "%s", service); snprintf(s->desktop, sizeof s->desktop, "%s", desktop);
		snprintf(s->remote_host, sizeof s->remote_host, "%s", rhost); snprintf(s->remote_user, sizeof s->remote_user, "%s", ruser);
		s->created = l1_now_us(CLOCK_REALTIME);
		if (pipe2(fd, O_CLOEXEC | O_NONBLOCK) < 0) return l1_err(x, "System.Error.EMFILE", "Cannot create session fifo");
		s->fifo = fd[0];
		first = !l1_user_count(uid);
		L1.nsess++;
		l1_user_runtime(uid, rt, sizeof rt);
		if (mkdir(rt, 0700) == 0 || errno == EEXIST) { if (chown(rt, uid, l1_user_gid(uid)) < 0 && geteuid() == 0) l1_log("chown %s: %s", rt, strerror(errno)); }
		else l1_log("mkdir %s: %s", rt, strerror(errno));
		l1_sess_path(s, p, sizeof p);
		if (first) l1_sig_uo("UserNew", uid);
		l1_sig_so("SessionNew", s->id, p);
		l1_refresh_active();
		l1_sync_files();
		x->fds[0] = dup(fd[1]); x->nfds = 1;   /* the pipe write end we got from pipe2 is handed out; keep nothing of it */
		close(fd[1]);
	} else {
		int w[2];   /* an existing session keeps its fifo; the caller gets an extra, harmless write end */
		if (pipe2(w, O_CLOEXEC) == 0) { close(w[0]); x->fds[0] = w[1]; x->nfds = 1; }
	}
	l1_sess_path(s, p, sizeof p);
	l1_user_runtime(uid, rt, sizeof rt);
	l1_user_name(uid, nm, sizeof nm);
	x->osig = "soshusub";
	qd_put_str(&x->out, s->id); qd_put_str(&x->out, p); qd_put_str(&x->out, rt); qd_put_u32(&x->out, 0);
	qd_put_u32(&x->out, uid); qd_put_str(&x->out, s->seat); qd_put_u32(&x->out, s->vtnr); qd_put_u32(&x->out, existing);
	return 0;
}

static int m_releasesession(l1_ctx *x)
{
	const char *id = qd_r_str(&x->in);
	l1_session *s = l1_find(id);
	if (!s) return l1_err(x, E_NOSESS, "No session '%s' known", id);
	if (!l1_owns(x, s)) return l1_err(x, E_DENIED, "Not your session");
	l1_drop_session((int)(s - L1.sess));
	return 0;
}

static void l1_set_vt(l1_ctx *x, unsigned vt)
{
	(void)x;
	if (!vt) return;
	if (getenv("QLOGIND_VTFILE")) {   /* mock backend: a plain file stands for /sys/class/tty/tty0/active */
		FILE *f = fopen(L1.vtfile, "w");
		if (f) { fprintf(f, "tty%u\n", vt); fclose(f); }
		L1.curvt = vt;
	} else l1_log("DRY RUN: would switch to VT %u (real VT switching is not implemented in this build)", vt);
	l1_refresh_active();
}

static int m_activatesession(l1_ctx *x)
{
	const char *id = qd_r_str(&x->in);
	l1_session *s = l1_find(id);
	if (!s) return l1_err(x, E_NOSESS, "No session '%s' known", id);
	if (!l1_owns(x, s) && !l1_authorized(x)) return l1_err(x, E_DENIED, "Not authorized");
	l1_set_vt(x, s->vtnr);
	return 0;
}
static int l1_lock_one(l1_session *s, int lock)
{
	char p[100];
	l1_sess_path(s, p, sizeof p);
	qd_send_signal(L1.c, NULL, p, "org.freedesktop.login1.Session", lock ? "Lock" : "Unlock", NULL, NULL);
	return 0;
}
static int m_locksession(l1_ctx *x) { l1_session *s = l1_find(qd_r_str(&x->in)); if (!s) return l1_err(x, E_NOSESS, "No such session"); if (!l1_authorized(x)) return l1_err(x, E_DENIED, "Not authorized"); return l1_lock_one(s, 1); }
static int m_unlocksession(l1_ctx *x) { l1_session *s = l1_find(qd_r_str(&x->in)); if (!s) return l1_err(x, E_NOSESS, "No such session"); if (!l1_authorized(x)) return l1_err(x, E_DENIED, "Not authorized"); return l1_lock_one(s, 0); }
static int m_locksessions(l1_ctx *x) { if (!l1_authorized(x)) return l1_err(x, E_DENIED, "Not authorized"); for (int i = 0; i < L1.nsess; i++) l1_lock_one(&L1.sess[i], 1); return 0; }
static int m_unlocksessions(l1_ctx *x) { if (!l1_authorized(x)) return l1_err(x, E_DENIED, "Not authorized"); for (int i = 0; i < L1.nsess; i++) l1_lock_one(&L1.sess[i], 0); return 0; }

static int m_killsession(l1_ctx *x)
{
	const char *id = qd_r_str(&x->in), *whom = qd_r_str(&x->in);
	int sig = (int)qd_r_u32(&x->in);
	l1_session *s = l1_find(id);
	if (!s) return l1_err(x, E_NOSESS, "No session '%s' known", id);
	if (!l1_owns(x, s)) return l1_err(x, E_DENIED, "Not your session");
	if (sig <= 0 || sig > 64) return l1_err(x, E_ARGS, "Invalid signal");
	if (strcmp(whom, "leader") && strcmp(whom, "all") && *whom) return l1_err(x, E_ARGS, "Invalid kill mode");
	/* "all" is the same as "leader" here: without cgroups there is no reliable member list (documented limit) */
	if (kill(s->leader, sig) < 0) return l1_err(x, "System.Error.ESRCH", "kill: %s", strerror(errno));
	return 0;
}

static int m_inhibit(l1_ctx *x)
{
	const char *what = qd_r_str(&x->in), *who = qd_r_str(&x->in), *why = qd_r_str(&x->in), *mode = qd_r_str(&x->in);
	int fd[2];
	l1_inhibit *n;
	if (x->in.bad || !*what) return l1_err(x, E_ARGS, "Malformed arguments");
	if (strcmp(mode, "block") && strcmp(mode, "delay")) return l1_err(x, E_ARGS, "Invalid mode '%s'", mode);
	for (const char *p = what; *p;) {
		size_t l = strcspn(p, ":"), k;
		for (k = 0; l1_what_names[k]; k++) if (strlen(l1_what_names[k]) == l && !strncmp(p, l1_what_names[k], l)) break;
		if (!l1_what_names[k]) return l1_err(x, E_ARGS, "Invalid inhibitor type in '%s'", what);
		if (!strcmp(mode, "delay") && k > 1) return l1_err(x, E_ARGS, "Delay locks only apply to shutdown and sleep");
		p += l + (p[l] == ':');
	}
	if (L1.ninh >= L1_MAXINH) return l1_err(x, "org.freedesktop.login1.TooManyInhibitors", "Too many inhibitors");
	l1_creds(x);
	if (!x->have_creds) return l1_err(x, E_DENIED, "Cannot identify caller");
	if (pipe2(fd, O_CLOEXEC | O_NONBLOCK) < 0) return l1_err(x, "System.Error.EMFILE", "Cannot create inhibitor pipe");
	fcntl(fd[1], F_SETFL, 0);
	n = &L1.inh[L1.ninh++];
	memset(n, 0, sizeof *n);
	snprintf(n->what, sizeof n->what, "%s", what); snprintf(n->who, sizeof n->who, "%s", who); snprintf(n->why, sizeof n->why, "%s", why);
	snprintf(n->mode, sizeof n->mode, "%s", mode);
	n->uid = x->cuid; n->pid = x->cpid; n->fd = fd[0]; n->created = l1_now_us(CLOCK_REALTIME);
	x->fds[0] = fd[1]; x->nfds = 1;   /* closing every copy of the write end releases the lock */
	x->osig = "h";
	qd_put_u32(&x->out, 0);
	return 0;
}

static int l1_power(l1_ctx *x, int k)
{
	if (!l1_authorized(x)) return l1_err(x, E_DENIED, "Not authorized to %s", l1_pnames[k]);
	(void)qd_r_u32(&x->in);   /* interactive: no polkit, nothing to ask */
	if (L1.pend.on) return l1_err(x, "org.freedesktop.login1.OperationInProgress", "A shutdown or sleep operation is already in progress");
	if (l1_inh_has("block", l1_is_sleep(k) ? "sleep" : "shutdown")) return l1_err(x, "org.freedesktop.login1.BlockedByInhibitor", "Operation inhibited by a block lock");
	L1.pend.on = 1; L1.pend.kind = k; L1.pend.deadline = l1_now_us(CLOCK_MONOTONIC) + (uint64_t)L1.cfg.delay_max_s * 1000000;
	if (l1_is_sleep(k)) { L1.preparing_sleep = 1; l1_sig_bool("PrepareForSleep", 1); }
	else { L1.preparing_shutdown = 1; l1_sig_bool("PrepareForShutdown", 1); }
	return 0;
}
static int l1_can(l1_ctx *x, int k)
{
	const char *r = !l1_authorized(x) ? "no" : (L1.cfg.real_power && !*L1.cfg.cmd[k]) ? "na" : "yes";
	x->osig = "s";
	qd_put_str(&x->out, r);
	return 0;
}
#define PW(n, k) static int m_p_##n(l1_ctx *x) { return l1_power(x, k); } static int m_c_##n(l1_ctx *x) { return l1_can(x, k); }
PW(poweroff, P_POWEROFF) PW(reboot, P_REBOOT) PW(halt, P_HALT) PW(suspend, P_SUSPEND) PW(hibernate, P_HIBERNATE) PW(hybrid, P_HYBRID) PW(suspendhib, P_SUSPENDHIB)
#undef PW

static const l1_method l1_mgr_methods[] = {
	{ "GetSession", "s", "o", m_getsession }, { "GetSessionByPID", "u", "o", m_getsessionbypid }, { "GetUser", "u", "o", m_getuser },
	{ "GetUserByPID", "u", "o", m_getuserbypid }, { "GetSeat", "s", "o", m_getseat }, { "ListSessions", "", "a(susso)", m_listsessions },
	{ "ListUsers", "", "a(uso)", m_listusers }, { "ListSeats", "", "a(so)", m_listseats }, { "ListInhibitors", "", "a(ssssuu)", m_listinhibitors },
	{ "CreateSession", "uusssssussbssa(sv)", "soshusub", m_createsession }, { "ReleaseSession", "s", "", m_releasesession },
	{ "ActivateSession", "s", "", m_activatesession }, { "LockSession", "s", "", m_locksession }, { "UnlockSession", "s", "", m_unlocksession },
	{ "LockSessions", "", "", m_locksessions }, { "UnlockSessions", "", "", m_unlocksessions }, { "KillSession", "ssi", "", m_killsession },
	{ "Inhibit", "ssss", "h", m_inhibit },
	{ "PowerOff", "b", "", m_p_poweroff }, { "Reboot", "b", "", m_p_reboot }, { "Halt", "b", "", m_p_halt }, { "Suspend", "b", "", m_p_suspend },
	{ "Hibernate", "b", "", m_p_hibernate }, { "HybridSleep", "b", "", m_p_hybrid }, { "SuspendThenHibernate", "b", "", m_p_suspendhib },
	{ "CanPowerOff", "", "s", m_c_poweroff }, { "CanReboot", "", "s", m_c_reboot }, { "CanHalt", "", "s", m_c_halt }, { "CanSuspend", "", "s", m_c_suspend },
	{ "CanHibernate", "", "s", m_c_hibernate }, { "CanHybridSleep", "", "s", m_c_hybrid }, { "CanSuspendThenHibernate", "", "s", m_c_suspendhib },
	{ NULL, NULL, NULL, NULL }
};
static const l1_signal l1_mgr_signals[] = {
	{ "SessionNew", "so" }, { "SessionRemoved", "so" }, { "UserNew", "uo" }, { "UserRemoved", "uo" },
	{ "PrepareForShutdown", "b" }, { "PrepareForSleep", "b" }, { NULL, NULL }
};

/* session methods */
static int m_s_activate(l1_ctx *x) { l1_session *s = x->obj.s; if (!l1_owns(x, s) && !l1_authorized(x)) return l1_err(x, E_DENIED, "Not authorized"); l1_set_vt(x, s->vtnr); return 0; }
static int m_s_lock(l1_ctx *x) { if (!l1_authorized(x)) return l1_err(x, E_DENIED, "Not authorized"); return l1_lock_one(x->obj.s, 1); }
static int m_s_unlock(l1_ctx *x) { if (!l1_authorized(x)) return l1_err(x, E_DENIED, "Not authorized"); return l1_lock_one(x->obj.s, 0); }
static int m_s_setidle(l1_ctx *x)
{
	l1_session *s = x->obj.s;
	int v = qd_r_u32(&x->in) != 0;
	if (!l1_owns(x, s)) return l1_err(x, E_DENIED, "Not your session");
	if (s->idle != v) { s->idle = v; s->idle_since = l1_now_us(CLOCK_REALTIME); l1_session_changed(s, 0, 0, 1, 0); }
	return 0;
}
static int m_s_setlocked(l1_ctx *x)
{
	l1_session *s = x->obj.s;
	int v = qd_r_u32(&x->in) != 0;
	if (!l1_owns(x, s)) return l1_err(x, E_DENIED, "Not your session");
	if (s->locked != v) { s->locked = v; l1_session_changed(s, 0, 0, 0, 1); }
	return 0;
}
static int m_s_takecontrol(l1_ctx *x)
{
	l1_session *s = x->obj.s;
	int force = qd_r_u32(&x->in) != 0;
	if (!l1_owns(x, s)) return l1_err(x, E_DENIED, "Not your session");
	if (s->controller[0] && strcmp(s->controller, x->m->sender)) {
		if (!force || x->cuid != 0) return l1_err(x, "System.Error.EBUSY", "Control of this session is already taken");
		l1_release_control(s);
	}
	snprintf(s->controller, sizeof s->controller, "%s", x->m->sender);
	return 0;
}
static int m_s_releasecontrol(l1_ctx *x)
{
	l1_session *s = x->obj.s;
	if (strcmp(s->controller, x->m->sender)) return l1_err(x, "System.Error.EINVAL", "You are not in control of this session");
	l1_release_control(s);
	return 0;
}
static int m_s_settype(l1_ctx *x)
{
	l1_session *s = x->obj.s;
	const char *t = qd_r_str(&x->in);
	static const char *const types[] = { "tty", "x11", "wayland", "mir", "web", "unspecified", NULL };
	int ok = 0;
	if (strcmp(s->controller, x->m->sender)) return l1_err(x, "System.Error.EPERM", "You are not in control of this session");
	for (int i = 0; types[i]; i++) if (!strcmp(t, types[i])) ok = 1;
	if (!ok) return l1_err(x, E_ARGS, "Invalid session type '%s'", t);
	snprintf(s->type, sizeof s->type, "%s", t);
	l1_sync_files();
	return 0;
}
static l1_dev *l1_find_dev(l1_session *s, unsigned maj, unsigned min)
{
	for (int i = 0; i < s->ndev; i++) if (s->dev[i].maj == maj && s->dev[i].min == min) return &s->dev[i];
	return NULL;
}
static int m_s_takedevice(l1_ctx *x)
{
	l1_session *s = x->obj.s;
	unsigned maj = qd_r_u32(&x->in), min = qd_r_u32(&x->in);
	l1_dev *d;
	int fd;
	if (strcmp(s->controller, x->m->sender)) return l1_err(x, "System.Error.EPERM", "You are not in control of this session");
	if (l1_find_dev(s, maj, min)) return l1_err(x, "System.Error.EALREADY", "Device already taken");
	if (s->ndev >= L1_MAXDEV) return l1_err(x, "System.Error.E2BIG", "Too many devices");
	if ((fd = l1_dev_open(maj, min)) < 0) return l1_err(x, "System.Error.ENODEV", "Cannot open device %u:%u: %s", maj, min, strerror(errno));
	d = &s->dev[s->ndev++];
	d->maj = maj; d->min = min; d->fd = fd; d->paused = !s->active;
	x->fds[0] = dup(fd); x->nfds = 1;
	x->osig = "hb";
	qd_put_u32(&x->out, 0); qd_put_u32(&x->out, !s->active);
	return 0;
}
static int m_s_releasedevice(l1_ctx *x)
{
	l1_session *s = x->obj.s;
	unsigned maj = qd_r_u32(&x->in), min = qd_r_u32(&x->in);
	l1_dev *d = l1_find_dev(s, maj, min);
	if (strcmp(s->controller, x->m->sender)) return l1_err(x, "System.Error.EPERM", "You are not in control of this session");
	if (!d) return l1_err(x, "System.Error.ENOENT", "Device not taken");
	close(d->fd);
	*d = s->dev[--s->ndev];
	return 0;
}
static int m_s_pausecomplete(l1_ctx *x)
{
	l1_session *s = x->obj.s;
	unsigned maj = qd_r_u32(&x->in), min = qd_r_u32(&x->in);
	if (strcmp(s->controller, x->m->sender)) return l1_err(x, "System.Error.EPERM", "You are not in control of this session");
	if (!l1_find_dev(s, maj, min)) return l1_err(x, "System.Error.ENOENT", "Device not taken");
	return 0;   /* the fd stays open: no DRM master drop / EVIOCREVOKE in this build (documented limit) */
}
static const l1_method l1_sess_methods[] = {
	{ "Activate", "", "", m_s_activate }, { "Lock", "", "", m_s_lock }, { "Unlock", "", "", m_s_unlock }, { "SetIdleHint", "b", "", m_s_setidle },
	{ "SetLockedHint", "b", "", m_s_setlocked }, { "TakeControl", "b", "", m_s_takecontrol }, { "ReleaseControl", "", "", m_s_releasecontrol },
	{ "SetType", "s", "", m_s_settype }, { "TakeDevice", "uu", "hb", m_s_takedevice }, { "ReleaseDevice", "uu", "", m_s_releasedevice },
	{ "PauseDeviceComplete", "uu", "", m_s_pausecomplete }, { NULL, NULL, NULL, NULL }
};
static const l1_signal l1_sess_signals[] = { { "PauseDevice", "uus" }, { "ResumeDevice", "uuh" }, { "Lock", "" }, { "Unlock", "" }, { NULL, NULL } };

static int m_seat_switchto(l1_ctx *x)
{
	unsigned vt = qd_r_u32(&x->in);
	if (!l1_authorized(x)) return l1_err(x, E_DENIED, "Not authorized");
	if (!vt) return l1_err(x, E_ARGS, "Invalid VT");
	l1_set_vt(x, vt);
	return 0;
}
static int m_seat_activate(l1_ctx *x) { return m_activatesession(x); }
static const l1_method l1_seat_methods[] = { { "SwitchTo", "u", "", m_seat_switchto }, { "ActivateSession", "s", "", m_seat_activate }, { NULL, NULL, NULL, NULL } };

static const l1_iface l1_ifaces[] = {
	{ "org.freedesktop.login1.Manager", l1_mgr_methods, l1_mgr_props, l1_mgr_signals },
	{ "org.freedesktop.login1.Session", l1_sess_methods, l1_sess_props, l1_sess_signals },
	{ "org.freedesktop.login1.Seat", l1_seat_methods, l1_seat_props, NULL },
	{ "org.freedesktop.login1.User", NULL, l1_user_props, NULL },
};
static const l1_iface *l1_iface_for(int kind, const char *name)
{
	const l1_iface *f = &l1_ifaces[kind == L1_MANAGER ? 0 : kind == L1_SESSION ? 1 : kind == L1_SEAT ? 2 : 3];
	return (kind >= L1_MANAGER && kind <= L1_USER) && (!name || !*name || !strcmp(name, f->name)) ? f : NULL;
}

/* ---- introspection ---- */
static void l1_xml_args(qd_buf *b, const char *sig, const char *dir)
{
	for (const char *p = sig; *p;) {
		size_t l = qd_sig_len(p);
		char t[64];
		snprintf(t, sizeof t, "%.*s", (int)l, p);
		char line[160];
		int n = snprintf(line, sizeof line, "   <arg type=\"%s\" direction=\"%s\"/>\n", t, dir);
		qd_put(b, line, n);
		p += l;
	}
}
static void l1_xml(l1_ctx *x, const char *path)
{
	qd_buf b = {0};
	const l1_iface *f = l1_iface_for(x->obj.kind, NULL);
	char line[256];
	static const char head[] = "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n<node>\n";
	qd_put(&b, head, sizeof head - 1);
	{
		static const char std[] =
		    " <interface name=\"org.freedesktop.DBus.Peer\">\n  <method name=\"Ping\"/>\n  <method name=\"GetMachineId\">\n   <arg type=\"s\" direction=\"out\"/>\n  </method>\n </interface>\n"
		    " <interface name=\"org.freedesktop.DBus.Introspectable\">\n  <method name=\"Introspect\">\n   <arg type=\"s\" direction=\"out\"/>\n  </method>\n </interface>\n"
		    " <interface name=\"org.freedesktop.DBus.Properties\">\n  <method name=\"Get\">\n   <arg type=\"s\" direction=\"in\"/>\n   <arg type=\"s\" direction=\"in\"/>\n   <arg type=\"v\" direction=\"out\"/>\n  </method>\n"
		    "  <method name=\"GetAll\">\n   <arg type=\"s\" direction=\"in\"/>\n   <arg type=\"a{sv}\" direction=\"out\"/>\n  </method>\n"
		    "  <signal name=\"PropertiesChanged\">\n   <arg type=\"s\"/>\n   <arg type=\"a{sv}\"/>\n   <arg type=\"as\"/>\n  </signal>\n </interface>\n";
		qd_put(&b, std, sizeof std - 1);
	}
	if (f) {
		int n = snprintf(line, sizeof line, " <interface name=\"%s\">\n", f->name);
		qd_put(&b, line, n);
		for (const l1_method *m = f->methods; m && m->name; m++) {
			n = snprintf(line, sizeof line, "  <method name=\"%s\">\n", m->name);
			qd_put(&b, line, n);
			l1_xml_args(&b, m->in, "in"); l1_xml_args(&b, m->out, "out");
			qd_put(&b, "  </method>\n", 12);
		}
		for (const l1_signal *s = f->sigs; s && s->name; s++) {
			n = snprintf(line, sizeof line, "  <signal name=\"%s\">\n", s->name);
			qd_put(&b, line, n);
			for (const char *p = s->sig; *p;) { size_t l = qd_sig_len(p); n = snprintf(line, sizeof line, "   <arg type=\"%.*s\"/>\n", (int)l, p); qd_put(&b, line, n); p += l; }
			qd_put(&b, "  </signal>\n", 12);
		}
		for (const l1_prop *p = f->props; p->name; p++) {
			n = snprintf(line, sizeof line, "  <property name=\"%s\" type=\"%s\" access=\"read\"/>\n", p->name, p->sig);
			qd_put(&b, line, n);
		}
		qd_put(&b, " </interface>\n", 14);
	}
	if (x->obj.kind == L1_MANAGER) { static const char kids[] = " <node name=\"session\"/>\n <node name=\"seat\"/>\n <node name=\"user\"/>\n"; qd_put(&b, kids, sizeof kids - 1); }
	else if (x->obj.kind == L1_NODE) {
		int n;
		if (!strcmp(path, L1_PATH "/session")) for (int i = 0; i < L1.nsess; i++) { char e[64]; l1_escape(L1.sess[i].id, e, sizeof e); n = snprintf(line, sizeof line, " <node name=\"%s\"/>\n", e); qd_put(&b, line, n); }
		else if (!strcmp(path, L1_PATH "/seat")) qd_put(&b, " <node name=\"seat0\"/>\n", 21);
		else if (!strcmp(path, L1_PATH "/user")) for (int i = 0; i < L1.nsess; i++) {
			char e[40], t[24]; int dup = 0;
			for (int j = 0; j < i; j++) if (L1.sess[j].uid == L1.sess[i].uid) dup = 1;
			if (dup) continue;
			snprintf(t, sizeof t, "%u", (unsigned)L1.sess[i].uid); l1_escape(t, e, sizeof e);
			n = snprintf(line, sizeof line, " <node name=\"%s\"/>\n", e); qd_put(&b, line, n);
		}
	}
	qd_put(&b, "</node>\n", 8);
	qd_put_u8(&b, 0);
	b.n--;
	{
		qd_buf o = {0};
		qd_put_str(&o, (const char *)b.b);
		x->osig = "s";
		qd_put(&x->out, o.b, o.n);
		free(o.b);
	}
	free(b.b);
}

/* ---- object resolution + dispatch ---- */
static int l1_resolve(l1_ctx *x, const char *path)
{
	const char *rest;
	char id[64];
	memset(&x->obj, 0, sizeof x->obj);
	if (!strcmp(path, L1_PATH)) { x->obj.kind = L1_MANAGER; return 0; }
	if (!strcmp(path, "/") || !strcmp(path, "/org") || !strcmp(path, "/org/freedesktop") || !strcmp(path, L1_PATH "/session") ||
	    !strcmp(path, L1_PATH "/seat") || !strcmp(path, L1_PATH "/user")) { x->obj.kind = L1_NODE; return 0; }
	if (!strncmp(path, L1_PATH "/session/", sizeof L1_PATH + 8)) {
		rest = path + sizeof L1_PATH + 8;
		if (!strcmp(rest, "auto") || !strcmp(rest, "self")) {
			l1_creds(x);
			x->obj.s = x->have_creds ? l1_by_pid(x->cpid) : NULL;
			if (!x->obj.s && x->have_creds) for (int i = 0; i < L1.nsess; i++) if (L1.sess[i].uid == x->cuid) { x->obj.s = &L1.sess[i]; break; }
		} else if (!l1_unescape(rest, id, sizeof id)) x->obj.s = l1_find(id);
		if (!x->obj.s) return -1;
		x->obj.kind = L1_SESSION;
		return 0;
	}
	if (!strncmp(path, L1_PATH "/seat/", sizeof L1_PATH + 5)) {
		rest = path + sizeof L1_PATH + 5;
		if (!strcmp(rest, "auto") || !strcmp(rest, "seat0")) { x->obj.kind = L1_SEAT; return 0; }
		return -1;
	}
	if (!strncmp(path, L1_PATH "/user/", sizeof L1_PATH + 5)) {
		unsigned u;
		rest = path + sizeof L1_PATH + 5;
		if (!strcmp(rest, "self")) { l1_creds(x); u = x->cuid; }
		else if (l1_unescape(rest, id, sizeof id) || sscanf(id, "%u", &u) != 1) return -1;
		if (!l1_user_count(u)) return -1;
		x->obj.kind = L1_USER; x->obj.uid = u;
		return 0;
	}
	return -1;
}

static int l1_std(l1_ctx *x)
{
	const qd_msg *m = x->m;
	if (!strcmp(m->iface, "org.freedesktop.DBus.Peer")) {
		if (!strcmp(m->member, "Ping")) return 0;
		if (!strcmp(m->member, "GetMachineId")) {
			char id[40] = "00000000000000000000000000000000";
			FILE *f = fopen("/etc/machine-id", "r");
			if (f) { if (fgets(id, sizeof id, f)) id[strcspn(id, "\n")] = 0; fclose(f); }
			x->osig = "s"; qd_put_str(&x->out, id);
			return 0;
		}
	}
	if (!strcmp(m->iface, "org.freedesktop.DBus.Introspectable") && !strcmp(m->member, "Introspect")) { l1_xml(x, m->path); return 0; }
	if (!strcmp(m->iface, "org.freedesktop.DBus.Properties")) {
		if (!strcmp(m->member, "Get") || !strcmp(m->member, "GetAll")) {
			const char *ifn = qd_r_str(&x->in);
			const l1_iface *f = l1_iface_for(x->obj.kind, ifn);
			int all = m->member[3] != 0;
			if (!f || !*ifn) return l1_err(x, E_ARGS, "No such interface '%s'", ifn);
			if (!all) {
				const char *pn = qd_r_str(&x->in);
				for (const l1_prop *p = f->props; p->name; p++) if (!strcmp(p->name, pn)) {
					x->osig = "v";
					qd_put_sig(&x->out, p->sig);
					p->get(&x->obj, &x->out);
					return 0;
				}
				return l1_err(x, "org.freedesktop.DBus.Error.UnknownProperty", "No such property '%s'", pn);
			}
			{
				qd_arr a = qd_arr_begin(&x->out, 8);
				x->osig = "a{sv}";
				for (const l1_prop *p = f->props; p->name; p++) { qd_align(&x->out, 8); qd_put_str(&x->out, p->name); qd_put_sig(&x->out, p->sig); p->get(&x->obj, &x->out); }
				qd_arr_end(&x->out, a);
				return 0;
			}
		}
		if (!strcmp(m->member, "Set")) return l1_err(x, "org.freedesktop.DBus.Error.PropertyReadOnly", "Property is read-only");
	}
	return 1;   /* not a standard call */
}

int l1_handle(qd_conn *c, const qd_msg *m)
{
	l1_ctx x;
	int rc;
	L1.c = c;
	if (m->type == 4 && !strcmp(m->iface, "org.freedesktop.DBus") && !strcmp(m->member, "NameOwnerChanged")) {
		qd_rd r = { m->body, 0, m->blen, 0 };
		const char *name = qd_r_str(&r), *oldo = qd_r_str(&r), *newo = qd_r_str(&r);
		(void)oldo;
		if (!r.bad && !*newo) {
			for (int i = 0; i < L1.ncreds; i++) if (!strcmp(L1.creds[i].who, name)) L1.creds[i] = L1.creds[--L1.ncreds];
			for (int i = 0; i < L1.nsess; i++) if (!strcmp(L1.sess[i].controller, name)) l1_release_control(&L1.sess[i]);
		}
		return 0;
	}
	if (m->type != 1) return 0;
	memset(&x, 0, sizeof x);
	x.c = c; x.m = m; x.in = (qd_rd){ m->body, 0, m->blen, 0 };
	if (l1_resolve(&x, m->path) < 0) {
		if (!(m->flags & 1)) qd_send_error(c, m, "org.freedesktop.DBus.Error.UnknownObject", "Unknown object");
		return 0;
	}
	rc = l1_std(&x);
	if (rc > 0) {
		const l1_iface *f = l1_iface_for(x.obj.kind, m->iface);
		const l1_method *me = NULL;
		if (f) for (const l1_method *k = f->methods; k && k->name; k++) if (!strcmp(k->name, m->member)) me = k;
		if (!me) { l1_err(&x, "org.freedesktop.DBus.Error.UnknownMethod", "Unknown method '%s' on '%s'", m->member, m->iface); rc = -1; }
		else if (strcmp(m->sig, me->in)) { l1_err(&x, "org.freedesktop.DBus.Error.InvalidArgs", "Invalid arguments '%s' to '%s', expected '%s'", m->sig, m->member, me->in); rc = -1; }
		else rc = me->fn(&x);
	}
	if (!(m->flags & 1)) {
		if (rc < 0) qd_send_error(c, m, x.errname ? x.errname : "org.freedesktop.DBus.Error.Failed", x.errtext);
		else qd_send_return(c, m, x.osig, x.out.n ? &x.out : NULL, x.fds, x.nfds);
	}
	for (int i = 0; i < x.nfds; i++) close(x.fds[i]);
	free(x.out.b);
	return 0;
}

/* ---- main loop ---- */
int l1_run(qd_conn *c, volatile int *stop)
{
	L1.c = c;
	{   /* NameOwnerChanged tells us when a session controller or caller disappears */
		qd_buf b = {0};
		qd_msg r;
		qd_put_str(&b, "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged'");
		if (qd_call(c, &r, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "AddMatch", "s", &b, NULL, 0, 3000) == 0) qd_msg_free(&r);
		else l1_log("AddMatch failed: controllers will not be released when they vanish");
		free(b.b);
	}
	L1.curvt = l1_read_vt();
	l1_sync_files();
	while (!*stop) {
		struct pollfd pf[1 + L1_MAXINH + L1_MAXSESS];
		int n = 0, timeout = 500, ni = L1.ninh, ns = L1.nsess;
		qd_msg m;
		pf[n++] = (struct pollfd){ c->fd, POLLIN, 0 };
		for (int i = 0; i < ni; i++) pf[n++] = (struct pollfd){ L1.inh[i].fd, POLLIN, 0 };
		for (int i = 0; i < ns; i++) pf[n++] = (struct pollfd){ L1.sess[i].fifo, POLLIN, 0 };
		if (L1.pend.on) timeout = 50;
		if (c->qhead || c->rn) timeout = 0;
		if (poll(pf, n, timeout) < 0 && errno != EINTR) return -1;
		if (pf[0].revents & (POLLHUP | POLLERR)) { l1_log("bus connection lost"); return -1; }
		for (int i = ni - 1; i >= 0; i--) if (pf[1 + i].revents & (POLLHUP | POLLERR)) {   /* every writer closed: lock released */
			close(L1.inh[i].fd);
			L1.inh[i] = L1.inh[--L1.ninh];
		}
		for (int i = ns - 1; i >= 0; i--) if (pf[1 + ni + i].revents & (POLLHUP | POLLERR)) {
			/* session indices may have shifted if nothing else removed any: look up by pollfd order only while sizes are unchanged */
			if (ns == L1.nsess) l1_drop_session(i);
		}
		while (qd_recv(c, &m, 0) == 1) { l1_handle(c, &m); qd_msg_free(&m); }
		{ int vt = l1_read_vt(); if (vt != L1.curvt) { L1.curvt = vt; l1_refresh_active(); } }
		l1_tick_action();
	}
	return 0;
}
#endif /* LOGIN1_IMPLEMENTATION_DONE */
#endif /* LOGIN1_IMPLEMENTATION */
