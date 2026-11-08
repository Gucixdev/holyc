/* login1_test: a fake client against a live qlogind on a private bus. Driven by tests/run_login1 (env: QT_BUS, QT_RUN, QT_DEVDIR, QT_VTFILE). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#define QDBUS_IMPLEMENTATION
#include "../src/mods/qdbus.h"

#define LOGIN "org.freedesktop.login1"
#define ROOT "/org/freedesktop/login1"
#define MGR LOGIN ".Manager"
#define SESS LOGIN ".Session"
static int fails, checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); fails++; } } while (0)

typedef struct { char member[40], path[100]; uint32_t a, b; char s[40]; int fd; int bval; } seen_t;
static seen_t seen[64]; static int nseen;

static void pump(qd_conn *c, int ms)
{
	qd_msg m;
	while (qd_recv(c, &m, ms) == 1) {
		if (m.type == 4 && nseen < 64) {
			seen_t *s = &seen[nseen++];
			qd_rd r = { m.body, 0, m.blen, 0 };
			memset(s, 0, sizeof *s); s->fd = -1;
			snprintf(s->member, sizeof s->member, "%s", m.member); snprintf(s->path, sizeof s->path, "%s", m.path);
			if (!strcmp(m.sig, "uus")) { s->a = qd_r_u32(&r); s->b = qd_r_u32(&r); snprintf(s->s, sizeof s->s, "%s", qd_r_str(&r)); }
			else if (!strcmp(m.sig, "uuh")) { s->a = qd_r_u32(&r); s->b = qd_r_u32(&r); if (m.nfds) { s->fd = m.fds[0]; m.fds[0] = -1; } }
			else if (!strcmp(m.sig, "b")) s->bval = qd_r_u32(&r);
			else if (!strcmp(m.sig, "so")) snprintf(s->s, sizeof s->s, "%s", qd_r_str(&r));
			else if (!strcmp(m.sig, "sa{sv}as")) { qd_r_str(&r); size_t end = qd_r_array(&r, 8); while (r.pos < end && !r.bad) { r.pos = (r.pos + 7) & ~7u; const char *k = qd_r_str(&r); const char *vs = qd_r_sig(&r); snprintf(s->s, sizeof s->s, "%s", k); s->bval = *vs == 'b' ? (int)qd_r_u32(&r) : 0; if (*vs != 'b') qd_r_skip(&r, &vs); } }
		}
		qd_msg_free(&m);
		ms = 0;
	}
}
static seen_t *find(qd_conn *c, const char *member, int wait_ms)
{
	for (int t = 0; t <= wait_ms; t += 50) {
		for (int i = nseen - 1; i >= 0; i--) if (!strcmp(seen[i].member, member)) return &seen[i];
		pump(c, 50);
	}
	return NULL;
}
static void forget(void) { for (int i = 0; i < nseen; i++) if (seen[i].fd >= 0) close(seen[i].fd); nseen = 0; }

static int call(qd_conn *c, qd_msg *r, const char *path, const char *iface, const char *member, const char *sig, const qd_buf *b)
{ return qd_call(c, r, LOGIN, path, iface, member, sig, b, NULL, 0, 4000); }

static int prop_bool(qd_conn *c, const char *path, const char *iface, const char *name, int *out)
{
	qd_buf b = {0}; qd_msg r;
	qd_put_str(&b, iface); qd_put_str(&b, name);
	int rc = call(c, &r, path, "org.freedesktop.DBus.Properties", "Get", "ss", &b);
	free(b.b);
	if (rc) { if (rc == 1) qd_msg_free(&r); return -1; }
	qd_rd rd = { r.body, 0, r.blen, 0 };
	const char *sg = qd_r_sig(&rd);
	*out = *sg == 'b' ? (int)qd_r_u32(&rd) : -1;
	qd_msg_free(&r);
	return *out < 0 ? -1 : 0;
}
static int prop_str(qd_conn *c, const char *path, const char *iface, const char *name, char *out, size_t n)
{
	qd_buf b = {0}; qd_msg r;
	qd_put_str(&b, iface); qd_put_str(&b, name);
	int rc = call(c, &r, path, "org.freedesktop.DBus.Properties", "Get", "ss", &b);
	free(b.b);
	if (rc) { if (rc == 1) qd_msg_free(&r); return -1; }
	qd_rd rd = { r.body, 0, r.blen, 0 };
	const char *sg = qd_r_sig(&rd);
	snprintf(out, n, "%s", *sg == 's' || *sg == 'o' ? qd_r_str(&rd) : "?");
	qd_msg_free(&r);
	return 0;
}
static int err_is(qd_conn *c, const char *path, const char *iface, const char *member, const char *sig, const qd_buf *b, const char *want)
{
	qd_msg r;
	int rc = call(c, &r, path, iface, member, sig, b);
	int ok = rc == 1 && !strcmp(r.err, want);
	if (rc == 1) { if (!ok) fprintf(stderr, "  got error %s: %s\n", r.err, r.blen > 4 ? (const char *)r.body + 4 : ""); qd_msg_free(&r); }
	return ok;
}

static int create_session(qd_conn *c, pid_t pid, unsigned vt, char *id, size_t idn, int *fifo, int *existing)
{
	qd_buf b = {0}; qd_msg r;
	qd_put_u32(&b, getuid()); qd_put_u32(&b, pid); qd_put_str(&b, "qtest"); qd_put_str(&b, "tty"); qd_put_str(&b, "user"); qd_put_str(&b, "");
	qd_put_str(&b, "seat0"); qd_put_u32(&b, vt); qd_put_str(&b, "/dev/tty1"); qd_put_str(&b, ""); qd_put_u32(&b, 0); qd_put_str(&b, ""); qd_put_str(&b, "");
	qd_arr a = qd_arr_begin(&b, 8); qd_arr_end(&b, a);
	int rc = call(c, &r, ROOT, MGR, "CreateSession", "uusssssussbssa(sv)", &b);
	free(b.b);
	if (rc) { if (rc == 1) { fprintf(stderr, "  CreateSession error %s\n", r.err); qd_msg_free(&r); } return -1; }
	qd_rd rd = { r.body, 0, r.blen, 0 };
	snprintf(id, idn, "%s", qd_r_str(&rd)); qd_r_str(&rd); qd_r_str(&rd); qd_r_u32(&rd); qd_r_u32(&rd); qd_r_str(&rd); qd_r_u32(&rd);
	*existing = qd_r_u32(&rd);
	CHECK(!rd.bad && !strcmp(r.sig, "soshusub"));
	*fifo = r.nfds == 1 ? r.fds[0] : -1;
	r.fds[0] = -1;
	qd_msg_free(&r);
	return 0;
}
static void sess_path(const char *id, char *o, size_t n) { snprintf(o, n, ROOT "/session/%s", id); }   /* "c1": no escaping needed */

