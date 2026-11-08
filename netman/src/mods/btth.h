/* btth.h - Bluetooth through bluez (D-Bus): list, scan, connect, disconnect, pair, trust, remove, adapter power, and a pairing agent
 * (org.bluez.Agent1) that answers PIN / passkey / confirmation prompts on the terminal (netman_in, default stdin) or
 * non-interactively via netman_bt_set_pin. stb-style, one header on top of dbus.h.
 *
 *   #define NETMAN_BT_IMPLEMENTATION   in exactly one .c file, then #include "btth.h"
 *
 * Every call returns 0 on success, 1 on failure (reason to netman_err), 2 for an unknown action. Results go to netman_out.
 * bluez calls our agent as root; the default D-Bus policy lets any user own an agent, so no sudo is needed. Not thread-safe.
 */
#ifndef NETMAN_BT_H
#define NETMAN_BT_H
#ifdef NETMAN_BT_IMPLEMENTATION
#define NETMAN_DBUS_IMPLEMENTATION
#endif
#include "dbus.h"

#ifdef __cplusplus
extern "C" {
#endif
int netman_bt_list(void);
int netman_bt_scan(int seconds);                                     /* discover for N seconds (<=0 means 5), then list */
int netman_bt_action(const char *action, const char *name_or_mac);   /* connect|disconnect|pair|trust|remove; 2 for an unknown action */
int netman_bt_power(int on);
void netman_bt_set_pin(const char *pin);                             /* non-interactive: answer PIN/passkey prompts with it, accept confirmations */
int netman_bt_agent(int seconds);                                    /* register as the default pairing agent for N seconds (<=0: until SIGINT) */
int netman_bt_info(const char *name_or_mac);                         /* details of one device: address, class, RSSI, paired/trusted/connected, battery, profiles */
int netman_bt_adapter(void);                                         /* "key   value" lines of the adapter: address, name, powered, discoverable, pairable, discovering, paired / connected counts */
int netman_bt_status(void);                                          /* "bt       adapter on, nothing connected" style lines */
#ifdef __cplusplus
}
#endif
#endif /* NETMAN_BT_H */

#ifdef NETMAN_BT_IMPLEMENTATION
#ifndef NETMAN_BT_IMPLEMENTATION_DONE
#define NETMAN_BT_IMPLEMENTATION_DONE

/* ---- bluetooth (bluez) ------------------------------------------------------------------------------------------- */

#define netman_bt__BZ "org.bluez"
#define netman_bt__DEV netman_bt__BZ ".Device1"
#define netman_bt__ADP netman_bt__BZ ".Adapter1"

static int netman_bt__bz_load(int fd)
{
	if (netman_dbus_load_objects(fd, netman_bt__BZ) || !netman_dbus_first_path(netman_bt__ADP)) { fputs("bluetooth: bluez is not running or there is no adapter\n", NETMAN_ERR); return -1; }
	return 0;
}

static void netman_bt__print(void)
{
	for (int i = 0; i < netman_dbus_nprops; i++) {
		const char *path = netman_dbus_props[i].path;
		if (strcmp(netman_dbus_props[i].iface, netman_bt__DEV) || *netman_dbus_props[i].key) continue;
		fprintf(NETMAN_OUT, "%c%c%c %s  %s  %s dBm\n", !strcmp(netman_dbus_pget(path, netman_bt__DEV, "Connected") ?: "", "true") ? 'C' : '-', !strcmp(netman_dbus_pget(path, netman_bt__DEV, "Paired") ?: "", "true") ? 'P' : '-',
		       !strcmp(netman_dbus_pget(path, netman_bt__DEV, "Trusted") ?: "", "true") ? 'T' : '-', netman_dbus_pget(path, netman_bt__DEV, "Address") ?: "?", netman_dbus_pget(path, netman_bt__DEV, "Alias") ?: "?",
		       netman_dbus_pget(path, netman_bt__DEV, "RSSI") ?: "  -");
	}
}

