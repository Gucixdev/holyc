/* fakes - a stateful fake iwd + bluez on a private dbus-daemon, for driving the TUI (or the CLI) without touching anything real.
 *
 *   cc -std=gnu11 -Wall -Wextra -O2 -Isrc/mods -o /tmp/nm_fakes fakes.c
 *   /tmp/nm_fakes LOGDIR COMMAND [ARGS...]     runs COMMAND with DBUS_SYSTEM_BUS_ADDRESS pointing at the fake; every call the fake receives is
 *                                              appended to LOGDIR/calls.log; exits with COMMAND's status
 *
 * Wi-Fi (iwd): Home (saved, autoconnect, in range, 2 access points), Guest cafe (open), Neighbour (psk "neighbour88"), Office 5G (psk, saved, weak),
 * Zażółć (psk), Eduroam (802.1X: student / eduroampw), Legacy WEP (wep), a name with a control character, a long CJK name, and two HIDDEN access points
 * (GetHiddenAccessPoints); ConnectHiddenNetwork("Hidden Lab") needs "hiddenpass". A passphrase the agent returns that does not match -> net.connman.iwd.Failed.
 * FAKES_WPA=1 adds a fake wpa_supplicant control socket (NETMAN_WPA_DIR=LOGDIR/wpa) with SCAN_RESULTS (hidden rows, \x-escaped names, 2 BSSIDs of Home),
 * STATUS, SIGNAL_POLL, LIST_NETWORKS and connect events (WRONG_KEY for a bad psk) instead of iwd being used.
 * Bluetooth: Fake Pad (paired), Headphones (paired, connected, battery 80 %, A2DP/AVRCP/HFP), Phone (unpaired). Connect / Disconnect / Forget / AutoConnect / Trusted /
 * Powered change the state the next listing shows. NETMAN_WPA_DIR points at an empty directory so the iwd path is taken.
 * What this cannot show: how real iwd / bluez behave.
 */
#define _GNU_SOURCE
#define NETMAN_DBUS_IMPLEMENTATION
#include "dbus.h"
#include <sys/wait.h>
#include <sys/un.h>

static struct { const char *name, *type; int dbm, known, autoc; const char *secret; } nets[] = {
	{ "Home", "psk", -52, 1, 1, "homepass1" }, { "Guest cafe", "open", -71, 0, 0, NULL }, { "Neighbour", "psk", -48, 0, 0, "neighbour88" },
	{ "Office 5G", "psk", -82, 1, 0, "officepass" }, { "Zażółć", "psk", -66, 0, 0, "zazolc123" },
	{ "Eduroam", "8021x", -60, 0, 0, "eduroampw" }, { "Legacy WEP", "wep", -75, 0, 0, "wepkey1234" },
	{ "Bad\x01Name", "open", -80, 0, 0, NULL }, { "日本語のとても長いネットワーク名ですよ", "psk", -77, 0, 0, "longname1" },
};
#define NNET (int)(sizeof nets / sizeof *nets)
static int connected = -1;
static const char *agent_sender, *agent_path;
static char agent_sender_buf[64], agent_path_buf[96];
static struct { const char *addr, *alias; int paired, conn, trusted, rssi; unsigned cls; const char *icon; int battery; const char *uuids; } devs[] = {
	{ "AA:BB:CC:DD:EE:FF", "Fake Pad", 1, 0, 0, -60, 0x000508, "input-gaming", -1, "00001124-0000-1000-8000-00805f9b34fb,00001200-0000-1000-8000-00805f9b34fb" },
	{ "11:22:33:44:55:66", "Headphones", 1, 1, 1, -45, 0x240404, "audio-headphones", 80, "0000110b-0000-1000-8000-00805f9b34fb,0000110e-0000-1000-8000-00805f9b34fb,0000111e-0000-1000-8000-00805f9b34fb" },
	{ "00:11:22:33:44:55", "Phone", 0, 0, 0, 0, 0x5a020c, "phone", -1, "" },
};
#define NDEV (int)(sizeof devs / sizeof *devs)
static int powered = 1;
static char logpath[520];

