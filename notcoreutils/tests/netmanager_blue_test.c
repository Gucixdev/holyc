/* netmanager_blue tests - no root, no hardware, nothing real is touched.
 *
 *   cc -Wall -Wextra -O2 -o /tmp/nmb_test tests/netmanager_blue_test.c && /tmp/nmb_test
 *
 * 1. unit: message encode/decode round trip, WPS event classification, agent replies over a socketpair
 * 2. fake wpa_supplicant: a unix datagram socket in a temp dir (NMB_WPA_DIR) answering ATTACH / WPS_* / *_NETWORK and sending events
 * 3. fake iwd + bluez: one process owning both names on a private dbus-daemon (DBUS_SYSTEM_BUS_ADDRESS), logging every call it
 *    receives and driving the client's Agent1 methods during Pair / `bt agent`
 *
 * What this cannot show: that real wpa_supplicant / iwd / bluez / a router / a phone behave as the fakes do.
 */
#define _GNU_SOURCE
#define NMB_IMPLEMENTATION
#include "../netmanager_blue.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d  %s\n      ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char *ob, *eb;
static size_t os_, es_;
static void cap_begin(void) { ob = eb = NULL; nmb_out = open_memstream(&ob, &os_); nmb_err = open_memstream(&eb, &es_); }
static void cap_end(void) { fclose(nmb_out); fclose(nmb_err); nmb_out = nmb_err = NULL; }

static char tmp[64];
static char *slurp_file(const char *name)
{
	static char buf[65536];
	char p[300];
	FILE *f;
	size_t n = 0;
	snprintf(p, sizeof p, "%s/%s", tmp, name);
	buf[0] = 0;
	if ((f = fopen(p, "r"))) { n = fread(buf, 1, sizeof buf - 1, f); fclose(f); }
	buf[n] = 0;
	return buf;
}

/* ---- 1. unit ------------------------------------------------------------------------------------------------------ */