static const char *netman_bt__find(const char *what)
{
	for (int i = 0; i < netman_dbus_nprops; i++) {
		const char *a, *n;
		if (strcmp(netman_dbus_props[i].iface, netman_bt__DEV) || *netman_dbus_props[i].key) continue;
		a = netman_dbus_pget(netman_dbus_props[i].path, netman_bt__DEV, "Address"); n = netman_dbus_pget(netman_dbus_props[i].path, netman_bt__DEV, "Alias");
		if ((a && !strcasecmp(a, what)) || (n && strcasestr(n, what))) return netman_dbus_props[i].path;
	}
	return NULL;
}

static int netman_bt__set_bool(int fd, const char *path, const char *iface, const char *key, int v) { return netman_dbus_set_var(fd, netman_bt__BZ, path, iface, key, v, NULL); }

/* system bus + bluez objects; *ad = adapter path. Returns the fd or -1 (message printed). */
static int netman_bt__bz_open(const char **ad)
{
	int fd = netman_dbus_system_bus();
	if (fd < 0) return -1;
	signal(SIGPIPE, SIG_IGN);
	if (netman_bt__bz_load(fd)) { close(fd); return -1; }
	*ad = netman_dbus_first_path(netman_bt__ADP);
	return fd;
}

/* ---- bluetooth pairing agent (org.bluez.Agent1) ------------------------------------------------------------------ */

static const char *netman_bt__pin;
static volatile sig_atomic_t netman_bt__stop;
#define netman_bt__AGENT_PATH "/netman/btagent"

static void netman_bt__error_reply(int fd, const netman_dbus_Msg *req, const char *name, const char *text)
{
	netman_dbus_Buf out = {0}, b = {0};
	netman_dbus_put_str(&b, text);
	netman_dbus_build(&out, 3, netman_dbus_serial_no++, req->sender, NULL, NULL, NULL, req->serial, name, "s", &b);
	netman_dbus_send_msg(fd, &out);
	free(out.b); free(b.b);
}

static int netman_bt__ask(const char *q, char *buf, size_t n)
{
	FILE *in = netman_in ? netman_in : stdin;
	fprintf(NETMAN_OUT, "%s ", q);
	fflush(NETMAN_OUT);
	if (!fgets(buf, n, in)) return -1;
	buf[strcspn(buf, "\r\n")] = 0;
	return 0;
}

/* yes/no: non-interactive mode (a PIN was given) accepts */
static int netman_bt__confirm(const char *q)
{
	char a[16];
	if (netman_bt__pin) { fprintf(NETMAN_OUT, "%s yes (non-interactive)\n", q); return 1; }
	return !netman_bt__ask(q, a, sizeof a) && (a[0] == 'y' || a[0] == 'Y');
}