static void flog(const char *s) { FILE *f = fopen(logpath, "a"); if (f) { fprintf(f, "%s\n", s); fclose(f); } }
static void arr_begin(netman_dbus_Buf *b, size_t *lp, size_t *st) { netman_dbus_align(b, 4); *lp = b->n; netman_dbus_put_u32(b, 0); netman_dbus_align(b, 8); *st = b->n; }
static void arr4_begin(netman_dbus_Buf *b, size_t *lp, size_t *st) { netman_dbus_align(b, 4); *lp = b->n; netman_dbus_put_u32(b, 0); *st = b->n; }   /* array of 4-aligned elements (strings): no padding after the length */
static void arr_end(netman_dbus_Buf *b, size_t lp, size_t st) { uint32_t l = b->n - st; memcpy(b->b + lp, &l, 4); }
static void prop_s(netman_dbus_Buf *b, const char *k, const char *v) { netman_dbus_align(b, 8); netman_dbus_put_str(b, k); netman_dbus_put_sig(b, "s"); netman_dbus_put_str(b, v); }
static void prop_o(netman_dbus_Buf *b, const char *k, const char *v) { netman_dbus_align(b, 8); netman_dbus_put_str(b, k); netman_dbus_put_sig(b, "o"); netman_dbus_put_str(b, v); }
static void prop_b(netman_dbus_Buf *b, const char *k, int v) { netman_dbus_align(b, 8); netman_dbus_put_str(b, k); netman_dbus_put_sig(b, "b"); netman_dbus_put_u32(b, v); }
static void prop_n(netman_dbus_Buf *b, const char *k, int v) { int16_t x = v; netman_dbus_align(b, 8); netman_dbus_put_str(b, k); netman_dbus_put_sig(b, "n"); netman_dbus_align(b, 2); netman_dbus_put(b, &x, 2); }

static void prop_u(netman_dbus_Buf *b, const char *k, unsigned v) { netman_dbus_align(b, 8); netman_dbus_put_str(b, k); netman_dbus_put_sig(b, "u"); netman_dbus_put_u32(b, v); }
static void prop_y(netman_dbus_Buf *b, const char *k, int v) { unsigned char x = (unsigned char)v; netman_dbus_align(b, 8); netman_dbus_put_str(b, k); netman_dbus_put_sig(b, "y"); netman_dbus_put(b, &x, 1); }
static void prop_as(netman_dbus_Buf *b, const char *k, const char *csv)
{
	char l[600], *p, *sv = NULL; size_t lp, st;
	snprintf(l, sizeof l, "%s", csv);
	netman_dbus_align(b, 8); netman_dbus_put_str(b, k); netman_dbus_put_sig(b, "as");
	arr4_begin(b, &lp, &st);
	for (p = strtok_r(l, ",", &sv); p; p = strtok_r(NULL, ",", &sv)) netman_dbus_put_str(b, p);
	arr_end(b, lp, st);
}
static void prop_ao(netman_dbus_Buf *b, const char *k, int n, const char *prefix)
{
	size_t lp, st; char p[96];
	netman_dbus_align(b, 8); netman_dbus_put_str(b, k); netman_dbus_put_sig(b, "ao");
	arr4_begin(b, &lp, &st);
	for (int i = 0; i < n; i++) { snprintf(p, sizeof p, "%s%d", prefix, i); netman_dbus_put_str(b, p); }
	arr_end(b, lp, st);
}
static void begin_obj(netman_dbus_Buf *b, const char *path, size_t *lp, size_t *st) { netman_dbus_align(b, 8); netman_dbus_put_str(b, path); arr_begin(b, lp, st); }
static void begin_if(netman_dbus_Buf *b, const char *name, size_t *lp, size_t *st) { netman_dbus_align(b, 8); netman_dbus_put_str(b, name); arr_begin(b, lp, st); }

static void netpath(char *o, size_t n, int i) { snprintf(o, n, "/net/connman/iwd/0/3/n%d", i); }
static void knownpath(char *o, size_t n, int i) { snprintf(o, n, "/net/connman/iwd/k%d", i); }