static void unit(void)
{
	nmb__Buf b = {0}, body = {0};
	nmb__Msg m;
	int sv[2];
	socketpair(AF_UNIX, SOCK_STREAM, 0, sv);

	nmb__put_str(&body, "/org/bluez/hci0/dev_AA");
	nmb__build(&b, 1, 5, "org.bluez", "/nmb/btagent", "org.bluez.Agent1", "RequestPinCode", 0, NULL, "o", &body);
	nmb__send_msg(sv[0], &b);
	CHECK(nmb__read_msg(sv[1], &m, 1000) == 0, "read back");
	CHECK(m.type == 1 && m.serial == 5 && !strcmp(m.dest, "org.bluez") && !strcmp(m.path, "/nmb/btagent") && !strcmp(m.iface, "org.bluez.Agent1") &&
	      !strcmp(m.member, "RequestPinCode") && !strcmp(m.sig, "o") && m.blen == body.n, "header fields: %s %s %s", m.dest, m.member, m.sig);

	/* agent: PIN given (non-interactive) -> reply "s" with it */
	nmb_bt_set_pin("4242");
	cap_begin();
	nmb__bt_serve(sv[1], &m);
	cap_end();
	CHECK(nmb__read_msg(sv[0], &m, 1000) == 0 && m.type == 2 && m.reply_serial == 5 && !strcmp(m.sig, "s"), "pin reply type=%d sig=%s", m.type, m.sig);
	{ nmb__R r = { m.body, 0, m.blen, 0 }; CHECK(!strcmp(nmb__r_str(&r), "4242"), "pin value"); }

	/* passkey: numeric reply "u" */
	nmb_bt_set_pin("123456");
	nmb__build(&b, 1, 6, "x", "/nmb/btagent", "org.bluez.Agent1", "RequestPasskey", 0, NULL, "o", &body);
	nmb__send_msg(sv[0], &b);
	nmb__read_msg(sv[1], &m, 1000);
	cap_begin(); nmb__bt_serve(sv[1], &m); cap_end();
	CHECK(nmb__read_msg(sv[0], &m, 1000) == 0 && m.type == 2 && !strcmp(m.sig, "u"), "passkey reply");
	{ nmb__R r = { m.body, 0, m.blen, 0 }; CHECK(nmb__r_u32(&r) == 123456, "passkey value"); }

	/* a passkey that is not a number is rejected, not guessed */
	nmb_bt_set_pin("12ab");
	nmb__build(&b, 1, 7, "x", "/nmb/btagent", "org.bluez.Agent1", "RequestPasskey", 0, NULL, "o", &body);
	nmb__send_msg(sv[0], &b);
	nmb__read_msg(sv[1], &m, 1000);
	cap_begin(); nmb__bt_serve(sv[1], &m); cap_end();
	CHECK(nmb__read_msg(sv[0], &m, 1000) == 0 && m.type == 3 && !strcmp(m.err, "org.bluez.Error.Rejected"), "bad passkey -> Rejected, got %s", m.err);

	/* confirmation, interactive: "n" declines, "y" accepts */
	nmb_bt_set_pin(NULL);
	for (int round = 0; round < 2; round++) {
		nmb__Buf cb = {0};
		nmb__put_str(&cb, "/org/bluez/hci0/dev_AA"); nmb__put_u32(&cb, 482913);
		nmb__build(&b, 1, 8, "x", "/nmb/btagent", "org.bluez.Agent1", "RequestConfirmation", 0, NULL, "ou", &cb);
		nmb__send_msg(sv[0], &b);
		nmb__read_msg(sv[1], &m, 1000);
		nmb_in = fmemopen(round ? "y\n" : "n\n", 2, "r");
		cap_begin(); nmb__bt_serve(sv[1], &m); cap_end();
		fclose(nmb_in); nmb_in = NULL;
		CHECK(strstr(ob, "482913"), "passkey shown: %s", ob);
		CHECK(nmb__read_msg(sv[0], &m, 1000) == 0 && (round ? m.type == 2 : m.type == 3), "confirm round %d type %d", round, m.type);
		free(cb.b);
	}

	/* unknown Agent1 method and foreign interface -> UnknownMethod error, never a hang */
	nmb__build(&b, 1, 9, "x", "/nmb/btagent", "org.bluez.Agent1", "Frobnicate", 0, NULL, NULL, NULL);
	nmb__send_msg(sv[0], &b);
	nmb__read_msg(sv[1], &m, 1000);
	nmb__bt_serve(sv[1], &m);
	CHECK(nmb__read_msg(sv[0], &m, 1000) == 0 && m.type == 3 && strstr(m.err, "UnknownMethod"), "unknown method");

	/* WPS event classification and the control-socket quoting guard */
	CHECK(nmb__wps_event("<3>WPS-SUCCESS") == 1 && nmb__wps_event("<3>CTRL-EVENT-CONNECTED - Connection to aa completed") == 1, "success events");
	CHECK(nmb__wps_event("<3>WPS-FAIL msg=8") == -1 && nmb__wps_event("WPS-TIMEOUT") == -1 && nmb__wps_event("<3>WPS-OVERLAP-DETECTED") == -1, "failure events");
	CHECK(nmb__wps_event("<3>WPS-PBC-ACTIVE") == 0 && nmb__wps_event("<3>CTRL-EVENT-SCAN-RESULTS") == 0, "progress events");
	CHECK(nmb__wpa_safe("my wifi") && !nmb__wpa_safe("a\"b") && !nmb__wpa_safe("a\\b") && !nmb__wpa_safe("a\nSET_NETWORK") && !nmb__wpa_safe(""), "quoting guard");

	/* bad PIN is refused before any socket is touched */
	cap_begin();
	CHECK(nmb_wifi_wps_pin("12") == 1 && nmb_wifi_wps_pin("1234567x") == 1 && nmb_wifi_wps_pin("123456789") == 1, "pin validation");
	cap_end();
	close(sv[0]); close(sv[1]);
	free(b.b); free(body.b);
}

/* ---- 2. fake wpa_supplicant --------------------------------------------------------------------------------------- */

static void wpa_log(const char *cmd) { FILE *f = fopen(strcat(strcpy((char[300]){0}, tmp), "/wpa.log"), "a"); if (f) { fprintf(f, "%s\n", cmd); fclose(f); } }