static void write_vt(const char *f, int vt) { FILE *x = fopen(f, "w"); if (x) { fprintf(x, "tty%d\n", vt); fclose(x); } }
static int file_has(const char *path, const char *needle)
{
	char buf[4096] = "";
	FILE *f = fopen(path, "r");
	if (!f) return 0;
	size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0;
	fclose(f);
	return strstr(buf, needle) != NULL;
}

int main(void)
{
	const char *bus = getenv("QT_BUS"), *run = getenv("QT_RUN"), *devdir = getenv("QT_DEVDIR"), *vtfile = getenv("QT_VTFILE");
	qd_conn A, B;
	qd_msg r;
	qd_buf b = {0};
	char id1[16], id2[16], p1[100], p2[100], tmp[300], str[200];
	int fifo1, fifo2, ex, bv;
	if (!bus || !run || !devdir || !vtfile) { fputs("missing env\n", stderr); return 2; }
	CHECK(qd_open(&A, bus) == 0);
	CHECK(qd_open(&B, bus) == 0);
	/* signals from the service */
	b.n = 0;
	qd_put_str(&b, "type='signal',sender='" LOGIN "'");
	CHECK(qd_call(&A, &r, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "AddMatch", "s", &b, NULL, 0, 3000) == 0);
	qd_msg_free(&r);

	/* introspection comes from the tables */
	CHECK(call(&A, &r, ROOT, "org.freedesktop.DBus.Introspectable", "Introspect", NULL, NULL) == 0);
	{ qd_rd rd = { r.body, 0, r.blen, 0 }; const char *xml = qd_r_str(&rd);
	  CHECK(strstr(xml, "name=\"" MGR "\"") && strstr(xml, "name=\"CreateSession\"") && strstr(xml, "name=\"Inhibit\"") && strstr(xml, "name=\"PrepareForShutdown\"")
	        && strstr(xml, "name=\"BlockInhibited\"") && strstr(xml, "org.freedesktop.DBus.Properties")); }
	qd_msg_free(&r);
	CHECK(call(&A, &r, ROOT, "org.freedesktop.DBus.Peer", "Ping", NULL, NULL) == 0); qd_msg_free(&r);

	/* sessions */
	write_vt(vtfile, 1);
	usleep(700000);   /* the daemon polls the VT file */
	CHECK(create_session(&A, getpid(), 1, id1, sizeof id1, &fifo1, &ex) == 0);
	CHECK(!strcmp(id1, "c1") && !ex && fifo1 >= 0);
	sess_path(id1, p1, sizeof p1);
	CHECK(find(&A, "SessionNew", 1000) != NULL);
	CHECK(find(&A, "UserNew", 200) != NULL);
	CHECK(prop_bool(&A, p1, SESS, "Active", &bv) == 0 && bv == 1);
	CHECK(prop_str(&A, p1, SESS, "Type", str, sizeof str) == 0 && !strcmp(str, "tty"));
	CHECK(prop_str(&A, p1, SESS, "State", str, sizeof str) == 0 && !strcmp(str, "active"));
	CHECK(call(&A, &r, p1, "org.freedesktop.DBus.Properties", "GetAll", "s", (b.n = 0, qd_put_str(&b, SESS), &b)) == 0 && r.blen > 200); qd_msg_free(&r);
	CHECK(call(&A, &r, ROOT, MGR, "GetSession", "s", (b.n = 0, qd_put_str(&b, id1), &b)) == 0); { qd_rd rd = { r.body, 0, r.blen, 0 }; CHECK(!strcmp(qd_r_str(&rd), p1)); } qd_msg_free(&r);
	CHECK(err_is(&A, ROOT, MGR, "GetSession", "s", (b.n = 0, qd_put_str(&b, "zz"), &b), LOGIN ".NoSuchSession"));
	CHECK(err_is(&A, ROOT, MGR, "NoSuchMethod", NULL, NULL, "org.freedesktop.DBus.Error.UnknownMethod"));
	CHECK(err_is(&A, ROOT, MGR, "GetSession", "u", (b.n = 0, qd_put_u32(&b, 1), &b), "org.freedesktop.DBus.Error.InvalidArgs"));
	CHECK(err_is(&A, "/org/freedesktop/login1/session/nope", SESS, "Activate", NULL, NULL, "org.freedesktop.DBus.Error.UnknownObject"));
	CHECK(call(&A, &r, ROOT, MGR, "ListSessions", NULL, NULL) == 0 && !strcmp(r.sig, "a(susso)") && r.blen > 40); qd_msg_free(&r);
	CHECK(call(&A, &r, ROOT, MGR, "ListSeats", NULL, NULL) == 0); qd_msg_free(&r);
	CHECK(call(&A, &r, ROOT, MGR, "ListUsers", NULL, NULL) == 0 && r.blen > 20); qd_msg_free(&r);
	CHECK(call(&A, &r, ROOT, MGR, "GetSessionByPID", "u", (b.n = 0, qd_put_u32(&b, getpid()), &b)) == 0); qd_msg_free(&r);
	pid_t kid = fork();
	if (kid == 0) { close(A.fd); close(B.fd); close(fifo1); pause(); _exit(0); }
	CHECK(call(&A, &r, ROOT, MGR, "GetSessionByPID", "u", (b.n = 0, qd_put_u32(&b, kid), &b)) == 0);   /* descendant of the leader */
	qd_msg_free(&r);
	CHECK(err_is(&A, ROOT, MGR, "GetSessionByPID", "u", (b.n = 0, qd_put_u32(&b, 1), &b), LOGIN ".NoSessionForPID"));
	CHECK(call(&A, &r, ROOT "/session/auto", "org.freedesktop.DBus.Properties", "Get", "ss", (b.n = 0, qd_put_str(&b, SESS), qd_put_str(&b, "Id"), &b)) == 0); qd_msg_free(&r);
	CHECK(call(&A, &r, ROOT "/seat/seat0", "org.freedesktop.DBus.Properties", "GetAll", "s", (b.n = 0, qd_put_str(&b, LOGIN ".Seat"), &b)) == 0); qd_msg_free(&r);
	snprintf(tmp, sizeof tmp, ROOT "/user/_%02x%02x%02x%02x", '0' + (int)(getuid() / 1000 % 10) + 0, 0, 0, 0);   /* replaced just below by GetUser */
	CHECK(call(&A, &r, ROOT, MGR, "GetUser", "u", (b.n = 0, qd_put_u32(&b, getuid()), &b)) == 0);
	{ qd_rd rd = { r.body, 0, r.blen, 0 }; snprintf(tmp, sizeof tmp, "%s", qd_r_str(&rd)); } qd_msg_free(&r);
	CHECK(prop_str(&A, tmp, LOGIN ".User", "RuntimePath", str, sizeof str) == 0 && strstr(str, run));
	snprintf(str, sizeof str, "%s/user/%u", run, (unsigned)getuid()); { struct stat st; CHECK(stat(str, &st) == 0 && S_ISDIR(st.st_mode) && (st.st_mode & 077) == 0); }
	snprintf(str, sizeof str, "%s/systemd/sessions/c1", run); CHECK(file_has(str, "ACTIVE=1") && file_has(str, "SEAT=seat0") && file_has(str, "VTNR=1"));
	snprintf(str, sizeof str, "%s/systemd/seats/seat0", run); CHECK(file_has(str, "ACTIVE=c1"));
	/* second session on VT 2, switching */
	CHECK(create_session(&A, kid, 2, id2, sizeof id2, &fifo2, &ex) == 0 && !strcmp(id2, "c2"));
	sess_path(id2, p2, sizeof p2);
	CHECK(prop_bool(&A, p2, SESS, "Active", &bv) == 0 && bv == 0);
	CHECK(create_session(&A, kid, 2, str, sizeof str, &(int){0}, &ex) == 0 && ex == 1 && !strcmp(str, id2));   /* same leader = existing session */

	/* devices: mock layer, real SCM_RIGHTS */
	snprintf(tmp, sizeof tmp, "%s/13:64", devdir);
	{ FILE *f = fopen(tmp, "w"); fputs("evdev-mock", f); fclose(f); }
	CHECK(err_is(&B, p1, SESS, "TakeDevice", "uu", (b.n = 0, qd_put_u32(&b, 13), qd_put_u32(&b, 64), &b), "System.Error.EPERM"));   /* no control yet */
	CHECK(call(&A, &r, p1, SESS, "TakeControl", "b", (b.n = 0, qd_put_u32(&b, 0), &b)) == 0); qd_msg_free(&r);
	CHECK(err_is(&B, p1, SESS, "TakeControl", "b", (b.n = 0, qd_put_u32(&b, 0), &b), "System.Error.EBUSY"));
	CHECK(call(&A, &r, p1, SESS, "SetType", "s", (b.n = 0, qd_put_str(&b, "wayland"), &b)) == 0); qd_msg_free(&r);
	CHECK(prop_str(&A, p1, SESS, "Type", str, sizeof str) == 0 && !strcmp(str, "wayland"));
	CHECK(call(&A, &r, p1, SESS, "TakeDevice", "uu", (b.n = 0, qd_put_u32(&b, 13), qd_put_u32(&b, 64), &b)) == 0);
	CHECK(!strcmp(r.sig, "hb") && r.nfds == 1);
	if (r.nfds == 1) { char buf[32] = ""; qd_rd rd = { r.body, 0, r.blen, 0 }; qd_r_u32(&rd); CHECK(qd_r_u32(&rd) == 0); CHECK(read(r.fds[0], buf, 31) == 10 && !strcmp(buf, "evdev-mock")); }
	qd_msg_free(&r);
	CHECK(err_is(&A, p1, SESS, "TakeDevice", "uu", (b.n = 0, qd_put_u32(&b, 13), qd_put_u32(&b, 99), &b), "System.Error.ENODEV"));
	forget();
	write_vt(vtfile, 2);   /* switch away: PauseDevice to the controller, Active false */
	{ seen_t *s = find(&A, "PauseDevice", 2500); CHECK(s && s->a == 13 && s->b == 64 && !strcmp(s->s, "pause")); }
	CHECK(prop_bool(&A, p1, SESS, "Active", &bv) == 0 && bv == 0);
	CHECK(prop_bool(&A, p2, SESS, "Active", &bv) == 0 && bv == 1);
	{ seen_t *s = find(&A, "PropertiesChanged", 100); CHECK(s != NULL); }
	CHECK(call(&A, &r, p1, SESS, "PauseDeviceComplete", "uu", (b.n = 0, qd_put_u32(&b, 13), qd_put_u32(&b, 64), &b)) == 0); qd_msg_free(&r);
	snprintf(str, sizeof str, "%s/systemd/seats/seat0", run); CHECK(file_has(str, "ACTIVE=c2"));
	forget();
	CHECK(call(&A, &r, ROOT, MGR, "ActivateSession", "s", (b.n = 0, qd_put_str(&b, id1), &b)) == 0); qd_msg_free(&r);   /* back: ResumeDevice with an fd */
	{ seen_t *s = find(&A, "ResumeDevice", 2500); CHECK(s && s->a == 13 && s->b == 64 && s->fd >= 0);
	  if (s && s->fd >= 0) { char buf[32] = ""; lseek(s->fd, 0, SEEK_SET); CHECK(read(s->fd, buf, 31) == 10 && !strcmp(buf, "evdev-mock")); } }
	CHECK(prop_bool(&A, p1, SESS, "Active", &bv) == 0 && bv == 1);
	CHECK(call(&A, &r, p1, SESS, "ReleaseDevice", "uu", (b.n = 0, qd_put_u32(&b, 13), qd_put_u32(&b, 64), &b)) == 0); qd_msg_free(&r);
	CHECK(call(&A, &r, p1, SESS, "ReleaseControl", NULL, NULL) == 0); qd_msg_free(&r);
	/* a controller that vanishes loses control */
	CHECK(call(&B, &r, p1, SESS, "TakeControl", "b", (b.n = 0, qd_put_u32(&b, 0), &b)) == 0); qd_msg_free(&r);
	qd_close(&B);
	usleep(300000);
	{ int rc = call(&A, &r, p1, SESS, "TakeControl", "b", (b.n = 0, qd_put_u32(&b, 0), &b)); CHECK(rc == 0); if (rc == 1) fprintf(stderr, "  TakeControl: %s\n", r.err); if (rc >= 0) qd_msg_free(&r); }
	CHECK(call(&A, &r, p1, SESS, "SetIdleHint", "b", (b.n = 0, qd_put_u32(&b, 1), &b)) == 0); qd_msg_free(&r);
	CHECK(prop_bool(&A, p1, SESS, "IdleHint", &bv) == 0 && bv == 1);
	CHECK(call(&A, &r, p1, SESS, "SetLockedHint", "b", (b.n = 0, qd_put_u32(&b, 1), &b)) == 0); qd_msg_free(&r);
	forget();
	CHECK(call(&A, &r, p1, SESS, "Lock", NULL, NULL) == 0); qd_msg_free(&r);
	CHECK(find(&A, "Lock", 500) != NULL);

	/* inhibitors + power (dry run) */
	{
		int ifd, sfd;
		snprintf(tmp, sizeof tmp, "%s/qlogind/actions.log", run);
		CHECK(call(&A, &r, ROOT, MGR, "CanPowerOff", NULL, NULL) == 0); { qd_rd rd = { r.body, 0, r.blen, 0 }; CHECK(!strcmp(qd_r_str(&rd), "yes")); } qd_msg_free(&r);
		CHECK(err_is(&A, ROOT, MGR, "Inhibit", "ssss", (b.n = 0, qd_put_str(&b, "bogus"), qd_put_str(&b, "t"), qd_put_str(&b, "w"), qd_put_str(&b, "block"), &b), "org.freedesktop.DBus.Error.InvalidArgs"));
		CHECK(call(&A, &r, ROOT, MGR, "Inhibit", "ssss", (b.n = 0, qd_put_str(&b, "shutdown"), qd_put_str(&b, "qtest"), qd_put_str(&b, "delay test"), qd_put_str(&b, "delay"), &b)) == 0);
		CHECK(!strcmp(r.sig, "h") && r.nfds == 1);
		ifd = r.nfds ? r.fds[0] : -1; r.fds[0] = -1; qd_msg_free(&r);
		CHECK(prop_str(&A, ROOT, MGR, "DelayInhibited", str, sizeof str) == 0 && !strcmp(str, "shutdown"));
		CHECK(call(&A, &r, ROOT, MGR, "ListInhibitors", NULL, NULL) == 0 && r.blen > 30); qd_msg_free(&r);
		forget();
		CHECK(call(&A, &r, ROOT, MGR, "PowerOff", "b", (b.n = 0, qd_put_u32(&b, 0), &b)) == 0); qd_msg_free(&r);
		{ seen_t *s = find(&A, "PrepareForShutdown", 1000); CHECK(s && s->bval == 1); }
		CHECK(err_is(&A, ROOT, MGR, "Reboot", "b", (b.n = 0, qd_put_u32(&b, 0), &b), LOGIN ".OperationInProgress"));
		usleep(300000);
		CHECK(!file_has(tmp, "DRYRUN"));   /* held back by the delay lock */
		forget();
		close(ifd);   /* release: the action runs, preparing goes false again (dry run) */
		{ seen_t *s = find(&A, "PrepareForShutdown", 3000); CHECK(s && s->bval == 0); }
		CHECK(file_has(tmp, "DRYRUN PowerOff"));
		/* delay lock that is never released: the deadline (conf inhibit_delay_max=1) fires */
		CHECK(call(&A, &r, ROOT, MGR, "Inhibit", "ssss", (b.n = 0, qd_put_str(&b, "sleep"), qd_put_str(&b, "qtest"), qd_put_str(&b, "stuck"), qd_put_str(&b, "delay"), &b)) == 0);
		ifd = r.fds[0]; r.fds[0] = -1; qd_msg_free(&r);
		CHECK(call(&A, &r, ROOT, MGR, "Suspend", "b", (b.n = 0, qd_put_u32(&b, 0), &b)) == 0); qd_msg_free(&r);
		usleep(300000); CHECK(!file_has(tmp, "DRYRUN Suspend"));
		usleep(1500000); CHECK(file_has(tmp, "DRYRUN Suspend"));
		close(ifd);
		/* block lock refuses */
		CHECK(call(&A, &r, ROOT, MGR, "Inhibit", "ssss", (b.n = 0, qd_put_str(&b, "sleep:shutdown"), qd_put_str(&b, "qtest"), qd_put_str(&b, "blocker"), qd_put_str(&b, "block"), &b)) == 0);
		sfd = r.fds[0]; r.fds[0] = -1; qd_msg_free(&r);
		CHECK(prop_str(&A, ROOT, MGR, "BlockInhibited", str, sizeof str) == 0 && !strcmp(str, "shutdown:sleep"));
		CHECK(err_is(&A, ROOT, MGR, "Hibernate", "b", (b.n = 0, qd_put_u32(&b, 0), &b), LOGIN ".BlockedByInhibitor"));
		close(sfd);
		usleep(400000);
		CHECK(prop_str(&A, ROOT, MGR, "BlockInhibited", str, sizeof str) == 0 && !*str);
	}


	/* the rest of the Manager/Session/Seat/User surface */
	{
		char actlog[256];
		snprintf(actlog, sizeof actlog, "%s/qlogind/actions.log", run);
		static const char *const cans[] = { "CanPowerOff", "CanReboot", "CanHalt", "CanSuspend", "CanHibernate", "CanHybridSleep", "CanSuspendThenHibernate" };
		static const char *const acts[] = { "Reboot", "Halt", "Hibernate", "HybridSleep", "SuspendThenHibernate" };
		static const char *const mprops[] = { "NAutoVTs", "KillUserProcesses", "IdleHint", "IdleSinceHint", "IdleSinceHintMonotonic", "BlockWeakInhibited",
			"UserStopDelayUSec", "HandlePowerKey", "HandleSuspendKey", "HandleHibernateKey", "HandleLidSwitch", "HandleLidSwitchExternalPower",
			"HandleLidSwitchDocked", "PreparingForShutdown", "PreparingForSleep", "InhibitorsMax", "NCurrentInhibitors", "SessionsMax", "NCurrentSessions" };
		static const char *const sprops[] = { "User", "Name", "VTNr", "Seat", "TTY", "Display", "Remote", "RemoteHost", "RemoteUser", "Service", "Desktop",
			"Scope", "Leader", "Class", "IdleSinceHint", "IdleSinceHintMonotonic", "CanIdle", "CanLock", "LockedHint" };
		int ok = 1;
		for (unsigned i = 0; i < sizeof cans / sizeof *cans; i++) {
			int rc = call(&A, &r, ROOT, MGR, cans[i], NULL, NULL);
			if (rc != 0) { ok = 0; if (rc == 1) qd_msg_free(&r); continue; }
			qd_rd rd = { r.body, 0, r.blen, 0 }; const char *v = qd_r_str(&rd);
			if (strcmp(v, "yes") && strcmp(v, "no") && strcmp(v, "na") && strcmp(v, "challenge")) ok = 0;
			qd_msg_free(&r);
		}
		CHECK(ok);
		ok = 1;
		for (unsigned i = 0; i < sizeof mprops / sizeof *mprops; i++) {
			qd_buf q = {0};
			qd_put_str(&q, MGR); qd_put_str(&q, mprops[i]);
			int rc = call(&A, &r, ROOT, "org.freedesktop.DBus.Properties", "Get", "ss", &q);
			free(q.b);
			if (rc != 0) { ok = 0; fprintf(stderr, "  Manager property %s: rc %d\n", mprops[i], rc); if (rc == 1) qd_msg_free(&r); } else qd_msg_free(&r);
		}
		CHECK(ok);
		ok = 1;
		for (unsigned i = 0; i < sizeof sprops / sizeof *sprops; i++) {
			qd_buf q = {0};
			qd_put_str(&q, SESS); qd_put_str(&q, sprops[i]);
			int rc = call(&A, &r, p1, "org.freedesktop.DBus.Properties", "Get", "ss", &q);
			free(q.b);
			if (rc != 0) { ok = 0; fprintf(stderr, "  Session property %s: rc %d\n", sprops[i], rc); if (rc == 1) qd_msg_free(&r); } else qd_msg_free(&r);
		}
		CHECK(ok);
		CHECK(prop_bool(&A, p1, SESS, "LockedHint", &bv) == 0 && bv == 1);   /* set above */
		CHECK(prop_str(&A, ROOT "/seat/seat0", LOGIN ".Seat", "Id", str, sizeof str) == 0 && !strcmp(str, "seat0"));
		CHECK(call(&A, &r, ROOT "/seat/seat0", "org.freedesktop.DBus.Properties", "Get", "ss", (b.n = 0, qd_put_str(&b, LOGIN ".Seat"), qd_put_str(&b, "Sessions"), &b)) == 0); qd_msg_free(&r);
		CHECK(call(&A, &r, ROOT "/seat/seat0", "org.freedesktop.DBus.Properties", "Get", "ss", (b.n = 0, qd_put_str(&b, LOGIN ".Seat"), qd_put_str(&b, "ActiveSession"), &b)) == 0); qd_msg_free(&r);

		/* GetSeat / GetUserByPID */
		CHECK(call(&A, &r, ROOT, MGR, "GetSeat", "s", (b.n = 0, qd_put_str(&b, "seat0"), &b)) == 0);
		{ qd_rd rd = { r.body, 0, r.blen, 0 }; CHECK(!strcmp(qd_r_str(&rd), ROOT "/seat/seat0")); } qd_msg_free(&r);
		CHECK(err_is(&A, ROOT, MGR, "GetSeat", "s", (b.n = 0, qd_put_str(&b, "seat9"), &b), LOGIN ".NoSuchSeat"));
		char upath[100];
		CHECK(call(&A, &r, ROOT, MGR, "GetUser", "u", (b.n = 0, qd_put_u32(&b, getuid()), &b)) == 0);
		{ qd_rd rd = { r.body, 0, r.blen, 0 }; snprintf(upath, sizeof upath, "%s", qd_r_str(&rd)); } qd_msg_free(&r);
		CHECK(call(&A, &r, ROOT, MGR, "GetUserByPID", "u", (b.n = 0, qd_put_u32(&b, getpid()), &b)) == 0);
		{ qd_rd rd = { r.body, 0, r.blen, 0 }; CHECK(!strcmp(qd_r_str(&rd), upath)); } qd_msg_free(&r);
		CHECK(err_is(&A, ROOT, MGR, "GetUserByPID", "u", (b.n = 0, qd_put_u32(&b, 1), &b), LOGIN ".NoSuchUser"));
		CHECK(err_is(&A, ROOT, MGR, "GetUser", "u", (b.n = 0, qd_put_u32(&b, 4000000), &b), LOGIN ".NoSuchUser"));

		/* lock / unlock, per session and for all */
		forget();
		CHECK(call(&A, &r, ROOT, MGR, "LockSession", "s", (b.n = 0, qd_put_str(&b, id1), &b)) == 0); qd_msg_free(&r);
		{ seen_t *x = find(&A, "Lock", 500); CHECK(x && !strcmp(x->path, p1)); }
		forget();
		CHECK(call(&A, &r, ROOT, MGR, "UnlockSession", "s", (b.n = 0, qd_put_str(&b, id1), &b)) == 0); qd_msg_free(&r);
		{ seen_t *x = find(&A, "Unlock", 500); CHECK(x && !strcmp(x->path, p1)); }
		forget();
		CHECK(call(&A, &r, ROOT, MGR, "LockSessions", NULL, NULL) == 0); qd_msg_free(&r);
		{ seen_t *x = find(&A, "Lock", 500); CHECK(x != NULL); }
		forget();
		CHECK(call(&A, &r, ROOT, MGR, "UnlockSessions", NULL, NULL) == 0); qd_msg_free(&r);
		{ seen_t *x = find(&A, "Unlock", 500); CHECK(x != NULL); }
		forget();
		CHECK(call(&A, &r, p1, SESS, "Unlock", NULL, NULL) == 0); qd_msg_free(&r);
		{ seen_t *x = find(&A, "Unlock", 500); CHECK(x && !strcmp(x->path, p1)); }
		CHECK(err_is(&A, ROOT, MGR, "LockSession", "s", (b.n = 0, qd_put_str(&b, "zz"), &b), LOGIN ".NoSuchSession"));

		/* Session.Activate (mock VT switch), then back */
		CHECK(call(&A, &r, p2, SESS, "Activate", NULL, NULL) == 0); qd_msg_free(&r);
		usleep(900000);
		CHECK(prop_bool(&A, p2, SESS, "Active", &bv) == 0 && bv == 1);
		CHECK(call(&A, &r, p1, SESS, "Activate", NULL, NULL) == 0); qd_msg_free(&r);
		usleep(900000);
		CHECK(prop_bool(&A, p1, SESS, "Active", &bv) == 0 && bv == 1);

		/* remaining power actions: dry run, one at a time */
		forget();
		for (unsigned i = 0; i < sizeof acts / sizeof *acts; i++) {
			char want[64];
			CHECK(call(&A, &r, ROOT, MGR, acts[i], "b", (b.n = 0, qd_put_u32(&b, 0), &b)) == 0); qd_msg_free(&r);
			usleep(400000);
			snprintf(want, sizeof want, "DRYRUN %s", acts[i]);
			CHECK(file_has(actlog, want));
		}
		{ seen_t *x = find(&A, "PrepareForSleep", 500); CHECK(x != NULL); }

		/* KillSession: signal the leader of c2 (a forked child, never ourselves) */
		CHECK(err_is(&A, ROOT, MGR, "KillSession", "ssi", (b.n = 0, qd_put_str(&b, id2), qd_put_str(&b, "bogus"), qd_put_u32(&b, 10), &b), "org.freedesktop.DBus.Error.InvalidArgs"));
		CHECK(call(&A, &r, ROOT, MGR, "KillSession", "ssi", (b.n = 0, qd_put_str(&b, id2), qd_put_str(&b, "leader"), qd_put_u32(&b, SIGUSR1), &b)) == 0); qd_msg_free(&r);
		{ int st = 0; CHECK(waitpid(kid, &st, 0) == kid && WIFSIGNALED(st) && WTERMSIG(st) == SIGUSR1); }
	}

	/* release + fifo semantics */
	forget();
	CHECK(call(&A, &r, ROOT, MGR, "ReleaseSession", "s", (b.n = 0, qd_put_str(&b, id2), &b)) == 0); qd_msg_free(&r);
	CHECK(find(&A, "SessionRemoved", 1000) != NULL);
	snprintf(str, sizeof str, "%s/systemd/sessions/c2", run); CHECK(access(str, F_OK) != 0);
	kill(kid, SIGKILL); waitpid(kid, NULL, 0);   /* the child inherited a copy of the fifo write end */
	close(fifo1);   /* closing the last writer ends session c1 */
	usleep(800000);
	CHECK(err_is(&A, ROOT, MGR, "GetSession", "s", (b.n = 0, qd_put_str(&b, id1), &b), LOGIN ".NoSuchSession"));
	snprintf(str, sizeof str, "%s/user/%u", run, (unsigned)getuid()); { struct stat st; CHECK(stat(str, &st) != 0); }   /* empty runtime dir removed with the last session */
	close(fifo2);
	CHECK(find(&A, "UserRemoved", 1500) != NULL);
	free(b.b);
	qd_close(&A);
	printf("login1_test: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