static void netman_bt__serve(int fd, const netman_dbus_Msg *m)
{
	netman_dbus_R r = { m->body, 0, m->blen, 0 };
	netman_dbus_Buf b = {0};
	const char *mem = m->member, *dev, *alias;
	char ans[96], q[200];
	if (strcmp(m->iface, "org.bluez.Agent1")) { netman_bt__error_reply(fd, m, "org.freedesktop.DBus.Error.UnknownMethod", "not implemented"); return; }
	if (!strcmp(mem, "Release")) { netman_dbus_reply_to_call(fd, m, NULL, NULL); return; }
	if (!strcmp(mem, "Cancel")) { netman_dbus_puts("pairing cancelled"); netman_dbus_reply_to_call(fd, m, NULL, NULL); return; }
	dev = m->sig[0] == 'o' ? netman_dbus_r_str(&r) : "";
	alias = netman_dbus_pget(dev, netman_bt__DEV, "Alias") ?: netman_dbus_pget(dev, netman_bt__DEV, "Address") ?: dev;
	if (!strcmp(mem, "RequestPinCode")) {
		snprintf(q, sizeof q, "PIN code for %s:", alias);
		if (netman_bt__pin) snprintf(ans, sizeof ans, "%s", netman_bt__pin);
		else if (netman_bt__ask(q, ans, sizeof ans)) ans[0] = 0;
		if (!ans[0] || strlen(ans) > 16) { netman_bt__error_reply(fd, m, "org.bluez.Error.Rejected", "no PIN"); return; }
		netman_dbus_put_str(&b, ans);
		netman_dbus_reply_to_call(fd, m, "s", &b);
	} else if (!strcmp(mem, "RequestPasskey")) {
		char *end;
		unsigned long v;
		snprintf(q, sizeof q, "passkey (0-999999) for %s:", alias);
		if (netman_bt__pin) snprintf(ans, sizeof ans, "%s", netman_bt__pin);
		else if (netman_bt__ask(q, ans, sizeof ans)) ans[0] = 0;
		v = strtoul(ans, &end, 10);
		if (!ans[0] || *end || v > 999999) { netman_bt__error_reply(fd, m, "org.bluez.Error.Rejected", "bad passkey"); return; }
		netman_dbus_put_u32(&b, (uint32_t)v);
		netman_dbus_reply_to_call(fd, m, "u", &b);
	} else if (!strcmp(mem, "DisplayPinCode")) {
		fprintf(NETMAN_OUT, "enter this PIN on %s: %s\n", alias, netman_dbus_r_str(&r));
		netman_dbus_reply_to_call(fd, m, NULL, NULL);
	} else if (!strcmp(mem, "DisplayPasskey")) {
		fprintf(NETMAN_OUT, "enter this passkey on %s: %06u\n", alias, netman_dbus_r_u32(&r));
		netman_dbus_reply_to_call(fd, m, NULL, NULL);
	} else if (!strcmp(mem, "RequestConfirmation")) {
		snprintf(q, sizeof q, "%s shows passkey %06u - does it match? [y/N]", alias, netman_dbus_r_u32(&r));
		if (netman_bt__confirm(q)) netman_dbus_reply_to_call(fd, m, NULL, NULL); else netman_bt__error_reply(fd, m, "org.bluez.Error.Rejected", "declined");
	} else if (!strcmp(mem, "RequestAuthorization")) {
		snprintf(q, sizeof q, "allow pairing with %s? [y/N]", alias);
		if (netman_bt__confirm(q)) netman_dbus_reply_to_call(fd, m, NULL, NULL); else netman_bt__error_reply(fd, m, "org.bluez.Error.Rejected", "declined");
	} else if (!strcmp(mem, "AuthorizeService")) {
		snprintf(q, sizeof q, "allow %s to use service %s? [y/N]", alias, netman_dbus_r_str(&r));
		if (netman_bt__confirm(q)) netman_dbus_reply_to_call(fd, m, NULL, NULL); else netman_bt__error_reply(fd, m, "org.bluez.Error.Rejected", "declined");
	} else netman_bt__error_reply(fd, m, "org.freedesktop.DBus.Error.UnknownMethod", mem);
	free(b.b);
}

/* installed into the D-Bus layer while the agent is registered: serve bluez's Agent1 calls that arrive during any pending call */
static int netman_bt__hook(int fd, const netman_dbus_Msg *m)
{
	if (strcmp(m->iface, "org.bluez.Agent1")) return 0;
	netman_bt__serve(fd, m);
	return 1;
}

static int netman_bt__register(int fd)
{
	netman_dbus_Buf b = {0};
	int rc;
	netman_dbus_put_str(&b, netman_bt__AGENT_PATH); netman_dbus_put_str(&b, "KeyboardDisplay");
	rc = netman_dbus_simple_call(fd, netman_bt__BZ, "/org/bluez", "org.bluez.AgentManager1", "RegisterAgent", "os", &b, "register agent");
	free(b.b);
	if (rc) return rc;
	netman_dbus_hook = netman_bt__hook;
	b = (netman_dbus_Buf){0};
	netman_dbus_put_str(&b, netman_bt__AGENT_PATH);
	rc = netman_dbus_simple_call(fd, netman_bt__BZ, "/org/bluez", "org.bluez.AgentManager1", "RequestDefaultAgent", "o", &b, "default agent");
	free(b.b);
	return rc;
}