static void wpa_server(int ready)
{
	char path[300];
	struct sockaddr_un a = { .sun_family = AF_UNIX }, from;
	int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
	snprintf(path, sizeof path, "%s/wpa", tmp);
	mkdir(path, 0700);
	snprintf(a.sun_path, sizeof a.sun_path, "%s/wpa/wlan0", tmp);
	unlink(a.sun_path);
	bind(fd, (struct sockaddr *)&a, sizeof a);
	if (write(ready, "x", 1) != 1) _exit(1);
	for (;;) {
		char cmd[512];
		socklen_t fl = sizeof from;
		ssize_t n = recvfrom(fd, cmd, sizeof cmd - 1, 0, (struct sockaddr *)&from, &fl);
		const char *rep = "OK", *ev = NULL;
		if (n <= 0) continue;
		cmd[n] = 0;
		if (!strcmp(cmd, "QUIT")) _exit(0);
		wpa_log(cmd);
		if (!strcmp(cmd, "WPS_PBC")) ev = "<3>WPS-SUCCESS";
		else if (!strcmp(cmd, "WPS_PIN any")) { rep = "12345670"; ev = "<3>WPS-SUCCESS"; }
		else if (!strncmp(cmd, "WPS_PIN any ", 12)) ev = "<3>WPS-FAIL msg=8 config_error=18";
		else if (!strcmp(cmd, "ADD_NETWORK")) rep = "7";
		else if (!strcmp(cmd, "LIST_NETWORKS")) rep = "network id / ssid / bssid / flags\n3\tnmb-ap-test\tany\t[DISABLED]\n4\thome\tany\t[CURRENT]\n";
		else if (!strcmp(cmd, "GET_NETWORK 3 id_str")) rep = "\"nmb-ap\"";
		else if (!strncmp(cmd, "GET_NETWORK", 11)) rep = "FAIL";
		if (ev) { sendto(fd, "<3>WPS-PBC-ACTIVE", 17, 0, (struct sockaddr *)&from, fl); }   /* an event before the reply must be skipped */
		sendto(fd, rep, strlen(rep), 0, (struct sockaddr *)&from, fl);
		if (ev) sendto(fd, ev, strlen(ev), 0, (struct sockaddr *)&from, fl);
	}
}

