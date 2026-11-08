/* fakes - a stateful fake iwd + bluez on a private dbus-daemon, for driving the TUI (or the CLI) without touching anything real.
 *
 *   cc -std=gnu11 -Wall -Wextra -O2 -I../../notcoreutils -o /tmp/nm_fakes fakes.c
 *   /tmp/nm_fakes LOGDIR COMMAND [ARGS...]     runs COMMAND with DBUS_SYSTEM_BUS_ADDRESS pointing at the fake; every call the fake receives is
 *                                              appended to LOGDIR/calls.log; exits with COMMAND's status
 *
 * Wi-Fi: Home (saved, autoconnect, in range), Guest cafe (open), Neighbour (psk, strong), Office 5G (psk, saved, weak), Zażółć (psk).
 * Bluetooth: Fake Pad (paired), Headphones (paired, connected), Phone (unpaired). Connect / Disconnect / Forget / AutoConnect / Trusted /
 * Powered change the state the next listing shows. NMB_WPA_DIR points at an empty directory so the iwd path is taken.
 * What this cannot show: how real iwd / bluez behave.
 */
#define _GNU_SOURCE
#define NMB_IMPLEMENTATION
#include "netmanager_blue.h"
#include <sys/wait.h>

static struct { const char *name, *type; int dbm, known, autoc; } nets[] = {
	{ "Home", "psk", -52, 1, 1 }, { "Guest cafe", "open", -71, 0, 0 }, { "Neighbour", "psk", -48, 0, 0 },
	{ "Office 5G", "psk", -82, 1, 0 }, { "Zażółć", "psk", -66, 0, 0 },
};
#define NNET (int)(sizeof nets / sizeof *nets)
static int connected = -1;
static struct { const char *addr, *alias; int paired, conn, trusted, rssi; } devs[] = {
	{ "AA:BB:CC:DD:EE:FF", "Fake Pad", 1, 0, 0, -60 }, { "11:22:33:44:55:66", "Headphones", 1, 1, 1, -45 }, { "00:11:22:33:44:55", "Phone", 0, 0, 0, 0 },
};
#define NDEV (int)(sizeof devs / sizeof *devs)
static int powered = 1;
static char logpath[520];

static void flog(const char *s) { FILE *f = fopen(logpath, "a"); if (f) { fprintf(f, "%s\n", s); fclose(f); } }
static void arr_begin(nmb__Buf *b, size_t *lp, size_t *st) { nmb__align(b, 4); *lp = b->n; nmb__put_u32(b, 0); nmb__align(b, 8); *st = b->n; }
static void arr_end(nmb__Buf *b, size_t lp, size_t st) { uint32_t l = b->n - st; memcpy(b->b + lp, &l, 4); }
static void prop_s(nmb__Buf *b, const char *k, const char *v) { nmb__align(b, 8); nmb__put_str(b, k); nmb__put_sig(b, "s"); nmb__put_str(b, v); }
static void prop_o(nmb__Buf *b, const char *k, const char *v) { nmb__align(b, 8); nmb__put_str(b, k); nmb__put_sig(b, "o"); nmb__put_str(b, v); }
static void prop_b(nmb__Buf *b, const char *k, int v) { nmb__align(b, 8); nmb__put_str(b, k); nmb__put_sig(b, "b"); nmb__put_u32(b, v); }
static void prop_n(nmb__Buf *b, const char *k, int v) { int16_t x = v; nmb__align(b, 8); nmb__put_str(b, k); nmb__put_sig(b, "n"); nmb__align(b, 2); nmb__put(b, &x, 2); }

static void begin_obj(nmb__Buf *b, const char *path, size_t *lp, size_t *st) { nmb__align(b, 8); nmb__put_str(b, path); arr_begin(b, lp, st); }
static void begin_if(nmb__Buf *b, const char *name, size_t *lp, size_t *st) { nmb__align(b, 8); nmb__put_str(b, name); arr_begin(b, lp, st); }

static void netpath(char *o, size_t n, int i) { snprintf(o, n, "/net/connman/iwd/0/3/n%d", i); }
static void knownpath(char *o, size_t n, int i) { snprintf(o, n, "/net/connman/iwd/k%d", i); }