static void netman_bt__unregister(int fd)
{
	netman_dbus_Buf b = {0};
	netman_dbus_put_str(&b, netman_bt__AGENT_PATH);
	netman_dbus_simple_call(fd, netman_bt__BZ, "/org/bluez", "org.bluez.AgentManager1", "UnregisterAgent", "o", &b, "unregister agent");
	free(b.b);
	netman_dbus_hook = NULL;
}

static void netman_bt__on_signal(int s) { (void)s; netman_bt__stop = 1; }


int netman_bt_list(void)
{
	const char *ad;
	int fd = netman_bt__bz_open(&ad);
	if (fd < 0) return 1;
	netman_bt__print();
	close(fd);
	return 0;
}

int netman_bt_scan(int secs)
{
	const char *ad;
	int fd = netman_bt__bz_open(&ad);
	if (fd < 0) return 1;
	if (netman_dbus_simple_call(fd, netman_bt__BZ, ad, netman_bt__ADP, "StartDiscovery", NULL, NULL, "scan")) { close(fd); return 1; }
	sleep(secs > 0 ? secs : 5);
	netman_dbus_simple_call(fd, netman_bt__BZ, ad, netman_bt__ADP, "StopDiscovery", NULL, NULL, "scan");
	if (netman_bt__bz_load(fd)) { close(fd); return 1; }
	netman_bt__print();
	close(fd);
	return 0;
}

int netman_bt_power(int on)
{
	const char *ad;
	int fd = netman_bt__bz_open(&ad), rc;
	if (fd < 0) return 1;
	rc = netman_bt__set_bool(fd, ad, netman_bt__ADP, "Powered", on);
	close(fd);
	return rc;
}

int netman_bt_action(const char *action, const char *what)
{
	const char *ad, *dev;
	int fd = netman_bt__bz_open(&ad), rc = 2;
	if (fd < 0) return 1;
	if (!(dev = netman_bt__find(what))) { fprintf(NETMAN_ERR, "bluetooth: no device matching %s (try: bt scan)\n", what); close(fd); return 1; }
	netman_dbus_call_timeout = 30000;
	if (!strcmp(action, "connect")) rc = netman_dbus_simple_call(fd, netman_bt__BZ, dev, netman_bt__DEV, "Connect", NULL, NULL, "connect");
	else if (!strcmp(action, "disconnect")) rc = netman_dbus_simple_call(fd, netman_bt__BZ, dev, netman_bt__DEV, "Disconnect", NULL, NULL, "disconnect");
	else if (!strcmp(action, "pair")) {   /* with our own agent so PIN/passkey/confirmation prompts reach this terminal */
		if (netman_bt__register(fd)) fputs("bluetooth: could not become the pairing agent; pairing without one\n", NETMAN_ERR);
		netman_dbus_call_timeout = 60000;
		rc = netman_dbus_simple_call(fd, netman_bt__BZ, dev, netman_bt__DEV, "Pair", NULL, NULL, "pair");
		if (netman_dbus_hook) netman_bt__unregister(fd);
	}
	else if (!strcmp(action, "trust")) rc = netman_bt__set_bool(fd, dev, netman_bt__DEV, "Trusted", 1);
	else if (!strcmp(action, "remove")) {
		netman_dbus_Buf b = {0};
		netman_dbus_put_str(&b, dev);
		rc = netman_dbus_simple_call(fd, netman_bt__BZ, ad, netman_bt__ADP, "RemoveDevice", "o", &b, "remove");
		free(b.b);
	}
	close(fd);
	return rc;
}

void netman_bt_set_pin(const char *pin) { netman_bt__pin = pin; }

int netman_bt_agent(int secs)
{
	const char *ad;
	int fd = netman_bt__bz_open(&ad);
	time_t end = secs > 0 ? time(NULL) + secs : 0;
	struct sigaction sa = { .sa_handler = netman_bt__on_signal };
	if (fd < 0) return 1;
	sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL);
	netman_bt__stop = 0;
	if (netman_bt__register(fd)) { close(fd); return 1; }
	netman_dbus_puts("pairing agent registered, waiting for requests (Ctrl-C to stop)");
	while (!netman_bt__stop && (!end || time(NULL) < end)) {
		struct pollfd p = { fd, POLLIN, 0 };
		netman_dbus_Msg m;
		if (poll(&p, 1, 500) <= 0) continue;
		if (netman_dbus_read_msg(fd, &m, 3000)) break;
		if (m.type == 1) netman_bt__serve(fd, &m);
	}
	netman_bt__unregister(fd);
	close(fd);
	return 0;
}