static void wpa_tests(void)
{
	int p[2], st;
	pid_t pid;
	char dir[320];
	const char *l;
	pipe(p);
	if (!(pid = fork())) { wpa_server(p[1]); _exit(0); }
	{ char c; if (read(p[0], &c, 1) != 1) { puts("fake wpa did not start"); fails++; return; } }
	snprintf(dir, sizeof dir, "%s/wpa", tmp);
	setenv("NMB_WPA_DIR", dir, 1);
	setenv("NMB_WPS_SECS", "5", 1);
	CHECK(nmb_wifi_backend_is_wpa() == 1, "backend detection via NMB_WPA_DIR");

	cap_begin();
	CHECK(nmb_wifi_wps_pbc() == 0, "pbc rc, err=%s", eb);
	cap_end();
	CHECK(strstr(ob, "WPS done"), "pbc out: %s", ob);
	l = slurp_file("wpa.log");
	CHECK(strstr(l, "ATTACH\nWPS_PBC\n") && strstr(l, "DETACH") && strstr(l, "SAVE_CONFIG"), "pbc command sequence:\n%s", l);

	cap_begin();
	CHECK(nmb_wifi_wps_pin(NULL) == 0, "generated pin rc, err=%s", eb);
	cap_end();
	CHECK(strstr(ob, "PIN: 12345670"), "generated pin printed: %s", ob);

	cap_begin();
	CHECK(nmb_wifi_wps_pin("12345670") == 1, "WPS-FAIL must give rc 1");
	cap_end();
	CHECK(strstr(eb, "WPS failed"), "fail message: %s", eb);

	cap_begin(); CHECK(nmb_wifi_wps_cancel() == 0, "cancel"); cap_end();
	CHECK(strstr(slurp_file("wpa.log"), "WPS_CANCEL"), "cancel sent");

	unlink(strcat(strcpy(dir, tmp), "/wpa.log"));
	cap_begin();
	CHECK(nmb_wifi_hotspot_start("My AP", "12345678") == 0, "hotspot start, err=%s", eb);
	CHECK(nmb_wifi_hotspot_start("My AP", "short") == 1, "short passphrase refused");
	CHECK(nmb_wifi_hotspot_start("bad\"ssid", "12345678") == 1, "quote in ssid refused");
	cap_end();
	l = slurp_file("wpa.log");
	CHECK(strstr(l, "SET_NETWORK 7 ssid \"My AP\"") && strstr(l, "SET_NETWORK 7 mode 2") && strstr(l, "SET_NETWORK 7 key_mgmt WPA-PSK") && strstr(l, "SET_NETWORK 7 psk \"12345678\"") &&
	      strstr(l, "SET_NETWORK 7 id_str \"nmb-ap\"") && strstr(l, "SET_NETWORK 7 frequency 2437") && strstr(l, "SELECT_NETWORK 7"), "hotspot commands:\n%s", l);
	CHECK(!strstr(l, "bad"), "refused ssid must not reach the socket");

	unlink(strcat(strcpy(dir, tmp), "/wpa.log"));
	cap_begin(); CHECK(nmb_wifi_hotspot_stop() == 0, "hotspot stop, err=%s", eb); cap_end();
	l = slurp_file("wpa.log");
	CHECK(strstr(l, "REMOVE_NETWORK 3") && !strstr(l, "REMOVE_NETWORK 4"), "only the nmb-ap network is removed:\n%s", l);

	cap_begin();
	CHECK(nmb_wifi_known() == 0, "known");
	cap_end();
	CHECK(strstr(ob, "nmb-ap-test") && strstr(ob, "autoconnect=off") && strstr(ob, "home") && strstr(ob, "autoconnect=on"), "known list: %s", ob);
	unlink(strcat(strcpy(dir, tmp), "/wpa.log"));
	cap_begin();
	CHECK(nmb_wifi_autoconnect("home", 0) == 0, "autoconnect off");
	CHECK(nmb_wifi_autoconnect("nope", 1) == 1, "unknown ssid");
	cap_end();
	l = slurp_file("wpa.log");
	CHECK(strstr(l, "DISABLE_NETWORK 4") && !strstr(l, "DISABLE_NETWORK 3") && !strstr(l, "ENABLE_NETWORK"), "autoconnect commands:\n%s", l);

	{   /* stop the fake */
		struct sockaddr_un a = { .sun_family = AF_UNIX };
		int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
		snprintf(a.sun_path, sizeof a.sun_path, "%s/wpa/wlan0", tmp);
		sendto(fd, "QUIT", 4, 0, (struct sockaddr *)&a, sizeof a);
		close(fd);
	}
	waitpid(pid, &st, 0);
	unsetenv("NMB_WPA_DIR");
}

/* ---- 3. fake iwd + bluez on a private bus ------------------------------------------------------------------------- */

static void arr_begin(nmb__Buf *b, size_t *lp, size_t *start) { nmb__align(b, 4); *lp = b->n; nmb__put_u32(b, 0); nmb__align(b, 8); *start = b->n; }
static void arr_end(nmb__Buf *b, size_t lp, size_t start) { uint32_t l = b->n - start; memcpy(b->b + lp, &l, 4); }
static void prop_s(nmb__Buf *b, const char *k, const char *v) { nmb__align(b, 8); nmb__put_str(b, k); nmb__put_sig(b, "s"); nmb__put_str(b, v); }
static void prop_b(nmb__Buf *b, const char *k, int v) { nmb__align(b, 8); nmb__put_str(b, k); nmb__put_sig(b, "b"); nmb__put_u32(b, v); }

struct oiface { const char *name; void (*props)(nmb__Buf *); };
static void object(nmb__Buf *b, const char *path, const struct oiface *ifs, int n)
{
	size_t lp, st;
	nmb__align(b, 8);
	nmb__put_str(b, path);
	arr_begin(b, &lp, &st);
	for (int i = 0; i < n; i++) {
		size_t lp2, st2;
		nmb__align(b, 8);
		nmb__put_str(b, ifs[i].name);
		arr_begin(b, &lp2, &st2);
		if (ifs[i].props) ifs[i].props(b);
		arr_end(b, lp2, st2);
	}
	arr_end(b, lp, st);
}