static void managed(nmb__Buf *out, int bluez)
{
	size_t lp, st, lp2, st2;
	arr_begin(out, &lp, &st);
	if (bluez) {
		begin_obj(out, "/org/bluez/hci0", &lp2, &st2);
		{ size_t l3, s3; begin_if(out, "org.bluez.Adapter1", &l3, &s3); prop_b(out, "Powered", powered); arr_end(out, l3, s3); }
		arr_end(out, lp2, st2);
		for (int i = 0; i < NDEV; i++) {
			char p[80];
			size_t l3, s3;
			snprintf(p, sizeof p, "/org/bluez/hci0/dev_%d", i);
			begin_obj(out, p, &lp2, &st2);
			begin_if(out, "org.bluez.Device1", &l3, &s3);
			prop_s(out, "Address", devs[i].addr); prop_s(out, "Alias", devs[i].alias); prop_b(out, "Paired", devs[i].paired);
			prop_b(out, "Connected", devs[i].conn); prop_b(out, "Trusted", devs[i].trusted);
			if (devs[i].rssi) prop_n(out, "RSSI", devs[i].rssi);
			arr_end(out, l3, s3);
			arr_end(out, lp2, st2);
		}
	} else {
		char p[80];
		size_t l3, s3;
		begin_obj(out, "/net/connman/iwd/0/3", &lp2, &st2);
		begin_if(out, "net.connman.iwd.Device", &l3, &s3); prop_s(out, "Name", "wlan0"); prop_s(out, "Mode", "station"); arr_end(out, l3, s3);
		begin_if(out, "net.connman.iwd.Station", &l3, &s3);
		prop_s(out, "State", connected >= 0 ? "connected" : "disconnected"); prop_b(out, "Scanning", 0);
		if (connected >= 0) { netpath(p, sizeof p, connected); prop_o(out, "ConnectedNetwork", p); }
		arr_end(out, l3, s3);
		arr_end(out, lp2, st2);
		for (int i = 0; i < NNET; i++) {
			char kp[80];
			netpath(p, sizeof p, i);
			begin_obj(out, p, &lp2, &st2);
			begin_if(out, "net.connman.iwd.Network", &l3, &s3);
			prop_s(out, "Name", nets[i].name); prop_s(out, "Type", nets[i].type);
			if (nets[i].known) { knownpath(kp, sizeof kp, i); prop_o(out, "KnownNetwork", kp); }
			arr_end(out, l3, s3);
			arr_end(out, lp2, st2);
			if (nets[i].known) {
				knownpath(kp, sizeof kp, i);
				begin_obj(out, kp, &lp2, &st2);
				begin_if(out, "net.connman.iwd.KnownNetwork", &l3, &s3);
				prop_s(out, "Name", nets[i].name); prop_s(out, "Type", nets[i].type); prop_b(out, "AutoConnect", nets[i].autoc);
				arr_end(out, l3, s3);
				arr_end(out, lp2, st2);
			}
		}
	}
	arr_end(out, lp, st);
}

static int idx_of(const char *path, const char *prefix)
{
	size_t l = strlen(prefix);
	return !strncmp(path, prefix, l) ? atoi(path + l) : -1;
}