static void managed(netman_dbus_Buf *out, int bluez)
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
			prop_u(out, "Class", devs[i].cls); prop_s(out, "Icon", devs[i].icon); prop_as(out, "UUIDs", devs[i].uuids);
			arr_end(out, l3, s3);
			if (devs[i].battery >= 0) { begin_if(out, "org.bluez.Battery1", &l3, &s3); prop_y(out, "Percentage", devs[i].battery); arr_end(out, l3, s3); }
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
			prop_b(out, "Connected", connected == i);
			if (i == 0) prop_ao(out, "ExtendedServiceSet", 2, "/net/connman/iwd/0/3/bss");
			arr_end(out, l3, s3);
			arr_end(out, lp2, st2);
			if (nets[i].known) {
				knownpath(kp, sizeof kp, i);
				begin_obj(out, kp, &lp2, &st2);
				begin_if(out, "net.connman.iwd.KnownNetwork", &l3, &s3);
				prop_s(out, "Name", nets[i].name); prop_s(out, "Type", nets[i].type); prop_b(out, "AutoConnect", nets[i].autoc); prop_s(out, "LastConnectedTime", i == 0 ? "2026-11-07T21:14:03Z" : "2026-10-02T08:30:00Z");
				arr_end(out, l3, s3);
				arr_end(out, lp2, st2);
			}
		}
	}
	if (!bluez) for (int i = 0; i < 2; i++) {   /* the two access points of Home */
		char p[80]; size_t l3, s3, lp2, st2;
		snprintf(p, sizeof p, "/net/connman/iwd/0/3/bss%d", i);
		begin_obj(out, p, &lp2, &st2);
		begin_if(out, "net.connman.iwd.BasicServiceSet", &l3, &s3); prop_s(out, "Address", i ? "aa:bb:cc:00:00:02" : "aa:bb:cc:00:00:01"); arr_end(out, l3, s3);
		arr_end(out, lp2, st2);
	}
	arr_end(out, lp, st);
}

static int idx_of(const char *path, const char *prefix)
{
	size_t l = strlen(prefix);
	return !strncmp(path, prefix, l) ? atoi(path + l) : -1;
}


static void err_reply(int fd, const netman_dbus_Msg *req, const char *name, const char *text)
{
	netman_dbus_Buf out = {0}, b = {0};
	netman_dbus_put_str(&b, text);
	netman_dbus_build(&out, 3, netman_dbus_serial_no++, req->sender, NULL, NULL, NULL, req->serial, name, "s", &b);
	netman_dbus_send_msg(fd, &out);
	free(out.b); free(b.b);
}

/* what iwd does on Connect to an unknown secured network: ask the registered agent; 1 = credentials match */
static int ask_agent(int fd, const char *netp, const char *secret, int eap)
{
	netman_dbus_Msg r;
	netman_dbus_Buf b = {0};
	char line[300], got[100] = "", user[100] = "";
	int rc, ok = 0;
	if (!agent_path) { flog("agent: none registered"); return 0; }
	netman_dbus_put_str(&b, netp);
	rc = netman_dbus_call(fd, &r, agent_sender, agent_path, "net.connman.iwd.Agent", eap ? "RequestUserNameAndPassword" : "RequestPassphrase", "o", &b);
	free(b.b);
	if (rc == 0 && r.type == 2) {
		netman_dbus_R rr = { r.body, 0, r.blen, 0 };
		if (eap) { snprintf(user, sizeof user, "%s", netman_dbus_r_str(&rr)); }
		snprintf(got, sizeof got, "%s", netman_dbus_r_str(&rr));
		ok = !strcmp(got, secret) && (!eap || !strcmp(user, "student"));
	}
	snprintf(line, sizeof line, "agent %s -> %s%s%s (%s)", eap ? "RequestUserNameAndPassword" : "RequestPassphrase", eap ? user : "", eap ? "/" : "", got, ok ? "accepted" : "rejected");
	flog(line);
	return ok;
}