static void p_adapter(nmb__Buf *b) { prop_b(b, "Powered", 1); }
static void p_device(nmb__Buf *b) { prop_s(b, "Address", "AA:BB:CC:DD:EE:FF"); prop_s(b, "Alias", "Fake Pad"); prop_b(b, "Paired", 0); prop_b(b, "Connected", 0); }
static void p_iwddev(nmb__Buf *b) { prop_s(b, "Name", "wlan0"); prop_s(b, "Mode", "station"); }
static void p_station(nmb__Buf *b) { prop_s(b, "State", "disconnected"); prop_b(b, "Scanning", 0); }
static void p_known(nmb__Buf *b) { prop_s(b, "Name", "Home"); prop_s(b, "Type", "psk"); prop_b(b, "AutoConnect", 1); }

static int ap_mode;
static const char *agent_path, *agent_sender;
static char agent_sender_buf[64], agent_path_buf[96];

static void flog(const char *line) { FILE *f = fopen(strcat(strcpy((char[300]){0}, tmp), "/dbus.log"), "a"); if (f) { fprintf(f, "%s\n", line); fclose(f); } }

/* call the client's Agent1 and log the outcome */
static int agent_call(int fd, const char *member, const char *sig, const nmb__Buf *body, const char *tag)
{
	nmb__Msg r;
	char line[300];
	int rc;
	if (!agent_path) return 1;
	rc = nmb__call(fd, &r, agent_sender, agent_path, "org.bluez.Agent1", member, sig, body);
	if (rc == 0 && r.type == 2 && r.sig[0] == 's') { nmb__R rr = { r.body, 0, r.blen, 0 }; snprintf(line, sizeof line, "agent %s -> %s", tag, nmb__r_str(&rr)); }
	else if (rc == 0 && r.type == 2 && r.sig[0] == 'u') { nmb__R rr = { r.body, 0, r.blen, 0 }; snprintf(line, sizeof line, "agent %s -> %u", tag, nmb__r_u32(&rr)); }
	else if (rc == 0 && r.type == 2) snprintf(line, sizeof line, "agent %s -> ok", tag);
	else snprintf(line, sizeof line, "agent %s -> error %s", tag, rc < 0 ? "none" : r.err);
	flog(line);
	return rc;
}

/* the sequence bluez runs during pairing: PIN, confirmation, a passkey display, a service authorisation */
static int drive_agent(int fd)
{
	nmb__Buf b = {0};
	int bad = 0;
	nmb__put_str(&b, "/org/bluez/hci0/dev_AA");
	bad |= agent_call(fd, "RequestPinCode", "o", &b, "RequestPinCode");
	bad |= agent_call(fd, "RequestPasskey", "o", &b, "RequestPasskey");
	free(b.b); b = (nmb__Buf){0};
	nmb__put_str(&b, "/org/bluez/hci0/dev_AA"); nmb__put_u32(&b, 654321);
	bad |= agent_call(fd, "RequestConfirmation", "ou", &b, "RequestConfirmation");
	free(b.b); b = (nmb__Buf){0};
	nmb__put_str(&b, "/org/bluez/hci0/dev_AA"); nmb__put_u32(&b, 111222); nmb__align(&b, 2); { uint16_t e = 3; nmb__put(&b, &e, 2); }
	bad |= agent_call(fd, "DisplayPasskey", "ouq", &b, "DisplayPasskey");
	free(b.b); b = (nmb__Buf){0};
	nmb__put_str(&b, "/org/bluez/hci0/dev_AA"); nmb__put_str(&b, "0000110b-0000-1000-8000-00805f9b34fb");
	bad |= agent_call(fd, "AuthorizeService", "os", &b, "AuthorizeService");
	free(b.b);
	return bad;
}

static void log_call(const nmb__Msg *m)
{
	char line[512];
	nmb__R r = { m->body, 0, m->blen, 0 };
	int o = snprintf(line, sizeof line, "%s %s.%s", m->path, m->iface, m->member);
	for (const char *s = m->sig; *s && !r.bad && o < 400; s++) {
		if (*s == 's' || *s == 'o') o += snprintf(line + o, sizeof line - o, " %s", nmb__r_str(&r));
		else if (*s == 'u') o += snprintf(line + o, sizeof line - o, " %u", nmb__r_u32(&r));
		else if (*s == 'v') { char v[100]; nmb__variant_str(&r, v, sizeof v); o += snprintf(line + o, sizeof line - o, " %s", v); }
		else break;
	}
	flog(line);
}