static void service(int ready)
{
	int fd = nmb__connect_bus(getenv("DBUS_SYSTEM_BUS_ADDRESS"));
	nmb__Msg m;
	if (fd < 0 || nmb__bus_hello(fd)) _exit(2);
	nmb__call_timeout = 3000;
	{
		const char *names[] = { "org.bluez", "net.connman.iwd" };
		for (int i = 0; i < 2; i++) {
			nmb__Buf b = {0};
			nmb__put_str(&b, names[i]); nmb__put_u32(&b, 0);
			if (nmb__call(fd, &m, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName", "su", &b) || m.type != 2) _exit(3);
			free(b.b);
		}
	}
	if (write(ready, "x", 1) != 1) _exit(1);
	for (;;) {
		nmb__Buf out = {0};
		char line[300];
		int i;
		if (nmb__read_msg(fd, &m, 200)) { if (getppid() == 1) _exit(0); continue; }
		if (m.type != 1) continue;
		if (strcmp(m.iface, "org.freedesktop.DBus.Introspectable")) { snprintf(line, sizeof line, "%s %s.%s", m.path, m.iface, m.member); flog(line); }
		if (!strcmp(m.member, "GetManagedObjects")) {
			managed(&out, !strcmp(m.dest, "org.bluez"));
			nmb__reply_to_call(fd, &m, "a{oa{sa{sv}}}", &out);
		} else if (!strcmp(m.member, "GetOrderedNetworks")) {
			size_t lp, st;
			char p[80];
			arr_begin(&out, &lp, &st);
			for (i = 0; i < NNET; i++) {   /* a(on): path + signal in 1/100 dBm, strongest first would be nicer but the client sorts nothing */
				int16_t s = nets[i].dbm * 100;
				netpath(p, sizeof p, i);
				nmb__align(&out, 8); nmb__put_str(&out, p); nmb__align(&out, 2); nmb__put(&out, &s, 2);
			}
			arr_end(&out, lp, st);
			nmb__reply_to_call(fd, &m, "a(on)", &out);
		} else if (!strcmp(m.member, "Connect") && (i = idx_of(m.path, "/net/connman/iwd/0/3/n")) >= 0 && i < NNET) {
			connected = i; nets[i].known = 1;
			nmb__reply_to_call(fd, &m, NULL, NULL);
		} else if (!strcmp(m.member, "Disconnect") && !strcmp(m.iface, "net.connman.iwd.Station")) {
			connected = -1;
			nmb__reply_to_call(fd, &m, NULL, NULL);
		} else if (!strcmp(m.member, "Forget") && (i = idx_of(m.path, "/net/connman/iwd/k")) >= 0 && i < NNET) {
			nets[i].known = 0; nets[i].autoc = 0;
			if (connected == i) connected = -1;
			nmb__reply_to_call(fd, &m, NULL, NULL);
		} else if (!strcmp(m.member, "Connect") && (i = idx_of(m.path, "/org/bluez/hci0/dev_")) >= 0 && i < NDEV) {
			devs[i].conn = 1;
			nmb__reply_to_call(fd, &m, NULL, NULL);
		} else if (!strcmp(m.member, "Disconnect") && (i = idx_of(m.path, "/org/bluez/hci0/dev_")) >= 0 && i < NDEV) {
			devs[i].conn = 0;
			nmb__reply_to_call(fd, &m, NULL, NULL);
		} else if (!strcmp(m.member, "Set") && !strcmp(m.iface, "org.freedesktop.DBus.Properties")) {
			nmb__R r = { m.body, 0, m.blen, 0 };
			const char *key;
			char v[40];
			nmb__r_str(&r);
			key = nmb__r_str(&r);
			nmb__variant_str(&r, v, sizeof v);
			snprintf(line, sizeof line, "   set %s=%s", key, v);
			flog(line);
			if (!strcmp(key, "AutoConnect") && (i = idx_of(m.path, "/net/connman/iwd/k")) >= 0 && i < NNET) nets[i].autoc = !strcmp(v, "true");
			else if (!strcmp(key, "Trusted") && (i = idx_of(m.path, "/org/bluez/hci0/dev_")) >= 0 && i < NDEV) devs[i].trusted = !strcmp(v, "true");
			else if (!strcmp(key, "Powered")) powered = !strcmp(v, "true");
			nmb__reply_to_call(fd, &m, NULL, NULL);
		} else nmb__reply_to_call(fd, &m, NULL, NULL);
		free(out.b);
	}
}

int main(int argc, char **argv)
{
	char dir[400], conf[420], sock[420], addr[460], empty[420];
	int op[2], rp[2], st = 0;
	pid_t dpid, spid, cpid;
	FILE *f;
	if (argc < 3) { fputs("usage: fakes LOGDIR COMMAND [ARGS...]\n", stderr); return 2; }
	snprintf(dir, sizeof dir, "%s", argv[1]);
	mkdir(dir, 0700);
	snprintf(logpath, sizeof logpath, "%s/calls.log", dir);
	snprintf(sock, sizeof sock, "%s/bus", dir); snprintf(conf, sizeof conf, "%s/bus.conf", dir); snprintf(empty, sizeof empty, "%s/empty", dir);
	mkdir(empty, 0700);
	if (!(f = fopen(conf, "w"))) return 1;
	fprintf(f, "<busconfig><type>session</type><listen>unix:path=%s</listen><auth>EXTERNAL</auth>"
	           "<policy context=\"default\"><allow send_destination=\"*\" eavesdrop=\"true\"/><allow eavesdrop=\"true\"/><allow own=\"*\"/></policy></busconfig>", sock);
	fclose(f);
	if (pipe(op) || pipe(rp)) return 1;
	if (!(dpid = fork())) { dup2(op[1], 1); execlp("dbus-daemon", "dbus-daemon", "--config-file", conf, "--nofork", "--print-address", (char *)NULL); _exit(127); }
	close(op[1]);
	{ char l[300] = ""; ssize_t n = read(op[0], l, sizeof l - 1); (void)n; if (!strstr(l, "unix:")) { fputs("fakes: dbus-daemon did not start\n", stderr); kill(dpid, SIGTERM); return 1; } }
	snprintf(addr, sizeof addr, "unix:path=%s", sock);
	setenv("DBUS_SYSTEM_BUS_ADDRESS", addr, 1);
	setenv("NMB_WPA_DIR", empty, 1);
	if (!(spid = fork())) { service(rp[1]); _exit(0); }
	{ char c; if (read(rp[0], &c, 1) != 1) { fputs("fakes: service did not start\n", stderr); kill(dpid, SIGTERM); return 1; } }
	if (!(cpid = fork())) { execvp(argv[2], argv + 2); perror(argv[2]); _exit(127); }
	waitpid(cpid, &st, 0);
	kill(spid, SIGTERM); kill(dpid, SIGTERM);
	waitpid(spid, NULL, 0); waitpid(dpid, NULL, 0);
	return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}