static void service(int ready)
{
	int fd = netman_dbus_connect_bus(getenv("DBUS_SYSTEM_BUS_ADDRESS"));
	netman_dbus_Msg m;
	if (fd < 0 || netman_dbus_bus_hello(fd)) _exit(2);
	netman_dbus_call_timeout = 3000;
	{
		const char *names[] = { "org.bluez", "net.connman.iwd" };
		for (int i = 0; i < 2; i++) {
			netman_dbus_Buf b = {0};
			netman_dbus_put_str(&b, names[i]); netman_dbus_put_u32(&b, 0);
			if (netman_dbus_call(fd, &m, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName", "su", &b) || m.type != 2) _exit(3);
			free(b.b);
		}
	}
	if (write(ready, "x", 1) != 1) _exit(1);
	for (;;) {
		netman_dbus_Buf out = {0};
		char line[300];
		int i;
		if (netman_dbus_read_msg(fd, &m, 200)) { if (getppid() == 1) _exit(0); continue; }
		if (m.type != 1) continue;
		if (strcmp(m.iface, "org.freedesktop.DBus.Introspectable")) { snprintf(line, sizeof line, "%s %s.%s", m.path, m.iface, m.member); flog(line); }
		if (!strcmp(m.member, "GetManagedObjects")) {
			managed(&out, !strcmp(m.dest, "org.bluez"));
			netman_dbus_reply_to_call(fd, &m, "a{oa{sa{sv}}}", &out);
		} else if (!strcmp(m.member, "GetOrderedNetworks")) {
			size_t lp, st;
			char p[80];
			arr_begin(&out, &lp, &st);
			for (i = 0; i < NNET; i++) {   /* a(on): path + signal in 1/100 dBm, strongest first would be nicer but the client sorts nothing */
				int16_t s = nets[i].dbm * 100;
				netpath(p, sizeof p, i);
				netman_dbus_align(&out, 8); netman_dbus_put_str(&out, p); netman_dbus_align(&out, 2); netman_dbus_put(&out, &s, 2);
			}
			arr_end(&out, lp, st);
			netman_dbus_reply_to_call(fd, &m, "a(on)", &out);
		} else if (!strcmp(m.member, "RegisterAgent") && !strcmp(m.iface, "net.connman.iwd.AgentManager")) {
			netman_dbus_R r = { m.body, 0, m.blen, 0 };
			snprintf(agent_path_buf, sizeof agent_path_buf, "%s", netman_dbus_r_str(&r));
			snprintf(agent_sender_buf, sizeof agent_sender_buf, "%s", m.sender);
			agent_path = agent_path_buf; agent_sender = agent_sender_buf;
			netman_dbus_reply_to_call(fd, &m, NULL, NULL);
		} else if (!strcmp(m.member, "GetHiddenAccessPoints")) {
			static const struct { const char *a; int dbm; const char *t; } hid[] = { { "11:22:33:00:00:01", -70, "psk" }, { "11:22:33:00:00:02", -81, "open" } };
			size_t lp, st;
			arr_begin(&out, &lp, &st);
			for (i = 0; i < 2; i++) { int16_t sg = hid[i].dbm * 100; netman_dbus_align(&out, 8); netman_dbus_put_str(&out, hid[i].a); netman_dbus_align(&out, 2); netman_dbus_put(&out, &sg, 2); netman_dbus_put_str(&out, hid[i].t); }
			arr_end(&out, lp, st);
			netman_dbus_reply_to_call(fd, &m, "a(sns)", &out);
		} else if (!strcmp(m.member, "GetDiagnostics")) {
			if (connected < 0) err_reply(fd, &m, "net.connman.iwd.NotConnected", "Not connected");
			else {
				size_t lp, st;
				arr_begin(&out, &lp, &st);
				prop_s(&out, "ConnectedBss", "aa:bb:cc:00:00:01"); prop_u(&out, "Frequency", 5180); prop_n(&out, "RSSI", nets[connected].dbm); prop_n(&out, "AverageRSSI", nets[connected].dbm - 1);
				prop_u(&out, "RxBitrate", 8660); prop_u(&out, "TxBitrate", 7800); prop_s(&out, "RxMode", "VHT"); prop_s(&out, "Security", "WPA2-Personal"); prop_u(&out, "ExpectedThroughput", 52000);
				arr_end(&out, lp, st);
				netman_dbus_reply_to_call(fd, &m, "a{sv}", &out);
			}
		} else if (!strcmp(m.member, "ConnectHiddenNetwork")) {
			netman_dbus_R r = { m.body, 0, m.blen, 0 };
			netman_dbus_Msg req = m;
			char nm[100];
			snprintf(nm, sizeof nm, "%s", netman_dbus_r_str(&r));
			snprintf(line, sizeof line, "   hidden=%s", nm);
			flog(line);
			if (!strcmp(nm, "Hidden Lab") && ask_agent(fd, "/net/connman/iwd/0/3/hidden", "hiddenpass", 0)) netman_dbus_reply_to_call(fd, &req, NULL, NULL);
			else err_reply(fd, &req, "net.connman.iwd.Failed", "Operation failed");
		} else if (!strcmp(m.member, "Connect") && (i = idx_of(m.path, "/net/connman/iwd/0/3/n")) >= 0 && i < NNET) {
			netman_dbus_Msg req = m;
			int ok = 1;
			if (!nets[i].known && nets[i].secret) ok = ask_agent(fd, req.path, nets[i].secret, !strcmp(nets[i].type, "8021x"));
			if (!ok) err_reply(fd, &req, "net.connman.iwd.Failed", "Operation failed");
			else { connected = i; nets[i].known = 1; netman_dbus_reply_to_call(fd, &req, NULL, NULL); }
		} else if (!strcmp(m.member, "Disconnect") && !strcmp(m.iface, "net.connman.iwd.Station")) {
			connected = -1;
			netman_dbus_reply_to_call(fd, &m, NULL, NULL);
		} else if (!strcmp(m.member, "Forget") && (i = idx_of(m.path, "/net/connman/iwd/k")) >= 0 && i < NNET) {
			nets[i].known = 0; nets[i].autoc = 0;
			if (connected == i) connected = -1;
			netman_dbus_reply_to_call(fd, &m, NULL, NULL);
		} else if (!strcmp(m.member, "Connect") && (i = idx_of(m.path, "/org/bluez/hci0/dev_")) >= 0 && i < NDEV) {
			devs[i].conn = 1;
			netman_dbus_reply_to_call(fd, &m, NULL, NULL);
		} else if (!strcmp(m.member, "Disconnect") && (i = idx_of(m.path, "/org/bluez/hci0/dev_")) >= 0 && i < NDEV) {
			devs[i].conn = 0;
			netman_dbus_reply_to_call(fd, &m, NULL, NULL);
		} else if (!strcmp(m.member, "Set") && !strcmp(m.iface, "org.freedesktop.DBus.Properties")) {
			netman_dbus_R r = { m.body, 0, m.blen, 0 };
			const char *key;
			char v[40];
			netman_dbus_r_str(&r);
			key = netman_dbus_r_str(&r);
			netman_dbus_variant_str(&r, v, sizeof v);
			snprintf(line, sizeof line, "   set %s=%s", key, v);
			flog(line);
			if (!strcmp(key, "AutoConnect") && (i = idx_of(m.path, "/net/connman/iwd/k")) >= 0 && i < NNET) nets[i].autoc = !strcmp(v, "true");
			else if (!strcmp(key, "Trusted") && (i = idx_of(m.path, "/org/bluez/hci0/dev_")) >= 0 && i < NDEV) devs[i].trusted = !strcmp(v, "true");
			else if (!strcmp(key, "Powered")) powered = !strcmp(v, "true");
			netman_dbus_reply_to_call(fd, &m, NULL, NULL);
		} else netman_dbus_reply_to_call(fd, &m, NULL, NULL);
		free(out.b);
	}
}


/* ---- fake wpa_supplicant control socket (FAKES_WPA=1) ---- */
static void wpa_server(const char *dir, int ready)
{
	struct sockaddr_un a = { .sun_family = AF_UNIX }, from, attached;
	socklen_t al = 0;
	int fd = socket(AF_UNIX, SOCK_DGRAM, 0), cur_open = 0, nid = 0;
	char ssid[8][64] = {{0}}, psk[8][64] = {{0}}, curssid[64] = "Home";
	int connected = 1;
	snprintf(a.sun_path, sizeof a.sun_path, "%s/wlan0", dir);
	unlink(a.sun_path);
	if (bind(fd, (struct sockaddr *)&a, sizeof a)) _exit(4);
	if (write(ready, "x", 1) != 1) _exit(1);
	(void)cur_open;
	for (;;) {
		char cmd[600], rep[8192];
		socklen_t fl = sizeof from;
		ssize_t n = recvfrom(fd, cmd, sizeof cmd - 1, 0, (struct sockaddr *)&from, &fl);
		const char *ev = NULL;
		char evbuf[300], line[700];
		if (n <= 0) { if (getppid() == 1) _exit(0); continue; }
		cmd[n] = 0;
		snprintf(line, sizeof line, "wpa %s", cmd); flog(line);
		snprintf(rep, sizeof rep, "OK");
		if (!strcmp(cmd, "ATTACH")) { attached = from; al = fl; }
		else if (!strcmp(cmd, "SCAN_RESULTS"))
			snprintf(rep, sizeof rep, "bssid / frequency / signal level / flags / ssid\n"
			  "aa:bb:cc:00:00:01\t5180\t-50\t[WPA2-PSK-CCMP][ESS]\tHome\n"
			  "aa:bb:cc:00:00:02\t2437\t-60\t[WPA2-PSK-CCMP][ESS]\tHome\n"
			  "cc:dd:ee:00:00:01\t2462\t-71\t[ESS]\tGuest cafe\n"
			  "dd:ee:ff:00:00:01\t5200\t-48\t[WPA2-PSK-CCMP+SAE-CCMP][ESS]\tNeighbour\n"
			  "11:22:33:00:00:01\t2412\t-70\t[WPA2-PSK-CCMP][ESS]\t\n"
			  "11:22:33:00:00:02\t2417\t-81\t[WPA2-PSK-CCMP][ESS]\t\\x00\\x00\\x00\\x00\n"
			  "ee:ff:00:00:00:01\t2447\t-66\t[WPA2-PSK-CCMP][ESS]\tZa\\xc5\\xbc\\xc3\\xb3\\xc5\\x82\\xc4\\x87\n"
			  "ee:ff:00:00:00:02\t5745\t-62\t[WPA2-EAP-CCMP][ESS]\tEduroam\n"
			  "ee:ff:00:00:00:03\t2452\t-75\t[WEP][ESS]\tLegacy WEP\n"
			  "ee:ff:00:00:00:04\t2457\t-80\t[ESS]\tBad\\x01Name\n"
			  "ee:ff:00:00:00:05\t6055\t-77\t[SAE-CCMP][ESS]\t\\xe6\\x97\\xa5\\xe6\\x9c\\xac\\xe8\\xaa\\x9e\\xe3\\x81\\xae\\xe9\\x95\\xb7\\xe3\\x81\\x84\\xe5\\x90\\x8d\\xe5\\x89\\x8d\n");
		else if (!strcmp(cmd, "STATUS")) {
			if (connected) snprintf(rep, sizeof rep, "bssid=aa:bb:cc:00:00:01\nfreq=5180\nssid=%s\nid=0\nmode=station\npairwise_cipher=CCMP\ngroup_cipher=CCMP\nkey_mgmt=WPA2-PSK\nwpa_state=COMPLETED\nip_address=192.168.1.23\n", curssid);
			else snprintf(rep, sizeof rep, "wpa_state=DISCONNECTED\n");
		} else if (!strcmp(cmd, "SIGNAL_POLL")) snprintf(rep, sizeof rep, "RSSI=-52\nLINKSPEED=433\nNOISE=-95\nFREQUENCY=5180\nWIDTH=80 MHz (VHT)\nAVG_RSSI=-53\n");
		else if (!strcmp(cmd, "LIST_NETWORKS")) snprintf(rep, sizeof rep, "network id / ssid / bssid / flags\n0\tHome\tany\t[CURRENT]\n1\tOffice 5G\tany\t[DISABLED]\n");
		else if (!strcmp(cmd, "ADD_NETWORK")) { snprintf(rep, sizeof rep, "%d", 2 + (nid++ % 5)); }
		else if (!strncmp(cmd, "SET_NETWORK ", 12)) {
			int id = atoi(cmd + 12) - 2; char *q = strchr(cmd + 12, ' ');
			if (id >= 0 && id < 8 && q) {
				q++;
				if (!strncmp(q, "ssid \"", 6)) { snprintf(ssid[id], sizeof ssid[id], "%s", q + 6); ssid[id][strcspn(ssid[id], "\"")] = 0; }
				else if (!strncmp(q, "psk \"", 5)) { snprintf(psk[id], sizeof psk[id], "%s", q + 5); psk[id][strcspn(psk[id], "\"")] = 0; }
				else if (!strncmp(q, "password \"", 10)) { snprintf(psk[id], sizeof psk[id], "%s", q + 10); psk[id][strcspn(psk[id], "\"")] = 0; }
			}
		} else if (!strncmp(cmd, "SELECT_NETWORK ", 15)) {
			int id = atoi(cmd + 15) - 2, good = 0;
			if (id >= 0 && id < 8) {
				if (!psk[id][0]) good = 1;   /* open */
				for (int i = 0; i < NNET; i++) if (!strcmp(nets[i].name, ssid[id]) && nets[i].secret && !strcmp(nets[i].secret, psk[id])) good = 1;
				if (!strcmp(ssid[id], "Eduroam") && strcmp(psk[id], "eduroampw")) good = 0;
			}
			if (good) { snprintf(evbuf, sizeof evbuf, "<3>CTRL-EVENT-CONNECTED - Connection to aa:bb:cc:00:00:01 completed [id=%d id_str=]", id + 2); snprintf(curssid, sizeof curssid, "%s", ssid[id]); connected = 1; }
			else snprintf(evbuf, sizeof evbuf, "<3>CTRL-EVENT-SSID-TEMP-DISABLED id=%d ssid=\"%s\" auth_failures=1 duration=10 reason=WRONG_KEY", id + 2, ssid[id]);
			ev = evbuf;
		} else if (!strcmp(cmd, "DISCONNECT")) connected = 0;
		sendto(fd, rep, strlen(rep), 0, (struct sockaddr *)&from, fl);
		if (ev && al) sendto(fd, ev, strlen(ev), 0, (struct sockaddr *)&attached, al);
	}
}

int main(int argc, char **argv)
{
	char dir[400], conf[420], sock[420], addr[460], empty[420];
	int op[2], rp[2], st = 0;
	pid_t dpid, spid, cpid, wpid = 0;
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
	setenv("NETMAN_WPA_DIR", empty, 1);
	if (!(spid = fork())) { service(rp[1]); _exit(0); }
	{ char c; if (read(rp[0], &c, 1) != 1) { fputs("fakes: service did not start\n", stderr); kill(dpid, SIGTERM); return 1; } }
	if (getenv("FAKES_WPA")) {   /* second fake: wpa_supplicant owns the interface */
		char wdir[440]; int wp[2];
		snprintf(wdir, sizeof wdir, "%s/wpa", dir);
		mkdir(wdir, 0700);
		if (pipe(wp)) return 1;
		if (!(wpid = fork())) { wpa_server(wdir, wp[1]); _exit(0); }
		{ char c; if (read(wp[0], &c, 1) != 1) { fputs("fakes: wpa fake did not start\n", stderr); return 1; } }
		setenv("NETMAN_WPA_DIR", wdir, 1); setenv("NETMAN_SCAN_SECS", "0", 1); setenv("NETMAN_CONNECT_SECS", "4", 1);
	}
	if (!(cpid = fork())) { execvp(argv[2], argv + 2); perror(argv[2]); _exit(127); }
	waitpid(cpid, &st, 0);
	kill(spid, SIGTERM); kill(dpid, SIGTERM); if (wpid) kill(wpid, SIGTERM);
	waitpid(spid, NULL, 0); waitpid(dpid, NULL, 0);
	return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}