static void fake_service(int ready)
{
	int fd = nmb__connect_bus(getenv("DBUS_SYSTEM_BUS_ADDRESS"));
	nmb__Msg m;
	char flag[300];
	snprintf(flag, sizeof flag, "%s/drive", tmp);
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
	nmb__call_timeout = 15000;
	for (;;) {
		nmb__Buf out = {0};
		if (nmb__read_msg(fd, &m, 200)) { if (getppid() == 1) _exit(0); continue; }
		if (m.type != 1) continue;
		if (strcmp(m.iface, "org.freedesktop.DBus.Introspectable")) log_call(&m);
		if (!strcmp(m.member, "GetManagedObjects")) {
			size_t lp, st;
			arr_begin(&out, &lp, &st);
			if (!strcmp(m.dest, "org.bluez")) {
				object(&out, "/org/bluez/hci0", (struct oiface[]){ { "org.bluez.Adapter1", p_adapter } }, 1);
				object(&out, "/org/bluez/hci0/dev_AA", (struct oiface[]){ { "org.bluez.Device1", p_device } }, 1);
			} else {
				struct oiface dev[] = { { "net.connman.iwd.Device", p_iwddev }, { "net.connman.iwd.Station", p_station }, { "net.connman.iwd.SimpleConfiguration", NULL }, { "net.connman.iwd.AccessPoint", NULL } };
				object(&out, "/net/connman/iwd/0/3", dev, ap_mode ? 4 : 3);
				object(&out, "/net/connman/iwd/aaaa_psk", (struct oiface[]){ { "net.connman.iwd.KnownNetwork", p_known } }, 1);
			}
			arr_end(&out, lp, st);
			nmb__reply_to_call(fd, &m, "a{oa{sa{sv}}}", &out);
		} else if (!strcmp(m.member, "Set") && !strcmp(m.iface, "org.freedesktop.DBus.Properties")) {
			nmb__R r = { m.body, 0, m.blen, 0 };
			nmb__r_str(&r);
			if (!strcmp(nmb__r_str(&r), "Mode")) { char v[32]; nmb__variant_str(&r, v, sizeof v); ap_mode = !strcmp(v, "ap"); }
			nmb__reply_to_call(fd, &m, NULL, NULL);
		} else if (!strcmp(m.member, "RegisterAgent")) {
			nmb__R r = { m.body, 0, m.blen, 0 };
			snprintf(agent_path_buf, sizeof agent_path_buf, "%s", nmb__r_str(&r));
			snprintf(agent_sender_buf, sizeof agent_sender_buf, "%s", m.sender);
			agent_path = agent_path_buf; agent_sender = agent_sender_buf;
			nmb__reply_to_call(fd, &m, NULL, NULL);
		} else if (!strcmp(m.member, "UnregisterAgent")) {
			agent_path = NULL;
			nmb__reply_to_call(fd, &m, NULL, NULL);
		} else if (!strcmp(m.member, "RequestDefaultAgent")) {
			nmb__reply_to_call(fd, &m, NULL, NULL);
			if (access(flag, F_OK) == 0) { unlink(flag); drive_agent(fd); }   /* `bt agent` test: bluez talks to us unprompted */
		} else if (!strcmp(m.member, "Pair")) {
			nmb__Msg req = m;   /* drive_agent reads further messages into the shared buffer */
			int bad = drive_agent(fd);
			if (bad) { nmb__Buf eb2 = {0}; nmb__put_str(&eb2, "pairing failed"); nmb__build(&out, 3, nmb__serial_no++, req.sender, NULL, NULL, NULL, req.serial, "org.bluez.Error.AuthenticationFailed", "s", &eb2); nmb__send_msg(fd, &out); free(eb2.b); }
			else nmb__reply_to_call(fd, &req, NULL, NULL);
		} else if (!strcmp(m.member, "GeneratePin")) {
			nmb__Buf b = {0};
			nmb__put_str(&b, "87654321");
			nmb__reply_to_call(fd, &m, "s", &b);
			free(b.b);
		} else if (!strcmp(m.iface, "org.freedesktop.DBus.Introspectable")) {
			nmb__reply_to_call(fd, &m, NULL, NULL);
		} else nmb__reply_to_call(fd, &m, NULL, NULL);
		free(out.b);
	}
}