static const char *netman_bt__profile(const char *uuid)
{
	static const struct { const char *u, *n; } t[] = {
		{ "0000110a", "A2DP source" }, { "0000110b", "A2DP sink (audio)" }, { "0000110c", "AVRCP target" }, { "0000110e", "AVRCP remote" },
		{ "0000111e", "Handsfree" }, { "0000111f", "Handsfree gateway" }, { "00001108", "Headset" }, { "00001112", "Headset AG" },
		{ "00001124", "HID" }, { "00001812", "HID over GATT" }, { "00001105", "OBEX push" }, { "0000112f", "Phonebook" }, { "00001200", "PnP info" },
		{ "0000180f", "Battery" }, { "0000180a", "Device info" }, { "00001800", "GAP" }, { "00001801", "GATT" }, { "00001115", "PAN" }, { "00001116", "NAP" },
	};
	for (size_t i = 0; i < sizeof t / sizeof *t; i++) if (!strncasecmp(uuid, t[i].u, 8)) return t[i].n;
	return NULL;
}

int netman_bt_info(const char *what)
{
	const char *ad, *p, *v;
	int fd = netman_bt__bz_open(&ad);
	if (fd < 0) return 1;
	if (!what || !(p = netman_bt__find(what))) { fprintf(NETMAN_ERR, "bt: no device matching %s (bt list)\n", what ? what : "(nothing)"); close(fd); return 1; }
	fprintf(NETMAN_OUT, "%-14s %s\n", "name", netman_dbus_pget(p, netman_bt__DEV, "Alias") ?: "?");
	fprintf(NETMAN_OUT, "%-14s %s\n", "address", netman_dbus_pget(p, netman_bt__DEV, "Address") ?: "?");
	if ((v = netman_dbus_pget(p, netman_bt__DEV, "Class"))) {
		unsigned c = (unsigned)strtoul(v, NULL, 10), major = (c >> 8) & 31;
		static const char *const maj[] = { "misc", "computer", "phone", "network", "audio/video", "peripheral", "imaging", "wearable", "toy", "health" };
		fprintf(NETMAN_OUT, "%-14s 0x%06x (%s)\n", "class", c, major < 10 ? maj[major] : "other");
	}
	if ((v = netman_dbus_pget(p, netman_bt__DEV, "Icon"))) fprintf(NETMAN_OUT, "%-14s %s\n", "type", v);
	if ((v = netman_dbus_pget(p, netman_bt__DEV, "RSSI"))) fprintf(NETMAN_OUT, "%-14s %d dBm\n", "signal", atoi(v));
	else fprintf(NETMAN_OUT, "%-14s not in range / not scanning\n", "signal");
	fprintf(NETMAN_OUT, "%-14s %s\n", "paired", netman_dbus_pget(p, netman_bt__DEV, "Paired") && !strcmp(netman_dbus_pget(p, netman_bt__DEV, "Paired"), "true") ? "yes" : "no");
	fprintf(NETMAN_OUT, "%-14s %s\n", "trusted", netman_dbus_pget(p, netman_bt__DEV, "Trusted") && !strcmp(netman_dbus_pget(p, netman_bt__DEV, "Trusted"), "true") ? "yes" : "no");
	fprintf(NETMAN_OUT, "%-14s %s\n", "connected", netman_dbus_pget(p, netman_bt__DEV, "Connected") && !strcmp(netman_dbus_pget(p, netman_bt__DEV, "Connected"), "true") ? "yes" : "no");
	if ((v = netman_dbus_pget(p, "org.bluez.Battery1", "Percentage"))) fprintf(NETMAN_OUT, "%-14s %d %%\n", "battery", atoi(v));
	if ((v = netman_dbus_pget(p, netman_bt__DEV, "UUIDs")) && *v) {
		char list[640], *q, *sv = NULL;
		snprintf(list, sizeof list, "%s", v);
		for (q = strtok_r(list, ",", &sv); q; q = strtok_r(NULL, ",", &sv)) fprintf(NETMAN_OUT, "%-14s %s\n", "profile", netman_bt__profile(q) ?: q);
	}
	close(fd);
	return 0;
}