static void dbus_tests(void)
{
	char conf[320], addr[360], sock[320];
	int op[2], rp[2], st;
	pid_t dpid, spid;
	FILE *f;
	const char *l;
	snprintf(sock, sizeof sock, "%s/bus", tmp);
	snprintf(conf, sizeof conf, "%s/bus.conf", tmp);
	f = fopen(conf, "w");
	fprintf(f, "<busconfig><type>session</type><listen>unix:path=%s</listen><auth>EXTERNAL</auth>"
	           "<policy context=\"default\"><allow send_destination=\"*\" eavesdrop=\"true\"/><allow eavesdrop=\"true\"/><allow own=\"*\"/></policy></busconfig>", sock);
	fclose(f);
	pipe(op);
	if (!(dpid = fork())) {
		dup2(op[1], 1);
		execlp("dbus-daemon", "dbus-daemon", "--config-file", conf, "--nofork", "--print-address", (char *)NULL);
		_exit(127);
	}
	close(op[1]);
	{ char line[300] = ""; ssize_t n = read(op[0], line, sizeof line - 1); (void)n; if (!strstr(line, "unix:")) { printf("SKIP dbus tests: dbus-daemon did not start\n"); kill(dpid, SIGTERM); return; } }
	snprintf(addr, sizeof addr, "unix:path=%s", sock);
	setenv("DBUS_SYSTEM_BUS_ADDRESS", addr, 1);
	{   /* backend must be iwd for the wifi part */
		char d[320];
		snprintf(d, sizeof d, "%s/empty", tmp);
		mkdir(d, 0700);
		setenv("NMB_WPA_DIR", d, 1);
	}
	pipe(rp);
	if (!(spid = fork())) { fake_service(rp[1]); _exit(0); }
	{ char c; if (read(rp[0], &c, 1) != 1) { puts("fake service did not start"); fails++; kill(dpid, SIGTERM); return; } }

	/* bluetooth list */
	cap_begin(); CHECK(nmb_bt_list() == 0, "bt list, err=%s", eb); cap_end();
	CHECK(strstr(ob, "AA:BB:CC:DD:EE:FF") && strstr(ob, "Fake Pad"), "bt list: %s", ob);

	/* pair, non-interactive --pin: the agent answers every prompt */
	nmb_bt_set_pin("424242");
	cap_begin(); CHECK(nmb_bt_action("pair", "Fake Pad") == 0, "pair rc, err=%s", eb); cap_end();
	l = slurp_file("dbus.log");
	CHECK(strstr(l, "AgentManager1.RegisterAgent /nmb/btagent KeyboardDisplay") && strstr(l, "RequestDefaultAgent /nmb/btagent") && strstr(l, "Device1.Pair") && strstr(l, "UnregisterAgent /nmb/btagent"), "agent registered, pair called, agent removed:\n%s", l);
	CHECK(strstr(l, "agent RequestPinCode -> 424242") && strstr(l, "agent RequestPasskey -> 424242") && strstr(l, "agent RequestConfirmation -> ok") &&
	      strstr(l, "agent DisplayPasskey -> ok") && strstr(l, "agent AuthorizeService -> ok"), "agent answers:\n%s", l);
	CHECK(strstr(ob, "654321") && strstr(ob, "111222") && strstr(ob, "non-interactive"), "prompts shown: %s", ob);

	/* pair interactively: answers come from nmb_in; a wrong ending ("n") rejects the confirmation and the pair fails */
	unlink(strcat(strcpy((char[300]){0}, tmp), "/dbus.log"));
	{
		char path[300];
		snprintf(path, sizeof path, "%s/dbus.log", tmp);
		unlink(path);
	}
	nmb_bt_set_pin(NULL);
	nmb_in = fmemopen("1234\n99\nn\n", 10, "r");
	cap_begin(); CHECK(nmb_bt_action("pair", "AA:BB:CC:DD:EE:FF") == 1, "declined pairing must fail"); cap_end();
	fclose(nmb_in); nmb_in = NULL;
	l = slurp_file("dbus.log");
	CHECK(strstr(l, "agent RequestPinCode -> 1234") && strstr(l, "agent RequestPasskey -> 99") && strstr(l, "agent RequestConfirmation -> error org.bluez.Error.Rejected"), "interactive answers:\n%s", l);
	CHECK(strstr(eb, "AuthenticationFailed"), "error shown: %s", eb);

	/* `bt agent`: runs for 2 s, bluez drives it right after RequestDefaultAgent */
	{
		char path[300];
		snprintf(path, sizeof path, "%s/dbus.log", tmp); unlink(path);
		snprintf(path, sizeof path, "%s/drive", tmp); close(open(path, O_CREAT | O_WRONLY, 0600));
	}
	nmb_bt_set_pin("7777");
	cap_begin(); CHECK(nmb_bt_agent(2) == 0, "agent rc, err=%s", eb); cap_end();
	l = slurp_file("dbus.log");
	CHECK(strstr(ob, "pairing agent registered") && strstr(l, "agent RequestPinCode -> 7777") && strstr(l, "UnregisterAgent"), "bt agent:\n%s\n%s", ob, l);
	nmb_bt_set_pin(NULL);

	/* iwd: known networks, autoconnect, hotspot, WPS */
	{ char path[300]; snprintf(path, sizeof path, "%s/dbus.log", tmp); unlink(path); }
	cap_begin();
	CHECK(nmb_wifi_known() == 0, "known");
	CHECK(nmb_wifi_autoconnect("Home", 0) == 0, "autoconnect off");
	CHECK(nmb_wifi_autoconnect("Nowhere", 1) == 1, "autoconnect unknown ssid");
	CHECK(nmb_wifi_hotspot_start("Hot Spot", "12345678") == 0, "hotspot start, err=%s", eb);
	CHECK(nmb_wifi_hotspot_start("Hot Spot", NULL) == 1, "iwd hotspot without a passphrase refused");
	CHECK(nmb_wifi_hotspot_stop() == 0, "hotspot stop, err=%s", eb);
	CHECK(nmb_wifi_wps_pbc() == 0, "wps pbc, err=%s", eb);
	CHECK(nmb_wifi_wps_pin(NULL) == 0, "wps pin, err=%s", eb);
	CHECK(nmb_wifi_wps_pin("12345670") == 0, "wps pin given, err=%s", eb);
	CHECK(nmb_wifi_wps_cancel() == 0, "wps cancel, err=%s", eb);
	cap_end();
	CHECK(strstr(ob, "Home") && strstr(ob, "autoconnect=on"), "known list: %s", ob);
	l = slurp_file("dbus.log");
	CHECK(strstr(l, "/net/connman/iwd/aaaa_psk org.freedesktop.DBus.Properties.Set net.connman.iwd.KnownNetwork AutoConnect false"), "autoconnect Set:\n%s", l);
	CHECK(strstr(l, "Properties.Set net.connman.iwd.Device Mode ap") && strstr(l, "net.connman.iwd.AccessPoint.Start Hot Spot 12345678") &&
	      strstr(l, "net.connman.iwd.AccessPoint.Stop") && strstr(l, "Properties.Set net.connman.iwd.Device Mode station"), "hotspot calls:\n%s", l);
	CHECK(strstr(l, "SimpleConfiguration.PushButton") && strstr(l, "SimpleConfiguration.GeneratePin") && strstr(l, "SimpleConfiguration.StartPin 87654321") &&
	      strstr(l, "SimpleConfiguration.StartPin 12345670") && strstr(l, "SimpleConfiguration.Cancel"), "wps calls:\n%s", l);
	CHECK(strstr(ob, "PIN: 87654321"), "generated pin printed: %s", ob);

	kill(spid, SIGTERM); kill(dpid, SIGTERM);
	waitpid(spid, &st, 0); waitpid(dpid, &st, 0);
	unsetenv("NMB_WPA_DIR");
}

int main(void)
{
	snprintf(tmp, sizeof tmp, "/tmp/nmbt-%d", (int)getpid());
	mkdir(tmp, 0700);
	signal(SIGPIPE, SIG_IGN);
	unit();
	wpa_tests();
	dbus_tests();
	printf("%d checks, %d failed\n", checks, fails);
	{ char cmd[400]; snprintf(cmd, sizeof cmd, "rm -rf %s", tmp); if (!fails && system(cmd)) {} else if (fails) printf("(logs kept in %s)\n", tmp); }
	return fails != 0;
}