int netman_bt_adapter(void)
{
	int fd = netman_dbus_system_bus(), paired = 0, conn = 0, known = 0;
	const char *ad, *v;
	if (fd < 0) return 1;
	if (netman_dbus_load_objects(fd, netman_bt__BZ) || !(ad = netman_dbus_first_path(netman_bt__ADP))) { fputs("bluetooth: bluez is not running or there is no adapter\n", NETMAN_ERR); close(fd); return 1; }
	for (int i = 0; i < netman_dbus_nprops; i++) {
		const char *path = netman_dbus_props[i].path;
		if (strcmp(netman_dbus_props[i].iface, netman_bt__DEV) || *netman_dbus_props[i].key) continue;
		known++;
		if (!strcmp(netman_dbus_pget(path, netman_bt__DEV, "Paired") ?: "", "true")) paired++;
		if (!strcmp(netman_dbus_pget(path, netman_bt__DEV, "Connected") ?: "", "true")) conn++;
	}
#define NB_YN(k) (!strcmp(netman_dbus_pget(ad, netman_bt__ADP, k) ?: "", "true") ? "yes" : (netman_dbus_pget(ad, netman_bt__ADP, k) ? "no" : "-"))
	fprintf(NETMAN_OUT, "%-14s %s\n", "adapter", (v = netman_dbus_pget(ad, netman_bt__ADP, "Name")) ? v : ad);
	fprintf(NETMAN_OUT, "%-14s %s\n", "address", (v = netman_dbus_pget(ad, netman_bt__ADP, "Address")) ? v : "-");
	fprintf(NETMAN_OUT, "%-14s %s\n", "powered", NB_YN("Powered"));
	fprintf(NETMAN_OUT, "%-14s %s\n", "discoverable", NB_YN("Discoverable"));
	fprintf(NETMAN_OUT, "%-14s %s\n", "pairable", NB_YN("Pairable"));
	fprintf(NETMAN_OUT, "%-14s %s\n", "discovering", NB_YN("Discovering"));
#undef NB_YN
	fprintf(NETMAN_OUT, "%-14s %d paired, %d connected, %d known\n", "devices", paired, conn, known);
	close(fd);
	return 0;
}

int netman_bt_status(void)
{
	int fd = netman_dbus_system_bus();
	if (fd < 0) return 0;
	if (!netman_dbus_load_objects(fd, netman_bt__BZ) && netman_dbus_first_path(netman_bt__ADP)) {
		int n = 0;
		const char *ad = netman_dbus_first_path(netman_bt__ADP);
		for (int i = 0; i < netman_dbus_nprops; i++)
			if (!strcmp(netman_dbus_props[i].iface, netman_bt__DEV) && !*netman_dbus_props[i].key && !strcmp(netman_dbus_pget(netman_dbus_props[i].path, netman_bt__DEV, "Connected") ?: "", "true")) {
				fprintf(NETMAN_OUT, "bt       connected: %s\n", netman_dbus_pget(netman_dbus_props[i].path, netman_bt__DEV, "Alias") ?: "?");
				n++;
			}
		fprintf(NETMAN_OUT, "bt       adapter %s%s\n", !strcmp(netman_dbus_pget(ad, netman_bt__ADP, "Powered") ?: "", "true") ? "on" : "off", n ? "" : ", nothing connected");
	} else netman_dbus_puts("bt       bluez not running or no adapter");
	close(fd);
	return 0;
}

#endif /* NETMAN_BT_IMPLEMENTATION_DONE */
#endif /* NETMAN_BT_IMPLEMENTATION */
