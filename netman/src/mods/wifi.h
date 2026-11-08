/* wifi.h - Wi-Fi through iwd (D-Bus) or wpa_supplicant (control socket): scan/list, connect, disconnect, forget, WPS (push button /
 * PIN), hotspot, saved networks + autoconnect. stb-style, one header; the iwd half sits on dbus.h.
 *
 *   #define NETMAN_WIFI_IMPLEMENTATION   in exactly one .c file, then #include "wifi.h"
 *
 * Every call returns 0 on success, 1 on failure (reason to netman_err), 2 when the request makes no sense for the backend (forget
 * under wpa_supplicant). Human-readable results go to netman_out. Not thread-safe.
 *
 * Backend: wpa_supplicant if $NETMAN_WPA_DIR/$NETMAN_IFACE (default /run/wpa_supplicant/wlan0) exists or is unreadable (needs root or
 * the netdev group), otherwise iwd; netman_wifi_backend(1|2) forces iwd / wpa (config key wifi_backend). Env: NETMAN_WPA_DIR,
 * NETMAN_IFACE, NETMAN_WPS_SECS (WPS wait, default 120), NETMAN_AP_FREQ (hotspot frequency under wpa_supplicant, default 2437),
 * DBUS_SYSTEM_BUS_ADDRESS.
 */
#ifndef NETMAN_WIFI_H
#define NETMAN_WIFI_H
#ifdef NETMAN_WIFI_IMPLEMENTATION
#define NETMAN_DBUS_IMPLEMENTATION
#endif
#include "dbus.h"

#ifdef __cplusplus
extern "C" {
#endif
void netman_wifi_backend(int which);                                 /* 0 auto (default), 1 iwd, 2 wpa_supplicant */
int netman_wifi_backend_is_wpa(void);                                /* 1 when wpa_supplicant owns the interface, 0 for iwd */
int netman_wifi_list(void);                                          /* scan and list visible networks (human table) */
int netman_wifi_scan(void);                                          /* the same as TSV, one row per network, strongest first:
                                                                        FLAG KNOWN SEC DBM PCT BSSID CHAN BAND NBSS HIDDEN NAME  (tab separated; FLAG '*' = connected;
                                                                        NAME is escaped, see netman_wifi_escape; empty for a hidden network; "-" = not known) */
int netman_wifi_info(const char *ssid);                              /* details of SSID (NULL = the connected one): "key   value" lines, see wifi.h */
int netman_wifi_connect_eap(const char *ssid, const char *identity, const char *pass);   /* 802.1X: identity + password (wpa: PEAP/MSCHAPV2) */
const char *netman_wifi_last_error(void);                            /* text of the last failure of connect (also on netman_err) */
int netman_wifi_last_wrong_password(void);                           /* 1 when the last connect failed in a way that means a wrong passphrase */
/* pure helpers (unit-tested): */
size_t netman_wifi_escape(const char *raw, size_t n, char *out, size_t on);   /* SSID bytes -> printable UTF-8: \\, \t, \n, \xNN for control bytes and invalid UTF-8 */
size_t netman_wifi_unescape(const char *esc, char *out, size_t on);           /* the reverse; out is NUL terminated */
const char *netman_wifi_sec_iwd(const char *type);                   /* iwd Network.Type (open|psk|8021x|wep) -> OPEN|WPA2|802.1X|WEP */
const char *netman_wifi_sec_flags(const char *wpa_flags);            /* wpa_supplicant "[WPA2-PSK-CCMP][ESS]" -> OPEN|WEP|WPA|WPA2|WPA3|WPA2/3|802.1X */
int netman_wifi_pct(int dbm);                                        /* -100 dBm = 0 %, -50 dBm and better = 100 % */
int netman_wifi_chan(int mhz, const char **band);                    /* channel number of a frequency, band "2.4"|"5"|"6"; 0 = unknown */
int netman_wifi_connect(const char *ssid, const char *pass);         /* pass NULL = open or already known network */
int netman_wifi_disconnect(void);
int netman_wifi_forget(const char *ssid);                            /* iwd only; returns 2 under wpa_supplicant */
int netman_wifi_wps_pbc(void);                                       /* WPS push-button; waits for the result (NETMAN_WPS_SECS) */
int netman_wifi_wps_pin(const char *pin);                            /* WPS PIN (4 or 8 digits); NULL = generate and print one */
int netman_wifi_wps_cancel(void);
int netman_wifi_hotspot_start(const char *ssid, const char *pass);   /* pass 8-63 chars; NULL = open (wpa_supplicant only) */
int netman_wifi_hotspot_stop(void);
int netman_wifi_known(void);                                         /* saved networks with their autoconnect state */
int netman_wifi_autoconnect(const char *ssid, int on);
int netman_wifi_status(void);                                        /* one or two lines: "wifi     connected to X" / backend notes */
#ifdef __cplusplus
}
#endif
#endif /* NETMAN_WIFI_H */

#ifdef NETMAN_WIFI_IMPLEMENTATION
#ifndef NETMAN_WIFI_IMPLEMENTATION_DONE
#define NETMAN_WIFI_IMPLEMENTATION_DONE
#include <dirent.h>
#include <stdarg.h>
#include <sys/wait.h>

static char netman_wifi__lasterr[600];
static int netman_wifi__wrongpw;
const char *netman_wifi_last_error(void) { return netman_wifi__lasterr; }
int netman_wifi_last_wrong_password(void) { return netman_wifi__wrongpw; }
static void netman_wifi__seterr(const char *msg, int wrong) { snprintf(netman_wifi__lasterr, sizeof netman_wifi__lasterr, "%s", msg); netman_wifi__wrongpw = wrong; fprintf(NETMAN_ERR, "wifi: %s\n", msg); }
size_t netman_wifi_escape(const char *raw, size_t n, char *out, size_t on);
static int netman_wifi__force;   /* 0 auto, 1 iwd, 2 wpa */
void netman_wifi_backend(int which) { netman_wifi__force = which; }

/* ---- wifi through wpa_supplicant's control socket (when it owns the interface instead of iwd) ------------------------ */

/* ---- wifi through wpa_supplicant's control socket (when it owns the interface instead of iwd) ------------------------ */

static char netman_wifi__wpa_dir[] = "/run/wpa_supplicant";
static const char *netman_wifi__wdir(void) { const char *d = getenv("NETMAN_WPA_DIR"); return d && *d ? d : netman_wifi__wpa_dir; }

static int netman_wifi__wpa_cmd(const char *ifname, const char *cmd, char *out, size_t n)
{
	struct sockaddr_un to = { .sun_family = AF_UNIX }, me = { .sun_family = AF_UNIX };
	int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	struct pollfd p = { fd, POLLIN, 0 };
	ssize_t r;
	if (fd < 0) return -1;
	snprintf(to.sun_path, sizeof to.sun_path, "%s/%s", netman_wifi__wdir(), ifname);
	snprintf(me.sun_path, sizeof me.sun_path, "/tmp/netman-%d", (int)getpid());
	unlink(me.sun_path);
	if (bind(fd, (struct sockaddr *)&me, sizeof me) < 0 || connect(fd, (struct sockaddr *)&to, sizeof to) < 0 || send(fd, cmd, strlen(cmd), 0) < 0) {
		close(fd); unlink(me.sun_path);
		return -1;
	}
	r = poll(&p, 1, 10000) > 0 ? recv(fd, out, n - 1, 0) : -1;
	close(fd); unlink(me.sun_path);
	if (r < 0) return -1;
	out[r] = 0;
	return 0;
}

static int netman_wifi__wpa_present(const char *ifname)
{
	char p[300];
	snprintf(p, sizeof p, "%s/%s", netman_wifi__wdir(), ifname);
	return access(p, F_OK) == 0 || errno == EACCES;
}

static const char *netman_wifi__wpa_iface(void) { return getenv("NETMAN_IFACE") ?: "wlan0"; }

/* values go into a quoted control-socket command: refuse anything that could break out of the quotes */
static int netman_wifi__wpa_safe(const char *s) { return s && *s && !strpbrk(s, "\"\\\r\n"); }

/* ---- wifi (iwd) -------------------------------------------------------------------------------------------------- */

#define netman_wifi__IWD "net.connman.iwd"

int netman_wifi_passive;   /* 1 = listing reads the backend's cached results and never triggers a scan (the TUI scans only on `r`) */

static int netman_wifi__iwd_load(int fd)
{
	if (netman_dbus_load_objects(fd, netman_wifi__IWD)) { fputs("wifi: iwd is not running (systemctl start iwd)\n", NETMAN_ERR); return -1; }
	return 0;
}

static int netman_wifi__scan(int fd, const char *station)
{
	if (netman_dbus_simple_call(fd, netman_wifi__IWD, station, netman_wifi__IWD ".Station", "Scan", NULL, NULL, "scan")) return -1;
	for (int i = 0; i < 20; i++) {
		struct timespec ts = { 0, 500000000 };
		nanosleep(&ts, NULL);
		if (netman_wifi__iwd_load(fd)) return -1;
		if (strcmp(netman_dbus_pget(station, netman_wifi__IWD ".Station", "Scanning") ?: "false", "true")) break;
	}
	return 0;
}

static const char *netman_wifi__net_by_name(const char *name)
{
	for (int i = 0; i < netman_dbus_nprops; i++)
		if (!strcmp(netman_dbus_props[i].iface, netman_wifi__IWD ".Network") && !strcmp(netman_dbus_props[i].key, "Name") && !strcmp(netman_dbus_props[i].val, name)) return netman_dbus_props[i].path;
	return NULL;
}

/* connect to the system bus and load iwd's objects; *st = the station path. Returns the fd or -1 (message printed). */
static int netman_wifi__iwd_open(const char **st)
{
	int fd = netman_dbus_system_bus();
	if (fd < 0) return -1;
	signal(SIGPIPE, SIG_IGN);
	if (netman_wifi__iwd_load(fd)) { close(fd); return -1; }
	if (!(*st = netman_dbus_first_path(netman_wifi__IWD ".Station"))) { fputs("wifi: no wifi device in station mode\n", NETMAN_ERR); close(fd); return -1; }
	return fd;
}

static int netman_wifi__iwd_connect(int fd, const char *st, const char *ssid, const char *pass, const char *ident)
{
	const char *net = netman_wifi__net_by_name(ssid);
	netman_dbus_Msg m;
	char esc[200];
	int rc;
	netman_wifi_escape(ssid, strlen(ssid), esc, sizeof esc);
	if (!net) { if (netman_wifi__scan(fd, st)) return 1; net = netman_wifi__net_by_name(ssid); }
	if (!net) {   /* a hidden network: iwd connects by name */
		netman_dbus_Buf b = {0};
		netman_dbus_put_str(&b, ssid);
		netman_dbus_agent_pass = pass; netman_dbus_agent_user = ident;
		if (pass) {
			netman_dbus_Buf ab = {0};
			netman_dbus_put_str(&ab, "/netman/agent");
			netman_dbus_simple_call(fd, netman_wifi__IWD, "/net/connman/iwd", "net.connman.iwd.AgentManager", "RegisterAgent", "o", &ab, "agent");
			free(ab.b);
		}
		netman_dbus_call_timeout = 40000;
		rc = netman_dbus_call(fd, &m, netman_wifi__IWD, st, netman_wifi__IWD ".Station", "ConnectHiddenNetwork", "s", &b);
		free(b.b);
		if (!rc) { fprintf(NETMAN_OUT, "connected to %s\n", esc); return 0; }
		{ char msg[600]; snprintf(msg, sizeof msg, "no network called %s in range, or it refused us (%s)", esc, m.err[0] ? m.err : "no answer"); netman_wifi__seterr(msg, 0); }
		return 1;
	}
	if (pass) {
		netman_dbus_Buf b = {0};
		netman_dbus_agent_pass = pass; netman_dbus_agent_user = ident;
		netman_dbus_put_str(&b, "/netman/agent");
		rc = netman_dbus_simple_call(fd, netman_wifi__IWD, "/net/connman/iwd", "net.connman.iwd.AgentManager", "RegisterAgent", "o", &b, "agent");
		free(b.b);
		if (rc) return 1;
	}
	netman_dbus_call_timeout = 40000;
	rc = netman_dbus_call(fd, &m, netman_wifi__IWD, net, netman_wifi__IWD ".Network", "Connect", NULL, NULL);
	if (!rc) { fprintf(NETMAN_OUT, "connected to %s\n", esc); return 0; }
	{
		char msg[600], detail[160] = "";
		netman_dbus_R r = { m.body, 0, m.blen, 0 };
		int wrong;
		if (m.type == 3 && m.sig[0] == 's') snprintf(detail, sizeof detail, "%s", netman_dbus_r_str(&r));
		/* iwd answers a wrong passphrase / refused handshake with a bare net.connman.iwd.Failed ("Operation failed") */
		wrong = pass != NULL && (strstr(m.err, "Failed") || strstr(m.err, "InvalidArguments") || strstr(m.err, "Aborted")) && !strstr(m.err, "NotFound");
		if (wrong) snprintf(msg, sizeof msg, "%s for %s (%s%s%s)", ident ? "login refused" : "wrong passphrase", esc, m.err, *detail ? ": " : "", detail);
		else snprintf(msg, sizeof msg, "%s: %s%s%s", esc, m.err[0] ? m.err : "no answer", *detail ? ": " : "", detail);
		netman_wifi__seterr(msg, wrong);
	}
	return 1;
}

static int netman_wifi__iwd_forget(int fd, const char *ssid)
{
	const char *net = netman_wifi__net_by_name(ssid), *k = net ? netman_dbus_pget(net, netman_wifi__IWD ".Network", "KnownNetwork") : NULL;
	if (!k) { fputs("wifi: that network is not known\n", NETMAN_ERR); return 1; }
	return netman_dbus_simple_call(fd, netman_wifi__IWD, k, netman_wifi__IWD ".KnownNetwork", "Forget", NULL, NULL, "forget");
}

/* ---- wpa_supplicant sessions with events: WPS, hotspot, saved networks ------------------------------------------- */

typedef struct { int fd; char me[108]; } netman_wifi__Wpa;

static int netman_wifi__wpa_open(netman_wifi__Wpa *w, const char *ifname)
{
	struct sockaddr_un to = { .sun_family = AF_UNIX }, me = { .sun_family = AF_UNIX };
	w->fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (w->fd < 0) return -1;
	snprintf(to.sun_path, sizeof to.sun_path, "%s/%s", netman_wifi__wdir(), ifname);
	snprintf(me.sun_path, sizeof me.sun_path, "/tmp/netman-%d-w", (int)getpid());
	unlink(me.sun_path);
	if (bind(w->fd, (struct sockaddr *)&me, sizeof me) < 0 || connect(w->fd, (struct sockaddr *)&to, sizeof to) < 0) { close(w->fd); unlink(me.sun_path); return -1; }
	snprintf(w->me, sizeof w->me, "%s", me.sun_path);
	return 0;
}
static void netman_wifi__wpa_close(netman_wifi__Wpa *w) { close(w->fd); unlink(w->me); }

/* send a command; events ("<3>...") that arrive first are skipped. 0 = got a reply in out. */
static int netman_wifi__wpa_req(netman_wifi__Wpa *w, const char *cmd, char *out, size_t n)
{
	if (send(w->fd, cmd, strlen(cmd), 0) < 0) return -1;
	for (;;) {
		struct pollfd p = { w->fd, POLLIN, 0 };
		ssize_t r;
		if (poll(&p, 1, 10000) <= 0 || (r = recv(w->fd, out, n - 1, 0)) < 0) return -1;
		out[r] = 0;
		if (out[0] != '<') { out[strcspn(out, "\n")] = 0; return 0; }
	}
}

/* classify one control-socket event line: 1 = WPS/connect success, -1 = failure, 0 = progress or unrelated */
static int netman_wifi__wps_event(const char *ev)
{
	if (ev[0] == '<') { const char *e = strchr(ev, '>'); if (e) ev = e + 1; }
	if (!strncmp(ev, "WPS-SUCCESS", 11) || !strncmp(ev, "CTRL-EVENT-CONNECTED", 20)) return 1;
	if (!strncmp(ev, "WPS-FAIL", 8) || !strncmp(ev, "WPS-TIMEOUT", 11) || !strncmp(ev, "WPS-OVERLAP", 11) || !strncmp(ev, "WPS-CANCEL", 10)) return -1;
	return 0;
}

static int netman_wifi__wps_secs(void) { const char *e = getenv("NETMAN_WPS_SECS"); return e && atoi(e) > 0 ? atoi(e) : 120; }

/* ATTACH, run cmd (WPS_PBC / WPS_PIN ...), print a generated PIN, then wait for the outcome event */
static int netman_wifi__wpa_wps(const char *ifname, const char *cmd)
{
	netman_wifi__Wpa w;
	char out[512];
	time_t end;
	int res = 0;
	if (netman_wifi__wpa_open(&w, ifname)) { fputs("wifi: cannot talk to wpa_supplicant (needs root or the netdev group)\n", NETMAN_ERR); return 1; }
	if (netman_wifi__wpa_req(&w, "ATTACH", out, sizeof out) || strncmp(out, "OK", 2)) { fputs("wifi: wpa_supplicant refused ATTACH\n", NETMAN_ERR); netman_wifi__wpa_close(&w); return 1; }
	if (netman_wifi__wpa_req(&w, cmd, out, sizeof out) || !strncmp(out, "FAIL", 4)) {
		fprintf(NETMAN_ERR, "wifi: wpa_supplicant refused %s\n", cmd);
		netman_wifi__wpa_close(&w);
		return 1;
	}
	if (out[0] && strncmp(out, "OK", 2) && out[strspn(out, "0123456789")] == 0) fprintf(NETMAN_OUT, "PIN: %s\n", out);
	fprintf(NETMAN_OUT, "WPS started, press the button / enter the PIN on the router (up to %d s)\n", netman_wifi__wps_secs());
	end = time(NULL) + netman_wifi__wps_secs();
	while (!res && time(NULL) < end) {
		struct pollfd p = { w.fd, POLLIN, 0 };
		ssize_t r;
		if (poll(&p, 1, 1000) <= 0 || (r = recv(w.fd, out, sizeof out - 1, 0)) <= 0) continue;
		out[r] = 0;
		out[strcspn(out, "\n")] = 0;
		res = netman_wifi__wps_event(out);
		if (strstr(out, "WPS-")) fprintf(NETMAN_OUT, "%s\n", out[0] == '<' && strchr(out, '>') ? strchr(out, '>') + 1 : out);
	}
	netman_wifi__wpa_req(&w, "DETACH", out, sizeof out);
	netman_wifi__wpa_close(&w);
	if (res > 0) { netman_wifi__wpa_cmd(ifname, "SAVE_CONFIG", out, sizeof out); netman_dbus_puts("WPS done (check: netman status)"); return 0; }
	fputs(res < 0 ? "wifi: WPS failed\n" : "wifi: WPS timed out\n", NETMAN_ERR);
	return 1;
}

static int netman_wifi__wpa_set(const char *ifname, const char *id, const char *key, const char *val)
{
	char cmd[400], out[128];
	snprintf(cmd, sizeof cmd, "SET_NETWORK %s %s %s", id, key, val);
	return netman_wifi__wpa_cmd(ifname, cmd, out, sizeof out) || strncmp(out, "OK", 2);
}

static int netman_wifi__wpa_hotspot_start(const char *ifname, const char *ssid, const char *pass)
{
	char out[256], id[16], q[200];
	const char *freq = getenv("NETMAN_AP_FREQ");
	if (!netman_wifi__wpa_safe(ssid) || (pass && !netman_wifi__wpa_safe(pass))) { fputs("wifi: quotes, backslashes and newlines are not allowed in SSID or passphrase\n", NETMAN_ERR); return 1; }
	if (pass && (strlen(pass) < 8 || strlen(pass) > 63)) { fputs("wifi: hotspot passphrase must be 8-63 characters\n", NETMAN_ERR); return 1; }
	if (netman_wifi__wpa_cmd(ifname, "ADD_NETWORK", out, sizeof out)) { fputs("wifi: cannot talk to wpa_supplicant\n", NETMAN_ERR); return 1; }
	snprintf(id, sizeof id, "%d", atoi(out));
	snprintf(q, sizeof q, "\"%s\"", ssid);
	if (netman_wifi__wpa_set(ifname, id, "ssid", q) || netman_wifi__wpa_set(ifname, id, "mode", "2") || netman_wifi__wpa_set(ifname, id, "frequency", freq && *freq ? freq : "2437") ||
	    netman_wifi__wpa_set(ifname, id, "id_str", "\"nmb-ap\"")) goto bad;
	if (pass) {
		snprintf(q, sizeof q, "\"%s\"", pass);
		if (netman_wifi__wpa_set(ifname, id, "key_mgmt", "WPA-PSK") || netman_wifi__wpa_set(ifname, id, "proto", "RSN") || netman_wifi__wpa_set(ifname, id, "pairwise", "CCMP") ||
		    netman_wifi__wpa_set(ifname, id, "group", "CCMP") || netman_wifi__wpa_set(ifname, id, "psk", q)) goto bad;
	} else if (netman_wifi__wpa_set(ifname, id, "key_mgmt", "NONE")) goto bad;
	snprintf(q, sizeof q, "SELECT_NETWORK %s", id);
	if (netman_wifi__wpa_cmd(ifname, q, out, sizeof out) || strncmp(out, "OK", 2)) goto bad;
	fprintf(NETMAN_OUT, "hotspot %s started (no DHCP/NAT here: give the interface an address and share the uplink yourself)\n", ssid);
	return 0;
bad:
	snprintf(q, sizeof q, "REMOVE_NETWORK %s", id);
	netman_wifi__wpa_cmd(ifname, q, out, sizeof out);
	fputs("wifi: wpa_supplicant refused the hotspot settings (does the driver support AP mode?)\n", NETMAN_ERR);
	return 1;
}

/* ADD_NETWORK + SET_NETWORK + SELECT_NETWORK, then wait for the verdict on an event socket. ident != NULL: 802.1X (PEAP / MSCHAPV2). */
static int netman_wifi__wpa_connect(const char *ifname, const char *ssid, const char *pass, const char *ident)
{
	static char out[16384];
	char cmd[400], id[16], esc[200];
	netman_wifi__Wpa w;
	time_t end;
	int verdict = 0, attached = 0, secs;
	const char *e = getenv("NETMAN_CONNECT_SECS");
	secs = e && atoi(e) > 0 ? atoi(e) : 25;
	netman_wifi_escape(ssid, strlen(ssid), esc, sizeof esc);
	if (!netman_wifi__wpa_safe(ssid) || (pass && !netman_wifi__wpa_safe(pass)) || (ident && !netman_wifi__wpa_safe(ident))) { netman_wifi__seterr("quotes, backslashes and newlines are not allowed in SSID, identity or passphrase", 0); return 1; }
	if (pass && !ident && (strlen(pass) < 8 || strlen(pass) > 63)) { netman_wifi__seterr("a WPA passphrase is 8-63 characters", 1); return 1; }
	if (netman_wifi__wpa_cmd(ifname, "ADD_NETWORK", out, sizeof out)) { netman_wifi__seterr("cannot talk to wpa_supplicant", 0); return 1; }
	snprintf(id, sizeof id, "%d", atoi(out));
	snprintf(cmd, sizeof cmd, "SET_NETWORK %s ssid \"%s\"", id, ssid); netman_wifi__wpa_cmd(ifname, cmd, out, sizeof out);
	if (ident) {
		snprintf(cmd, sizeof cmd, "SET_NETWORK %s key_mgmt WPA-EAP", id); netman_wifi__wpa_cmd(ifname, cmd, out, sizeof out);
		snprintf(cmd, sizeof cmd, "SET_NETWORK %s eap PEAP", id); netman_wifi__wpa_cmd(ifname, cmd, out, sizeof out);
		snprintf(cmd, sizeof cmd, "SET_NETWORK %s phase2 \"auth=MSCHAPV2\"", id); netman_wifi__wpa_cmd(ifname, cmd, out, sizeof out);
		snprintf(cmd, sizeof cmd, "SET_NETWORK %s identity \"%s\"", id, ident); netman_wifi__wpa_cmd(ifname, cmd, out, sizeof out);
		snprintf(cmd, sizeof cmd, "SET_NETWORK %s password \"%s\"", id, pass ? pass : ""); netman_wifi__wpa_cmd(ifname, cmd, out, sizeof out);
	} else if (pass) snprintf(cmd, sizeof cmd, "SET_NETWORK %s psk \"%s\"", id, pass), netman_wifi__wpa_cmd(ifname, cmd, out, sizeof out);
	else snprintf(cmd, sizeof cmd, "SET_NETWORK %s key_mgmt NONE", id), netman_wifi__wpa_cmd(ifname, cmd, out, sizeof out);
	if (strncmp(out, "OK", 2)) {
		char m[300];
		snprintf(m, sizeof m, "wpa_supplicant refused the settings (%.100s)", out);
		netman_wifi__seterr(m, 0);
		snprintf(cmd, sizeof cmd, "REMOVE_NETWORK %s", id); netman_wifi__wpa_cmd(ifname, cmd, out, sizeof out);
		return 1;
	}
	if (!netman_wifi__wpa_open(&w, ifname)) { attached = !netman_wifi__wpa_req(&w, "ATTACH", out, sizeof out) && !strncmp(out, "OK", 2); }
	if (attached) { snprintf(cmd, sizeof cmd, "SELECT_NETWORK %s", id); netman_wifi__wpa_req(&w, cmd, out, sizeof out); }
	else { snprintf(cmd, sizeof cmd, "SELECT_NETWORK %s", id); netman_wifi__wpa_cmd(ifname, cmd, out, sizeof out); }
	end = time(NULL) + secs;
	while (attached && !verdict && time(NULL) < end) {
		struct pollfd p = { w.fd, POLLIN, 0 };
		ssize_t r;
		const char *ev;
		if (poll(&p, 1, 500) <= 0 || (r = recv(w.fd, out, sizeof out - 1, 0)) <= 0) continue;
		out[r] = 0;
		ev = out[0] == '<' && strchr(out, '>') ? strchr(out, '>') + 1 : out;
		if (!strncmp(ev, "CTRL-EVENT-CONNECTED", 20)) verdict = 1;
		else if (!strncmp(ev, "CTRL-EVENT-SSID-TEMP-DISABLED", 29) && strstr(ev, "WRONG_KEY")) verdict = -1;
		else if (!strncmp(ev, "CTRL-EVENT-SSID-TEMP-DISABLED", 29) && strstr(ev, "reason=CONN_FAILED")) verdict = -2;
		else if (!strncmp(ev, "CTRL-EVENT-EAP-FAILURE", 22) || !strncmp(ev, "CTRL-EVENT-SSID-TEMP-DISABLED", 29)) verdict = -3;
		else if (!strncmp(ev, "CTRL-EVENT-NETWORK-NOT-FOUND", 28) && time(NULL) + secs - 3 > end) verdict = -4;
	}
	if (attached) { netman_wifi__wpa_req(&w, "DETACH", out, sizeof out); }
	if (attached || w.fd > 0) netman_wifi__wpa_close(&w);
	if (verdict > 0) {
		netman_wifi__wpa_cmd(ifname, "SAVE_CONFIG", out, sizeof out);
		fprintf(NETMAN_OUT, "connected to %s\n", esc);
		return 0;
	}
	if (verdict < 0) {
		char m[500];
		snprintf(cmd, sizeof cmd, "REMOVE_NETWORK %s", id); netman_wifi__wpa_cmd(ifname, cmd, out, sizeof out);
		snprintf(m, sizeof m, verdict == -1 ? "wrong passphrase for %s" : verdict == -3 && ident ? "login refused for %s (wrong identity or password)" : verdict == -4 ? "%s is not in range" : "could not join %s", esc);
		netman_wifi__seterr(m, verdict == -1 || (verdict == -3 && ident != NULL));
		return 1;
	}
	netman_wifi__wpa_cmd(ifname, "SAVE_CONFIG", out, sizeof out);
	fprintf(NETMAN_OUT, "selected %s (no answer within %d s; check: netman status)\n", esc, secs);
	return 0;
}

/* ids from LIST_NETWORKS ("id\tssid\tbssid\tflags"); calls f(id, ssid, flags, arg) for each */
static int netman_wifi__wpa_each_net(const char *ifname, void (*f)(const char *id, const char *ssid, const char *flags, void *arg), void *arg)
{
	static char out[8192];
	if (netman_wifi__wpa_cmd(ifname, "LIST_NETWORKS", out, sizeof out)) { fputs("wifi: cannot talk to wpa_supplicant (needs root or the netdev group)\n", NETMAN_ERR); return 1; }
	for (char *l = strtok(out, "\n"); l; l = strtok(NULL, "\n")) {
		char *fl[4] = {0}, *x = l;
		int k = 0;
		for (; k < 4; k++) { fl[k] = x; if (!(x = strchr(x, '\t'))) break; *x++ = 0; }
		if (k < 1 || !strncmp(fl[0], "network id", 10)) continue;
		f(fl[0], fl[1] ? fl[1] : "", fl[3] ? fl[3] : "", arg);
	}
	return 0;
}

static void netman_wifi__print_net(const char *id, const char *ssid, const char *flags, void *arg)
{
	(void)id; (void)arg;
	fprintf(NETMAN_OUT, "  %-32s autoconnect=%s\n", ssid, strstr(flags, "DISABLED") ? "off" : "on");
}
struct netman_wifi__find { const char *ifname, *ssid, *action; int hit; };
static void netman_wifi__ctl_net(const char *id, const char *ssid, const char *flags, void *arg)
{
	struct netman_wifi__find *f = arg;
	char cmd[64], out[64];
	(void)flags;
	if (f->ssid && strcmp(f->ssid, ssid)) return;
	snprintf(cmd, sizeof cmd, "%s %s", f->action, id);
	f->hit += !netman_wifi__wpa_cmd(f->ifname, cmd, out, sizeof out) && !strncmp(out, "OK", 2);
}
static void netman_wifi__ap_net(const char *id, const char *ssid, const char *flags, void *arg)
{
	struct netman_wifi__find *f = arg;
	char cmd[64], out[64];
	(void)ssid; (void)flags;
	snprintf(cmd, sizeof cmd, "GET_NETWORK %s id_str", id);
	if (netman_wifi__wpa_cmd(f->ifname, cmd, out, sizeof out) || !strstr(out, "nmb-ap")) return;
	snprintf(cmd, sizeof cmd, "REMOVE_NETWORK %s", id);
	f->hit += !netman_wifi__wpa_cmd(f->ifname, cmd, out, sizeof out) && !strncmp(out, "OK", 2);
}

/* ---- iwd: WPS, access point, known networks ---------------------------------------------------------------------- */

static const char *netman_wifi__iwd_wsc(const char **iface)
{
	static const char *names[] = { netman_wifi__IWD ".SimpleConfiguration", netman_wifi__IWD ".WiFiSimpleConfiguration" };
	for (int i = 0; i < 2; i++) { const char *p = netman_dbus_first_path(names[i]); if (p) { *iface = names[i]; return p; } }
	return NULL;
}

static int netman_wifi__iwd_wps(int pbc, const char *pin, int cancel)
{
	const char *st, *wsc, *iface;
	char gen[32] = "";
	int fd = netman_wifi__iwd_open(&st), rc;
	if (fd < 0) return 1;
	if (!(wsc = netman_wifi__iwd_wsc(&iface))) { fputs("wifi: iwd offers no WPS on this device\n", NETMAN_ERR); close(fd); return 1; }
	netman_dbus_call_timeout = netman_wifi__wps_secs() * 1000 + 10000;
	if (cancel) rc = netman_dbus_simple_call(fd, netman_wifi__IWD, wsc, iface, "Cancel", NULL, NULL, "wps cancel");
	else if (pbc) {
		netman_dbus_puts("WPS started, press the button on the router");
		rc = netman_dbus_simple_call(fd, netman_wifi__IWD, wsc, iface, "PushButton", NULL, NULL, "wps");
	} else {
		netman_dbus_Buf b = {0};
		if (!pin) {
			netman_dbus_Msg m;
			if (netman_dbus_call(fd, &m, netman_wifi__IWD, wsc, iface, "GeneratePin", NULL, NULL)) { close(fd); return netman_dbus_fail(&m, "wps pin"); }
			{ netman_dbus_R r = { m.body, 0, m.blen, 0 }; snprintf(gen, sizeof gen, "%s", netman_dbus_r_str(&r)); }
			pin = gen;
			fprintf(NETMAN_OUT, "PIN: %s\n", gen);
		}
		netman_dbus_put_str(&b, pin);
		fprintf(NETMAN_OUT, "WPS started, enter the PIN on the router (up to %d s)\n", netman_wifi__wps_secs());
		rc = netman_dbus_simple_call(fd, netman_wifi__IWD, wsc, iface, "StartPin", "s", &b, "wps");
		free(b.b);
	}
	if (!rc && !cancel) netman_dbus_puts("WPS done (check: netman status)");
	close(fd);
	return rc;
}

#define netman_wifi__AP netman_wifi__IWD ".AccessPoint"

static int netman_wifi__iwd_hotspot(const char *ssid, const char *pass, int stop)
{
	const char *dev;
	int fd = netman_dbus_system_bus(), rc = 0;
	netman_dbus_Buf b = {0};
	if (fd < 0) return 1;
	signal(SIGPIPE, SIG_IGN);
	if (netman_wifi__iwd_load(fd)) { close(fd); return 1; }
	if (!(dev = netman_dbus_first_path(netman_wifi__IWD ".Device"))) { fputs("wifi: iwd has no wifi device\n", NETMAN_ERR); close(fd); return 1; }
	if (stop) {
		const char *ap = netman_dbus_first_path(netman_wifi__AP);
		if (!ap) { fputs("wifi: no hotspot is running\n", NETMAN_ERR); close(fd); return 1; }
		rc = netman_dbus_simple_call(fd, netman_wifi__IWD, ap, netman_wifi__AP, "Stop", NULL, NULL, "hotspot stop");
		if (!rc) netman_dbus_set_var(fd, netman_wifi__IWD, ap, netman_wifi__IWD ".Device", "Mode", 0, "station");
		if (!rc) netman_dbus_puts("hotspot stopped");
		close(fd);
		return rc;
	}
	if (!pass || strlen(pass) < 8 || strlen(pass) > 63) { fputs("wifi: iwd hotspot needs a passphrase of 8-63 characters\n", NETMAN_ERR); close(fd); return 1; }
	if (!netman_dbus_pget(dev, netman_wifi__AP, "")) {
		if (netman_dbus_set_var(fd, netman_wifi__IWD, dev, netman_wifi__IWD ".Device", "Mode", 0, "ap")) { close(fd); return 1; }
		for (int i = 0; i < 10 && !netman_dbus_pget(dev, netman_wifi__AP, ""); i++) {
			struct timespec ts = { 0, 300000000 };
			nanosleep(&ts, NULL);
			if (netman_wifi__iwd_load(fd)) { close(fd); return 1; }
		}
		if (!netman_dbus_pget(dev, netman_wifi__AP, "")) { fputs("wifi: the device did not switch to AP mode\n", NETMAN_ERR); close(fd); return 1; }
	}
	netman_dbus_put_str(&b, ssid); netman_dbus_put_str(&b, pass);
	rc = netman_dbus_simple_call(fd, netman_wifi__IWD, dev, netman_wifi__AP, "Start", "ss", &b, "hotspot start");
	free(b.b);
	if (!rc) fprintf(NETMAN_OUT, "hotspot %s started (no DHCP/NAT here: iwd's EnableNetworkConfiguration or your own dnsmasq/nft)\n", ssid);
	close(fd);
	return rc;
}

static int netman_wifi__iwd_known(const char *ssid, int on)
{
	const char *st, *k = NULL;
	int fd = netman_wifi__iwd_open(&st), rc = 0, any = 0;
	if (fd < 0) return 1;
	for (int i = 0; i < netman_dbus_nprops; i++) {
		const char *p = netman_dbus_props[i].path, *n;
		if (strcmp(netman_dbus_props[i].iface, netman_wifi__IWD ".KnownNetwork") || *netman_dbus_props[i].key) continue;
		n = netman_dbus_pget(p, netman_wifi__IWD ".KnownNetwork", "Name");
		if (ssid) { if (n && !strcmp(n, ssid)) k = p; continue; }
		fprintf(NETMAN_OUT, "  %-32s %-6s autoconnect=%s\n", n ?: "?", netman_dbus_pget(p, netman_wifi__IWD ".KnownNetwork", "Type") ?: "?",
		       !strcmp(netman_dbus_pget(p, netman_wifi__IWD ".KnownNetwork", "AutoConnect") ?: "", "true") ? "on" : "off");
		any = 1;
	}
	if (ssid) {
		if (!k) { fputs("wifi: that network is not known\n", NETMAN_ERR); rc = 1; }
		else rc = netman_dbus_set_var(fd, netman_wifi__IWD, k, netman_wifi__IWD ".KnownNetwork", "AutoConnect", on, NULL);
	} else if (!any) netman_dbus_puts("no saved networks");
	close(fd);
	return rc;
}


int netman_wifi_backend_is_wpa(void)
{
	if (netman_wifi__force) return netman_wifi__force == 2;
	return netman_wifi__wpa_present(netman_wifi__wpa_iface());
}

static int netman_wifi__connect(const char *ssid, const char *pass, const char *ident)
{
	const char *st;
	int fd, rc;
	netman_wifi__lasterr[0] = 0; netman_wifi__wrongpw = 0;
	if (!ssid || !*ssid) { netman_wifi__seterr("connect needs a network name", 0); return 1; }
	if (netman_wifi_backend_is_wpa() && netman_wifi__wpa_present(netman_wifi__wpa_iface())) return netman_wifi__wpa_connect(netman_wifi__wpa_iface(), ssid, pass, ident);
	if ((fd = netman_wifi__iwd_open(&st)) < 0) return 1;
	rc = netman_wifi__iwd_connect(fd, st, ssid, pass, ident);
	close(fd);
	return rc;
}
int netman_wifi_connect(const char *ssid, const char *pass) { return netman_wifi__connect(ssid, pass, NULL); }
int netman_wifi_connect_eap(const char *ssid, const char *identity, const char *pass) { return netman_wifi__connect(ssid, pass ? pass : "", identity ? identity : ""); }

int netman_wifi_disconnect(void)
{
	const char *st;
	int fd, rc;
	if (netman_wifi__wpa_present(netman_wifi__wpa_iface())) { char out[256]; netman_wifi__wpa_cmd(netman_wifi__wpa_iface(), "DISCONNECT", out, sizeof out); return 0; }
	if ((fd = netman_wifi__iwd_open(&st)) < 0) return 1;
	rc = netman_dbus_simple_call(fd, netman_wifi__IWD, st, netman_wifi__IWD ".Station", "Disconnect", NULL, NULL, "disconnect");
	close(fd);
	return rc;
}

int netman_wifi_forget(const char *ssid)
{
	const char *st;
	int fd, rc;
	if (netman_wifi__wpa_present(netman_wifi__wpa_iface())) return 2;   /* wpa_supplicant backend has no forget */
	if ((fd = netman_wifi__iwd_open(&st)) < 0) return 1;
	rc = netman_wifi__iwd_forget(fd, ssid);
	close(fd);
	return rc;
}

int netman_wifi_wps_pbc(void)
{
	if (netman_wifi__wpa_present(netman_wifi__wpa_iface())) return netman_wifi__wpa_wps(netman_wifi__wpa_iface(), "WPS_PBC");
	return netman_wifi__iwd_wps(1, NULL, 0);
}

int netman_wifi_wps_pin(const char *pin)
{
	char cmd[64];
	if (pin) {
		size_t n = strlen(pin);
		if ((n != 4 && n != 8) || pin[strspn(pin, "0123456789")]) { fputs("wifi: a WPS PIN is 4 or 8 digits\n", NETMAN_ERR); return 1; }
	}
	if (!netman_wifi__wpa_present(netman_wifi__wpa_iface())) return netman_wifi__iwd_wps(0, pin, 0);
	snprintf(cmd, sizeof cmd, pin ? "WPS_PIN any %s" : "WPS_PIN any", pin);
	return netman_wifi__wpa_wps(netman_wifi__wpa_iface(), cmd);
}

int netman_wifi_wps_cancel(void)
{
	char out[64];
	if (!netman_wifi__wpa_present(netman_wifi__wpa_iface())) return netman_wifi__iwd_wps(0, NULL, 1);
	if (netman_wifi__wpa_cmd(netman_wifi__wpa_iface(), "WPS_CANCEL", out, sizeof out) || strncmp(out, "OK", 2)) { fputs("wifi: WPS_CANCEL failed\n", NETMAN_ERR); return 1; }
	return 0;
}

int netman_wifi_hotspot_start(const char *ssid, const char *pass)
{
	if (!ssid || !*ssid) { fputs("wifi: hotspot needs an SSID\n", NETMAN_ERR); return 1; }
	if (netman_wifi__wpa_present(netman_wifi__wpa_iface())) return netman_wifi__wpa_hotspot_start(netman_wifi__wpa_iface(), ssid, pass);
	return netman_wifi__iwd_hotspot(ssid, pass, 0);
}

int netman_wifi_hotspot_stop(void)
{
	if (netman_wifi__wpa_present(netman_wifi__wpa_iface())) {
		struct netman_wifi__find f = { netman_wifi__wpa_iface(), NULL, NULL, 0 };
		if (netman_wifi__wpa_each_net(f.ifname, netman_wifi__ap_net, &f)) return 1;
		if (!f.hit) { fputs("wifi: no hotspot is running\n", NETMAN_ERR); return 1; }
		netman_dbus_puts("hotspot stopped");
		return 0;
	}
	return netman_wifi__iwd_hotspot(NULL, NULL, 1);
}

int netman_wifi_known(void)
{
	if (netman_wifi__wpa_present(netman_wifi__wpa_iface())) return netman_wifi__wpa_each_net(netman_wifi__wpa_iface(), netman_wifi__print_net, NULL);
	return netman_wifi__iwd_known(NULL, 0);
}

int netman_wifi_autoconnect(const char *ssid, int on)
{
	if (netman_wifi__wpa_present(netman_wifi__wpa_iface())) {
		struct netman_wifi__find f = { netman_wifi__wpa_iface(), ssid, on ? "ENABLE_NETWORK" : "DISABLE_NETWORK", 0 };
		char out[64];
		if (netman_wifi__wpa_each_net(f.ifname, netman_wifi__ctl_net, &f)) return 1;
		if (!f.hit) { fputs("wifi: that network is not known\n", NETMAN_ERR); return 1; }
		netman_wifi__wpa_cmd(f.ifname, "SAVE_CONFIG", out, sizeof out);
		return 0;
	}
	return netman_wifi__iwd_known(ssid, on);
}

int netman_wifi_status(void)
{
	int fd;
	if (netman_wifi__force != 2 && (fd = netman_dbus_system_bus()) >= 0) {
		if (!netman_dbus_load_objects(fd, netman_wifi__IWD)) {
			const char *st = netman_dbus_first_path(netman_wifi__IWD ".Station"), *c = st ? netman_dbus_pget(st, netman_wifi__IWD ".Station", "ConnectedNetwork") : NULL;
			if (st) fprintf(NETMAN_OUT, "wifi     %s%s%s\n", netman_dbus_pget(st, netman_wifi__IWD ".Station", "State") ?: "?", c ? " to " : "", c ? netman_dbus_pget(c, netman_wifi__IWD ".Network", "Name") ?: "" : "");
		} else netman_dbus_puts("wifi     iwd not running");
		close(fd);
	}
	if (netman_wifi__force == 2 || netman_wifi__wpa_present(netman_wifi__wpa_iface())) fprintf(NETMAN_OUT, "wifi     %s is driven by wpa_supplicant (not iwd)\n", netman_wifi__wpa_iface());
	return 0;
}


/* ---- listing with hidden networks, security tags, details ---------------------------------------------------------- */


/* length of the valid UTF-8 sequence at s (1..4), 0 when invalid or a control character */
static int netman_wifi__u8(const unsigned char *s, size_t n)
{
	if (!n) return 0;
	if (s[0] >= 0x20 && s[0] < 0x7f) return 1;
	if (s[0] < 0x80) return 0;
	if (s[0] >= 0xc2 && s[0] <= 0xdf && n >= 2 && (s[1] & 0xc0) == 0x80) return 2;
	if (s[0] >= 0xe0 && s[0] <= 0xef && n >= 3 && (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80) {
		unsigned c = ((unsigned)(s[0] & 15) << 12) | ((unsigned)(s[1] & 63) << 6) | (s[2] & 63);
		if (c < 0x800 || (c >= 0xd800 && c < 0xe000) || (c >= 0x80 && c < 0xa0)) return 0;
		return 3;
	}
	if (s[0] >= 0xf0 && s[0] <= 0xf4 && n >= 4 && (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80 && (s[3] & 0xc0) == 0x80) {
		unsigned c = ((unsigned)(s[0] & 7) << 18) | ((unsigned)(s[1] & 63) << 12) | ((unsigned)(s[2] & 63) << 6) | (s[3] & 63);
		return c >= 0x10000 && c <= 0x10ffff ? 4 : 0;
	}
	return 0;
}

size_t netman_wifi_escape(const char *raw, size_t n, char *out, size_t on)
{
	size_t o = 0, i = 0;
	const unsigned char *s = (const unsigned char *)raw;
	if (!on) return 0;
	while (i < n && o + 5 < on) {
		int l = s[i] == '\\' ? 0 : netman_wifi__u8(s + i, n - i);
		if (l) { memcpy(out + o, s + i, (size_t)l); o += (size_t)l; i += (size_t)l; }
		else if (s[i] == '\\') { out[o++] = '\\'; out[o++] = '\\'; i++; }
		else if (s[i] == '\t') { out[o++] = '\\'; out[o++] = 't'; i++; }
		else if (s[i] == '\n') { out[o++] = '\\'; out[o++] = 'n'; i++; }
		else { o += (size_t)snprintf(out + o, 5, "\\x%02x", s[i]); i++; }
	}
	out[o] = 0;
	return o;
}

size_t netman_wifi_unescape(const char *e, char *out, size_t on)
{
	size_t o = 0;
	if (!on) return 0;
	while (*e && o + 1 < on) {
		if (e[0] == '\\' && e[1] == '\\') { out[o++] = '\\'; e += 2; }
		else if (e[0] == '\\' && e[1] == 't') { out[o++] = '\t'; e += 2; }
		else if (e[0] == '\\' && e[1] == 'n') { out[o++] = '\n'; e += 2; }
		else if (e[0] == '\\' && e[1] == 'x' && strchr("0123456789abcdefABCDEF", e[2]) && e[2] && strchr("0123456789abcdefABCDEF", e[3]) && e[3]) {
			char h[3] = { e[2], e[3], 0 };
			out[o++] = (char)strtol(h, NULL, 16);
			e += 4;
		} else out[o++] = *e++;
	}
	out[o] = 0;
	return o;
}

/* wpa_supplicant prints SSIDs with printf_encode: \\ \" \e \n \r \t \xNN (all bytes > 126 as \xNN) */
static size_t netman_wifi__wpa_decode(const char *e, char *out, size_t on)
{
	size_t o = 0;
	while (*e && o + 1 < on) {
		if (e[0] != '\\') { out[o++] = *e++; continue; }
		switch (e[1]) {
		case '\\': out[o++] = '\\'; e += 2; break;
		case '"': out[o++] = '"'; e += 2; break;
		case 'e': out[o++] = 27; e += 2; break;
		case 'n': out[o++] = '\n'; e += 2; break;
		case 'r': out[o++] = '\r'; e += 2; break;
		case 't': out[o++] = '\t'; e += 2; break;
		case 'x': if (e[2] && e[3]) { char h[3] = { e[2], e[3], 0 }; out[o++] = (char)strtol(h, NULL, 16); e += 4; break; }   /* fallthrough */
		default: out[o++] = *e++;
		}
	}
	out[o] = 0;
	return o;
}

const char *netman_wifi_sec_iwd(const char *t)
{
	if (!t) return "?";
	if (!strcmp(t, "open")) return "OPEN";
	if (!strcmp(t, "psk")) return "WPA2";
	if (!strcmp(t, "8021x")) return "802.1X";
	if (!strcmp(t, "wep")) return "WEP";
	return "?";
}

const char *netman_wifi_sec_flags(const char *f)
{
	int eap, sae, wpa2, wpa1;
	if (!f) return "?";
	eap = strstr(f, "EAP") != NULL;
	sae = strstr(f, "SAE") != NULL;
	wpa2 = strstr(f, "WPA2-PSK") != NULL || strstr(f, "RSN-PSK") != NULL;
	wpa1 = strstr(f, "WPA-PSK") != NULL;
	if (eap) return "802.1X";
	if (sae && wpa2) return "WPA2/3";
	if (sae) return "WPA3";
	if (wpa2) return "WPA2";
	if (wpa1) return "WPA";
	if (strstr(f, "WEP")) return "WEP";
	if (strstr(f, "OWE")) return "OWE";
	return "OPEN";
}

int netman_wifi_pct(int dbm) { int p = 2 * (dbm + 100); return p < 0 ? 0 : p > 100 ? 100 : p; }

int netman_wifi_chan(int f, const char **band)
{
	const char *b = "";
	int c = 0;
	if (f == 2484) { c = 14; b = "2.4"; }
	else if (f >= 2412 && f <= 2472) { c = (f - 2407) / 5; b = "2.4"; }
	else if (f >= 5160 && f <= 5885) { c = (f - 5000) / 5; b = "5"; }
	else if (f >= 5955 && f <= 7115) { c = (f - 5950) / 5; b = "6"; }
	if (band) *band = b;
	return c;
}

typedef struct { int conn, known, dbm, pct, freq, nbss, hidden; char sec[8], bssid[20], name[200], raw[80], dev_path[160], chan[8]; } netman_wifi__Row;
#define NETMAN_WIFI_MAXROWS 160

static int netman_wifi__rowcmp(const void *a, const void *b)
{
	const netman_wifi__Row *x = a, *y = b;
	if (x->conn != y->conn) return y->conn - x->conn;
	return y->dbm - x->dbm;
}

/* ---- iwd rows / diagnostics ---- */

typedef struct { char k[32], v[100]; } netman_wifi__KV;
static netman_wifi__KV netman_wifi__diag[32];
static int netman_wifi__ndiag;
static const char *netman_wifi__dget(const char *k) { for (int i = 0; i < netman_wifi__ndiag; i++) if (!strcmp(netman_wifi__diag[i].k, k)) return netman_wifi__diag[i].v; return NULL; }

static int netman_wifi__iwd_diag(int fd, const char *st)
{
	netman_dbus_Msg m;
	netman_dbus_R r;
	uint32_t len;
	size_t end;
	netman_wifi__ndiag = 0;
	if (netman_dbus_call(fd, &m, netman_wifi__IWD, st, netman_wifi__IWD ".StationDiagnostic", "GetDiagnostics", NULL, NULL)) return -1;
	r = (netman_dbus_R){ m.body, 0, m.blen, 0 };
	len = netman_dbus_r_u32(&r); netman_dbus_ralign(&r, 8); end = r.pos + len;
	while (!r.bad && r.pos < end && netman_wifi__ndiag < 32) {
		netman_dbus_ralign(&r, 8);
		snprintf(netman_wifi__diag[netman_wifi__ndiag].k, sizeof netman_wifi__diag[0].k, "%s", netman_dbus_r_str(&r));
		netman_dbus_variant_str(&r, netman_wifi__diag[netman_wifi__ndiag].v, sizeof netman_wifi__diag[0].v);
		netman_wifi__ndiag++;
	}
	return r.bad ? -1 : 0;
}

static int netman_wifi__iwd_rows(int fd, const char *st, netman_wifi__Row *rows, int max)
{
	netman_dbus_Msg m;
	netman_dbus_R r;
	uint32_t len;
	size_t end;
	int n = 0;
	const char *conn = netman_dbus_pget(st, netman_wifi__IWD ".Station", "ConnectedNetwork");
	if (!netman_wifi_passive && netman_wifi__scan(fd, st)) return -1;
	conn = netman_dbus_pget(st, netman_wifi__IWD ".Station", "ConnectedNetwork");
	if (netman_dbus_call(fd, &m, netman_wifi__IWD, st, netman_wifi__IWD ".Station", "GetOrderedNetworks", NULL, NULL)) { netman_dbus_fail(&m, "list"); return -1; }
	r = (netman_dbus_R){ m.body, 0, m.blen, 0 };
	len = netman_dbus_r_u32(&r); netman_dbus_ralign(&r, 8); end = r.pos + len;
	while (!r.bad && r.pos < end && n < max) {
		char path[160];
		int16_t sig = 0;
		const char *name, *type;
		netman_wifi__Row *w = &rows[n];
		netman_dbus_ralign(&r, 8);
		snprintf(path, sizeof path, "%s", netman_dbus_r_str(&r));
		netman_dbus_ralign(&r, 2);
		if (r.pos + 2 <= r.len) memcpy(&sig, r.p + r.pos, 2);
		r.pos += 2;
		name = netman_dbus_pget(path, netman_wifi__IWD ".Network", "Name");
		type = netman_dbus_pget(path, netman_wifi__IWD ".Network", "Type");
		memset(w, 0, sizeof *w);
		w->conn = conn && !strcmp(conn, path);
		w->known = netman_dbus_pget(path, netman_wifi__IWD ".Network", "KnownNetwork") != NULL;
		w->dbm = sig / 100; w->pct = netman_wifi_pct(w->dbm); w->nbss = 1;
		snprintf(w->sec, sizeof w->sec, "%s", netman_wifi_sec_iwd(type));
		snprintf(w->bssid, sizeof w->bssid, "-");
		netman_wifi_escape(name ? name : "", name ? strlen(name) : 0, w->name, sizeof w->name);
		w->hidden = !name || !*name || !name[strspn(name, " ")];
		snprintf(w->dev_path, sizeof w->dev_path, "%s", path);
		n++;
	}
	/* hidden access points never become Network objects until you connect by name: they come from their own call */
	if (!netman_dbus_call(fd, &m, netman_wifi__IWD, st, netman_wifi__IWD ".Station", "GetHiddenAccessPoints", NULL, NULL) && m.type == 2) {
		r = (netman_dbus_R){ m.body, 0, m.blen, 0 };
		len = netman_dbus_r_u32(&r); netman_dbus_ralign(&r, 8); end = r.pos + len;
		while (!r.bad && r.pos < end && n < max) {
			netman_wifi__Row *w = &rows[n];
			const char *addr, *type;
			int16_t sig = 0;
			netman_dbus_ralign(&r, 8);
			addr = netman_dbus_r_str(&r);
			snprintf(w->bssid, sizeof w->bssid, "%s", addr);
			netman_dbus_ralign(&r, 2);
			if (r.pos + 2 <= r.len) memcpy(&sig, r.p + r.pos, 2);
			r.pos += 2;
			type = netman_dbus_r_str(&r);
			{ char b[20]; snprintf(b, sizeof b, "%s", w->bssid); memset(w, 0, sizeof *w); snprintf(w->bssid, sizeof w->bssid, "%s", b); }
			w->hidden = 1; w->nbss = 1; w->dbm = sig / 100; w->pct = netman_wifi_pct(w->dbm);
			snprintf(w->sec, sizeof w->sec, "%s", netman_wifi_sec_iwd(type));
			n++;
		}
	}
	/* the connected network: frequency and BSSID from the diagnostics */
	for (int i = 0; i < n; i++)
		if (rows[i].conn && !netman_wifi__iwd_diag(fd, st)) {
			const char *f = netman_wifi__dget("Frequency"), *b = netman_wifi__dget("ConnectedBss");
			if (f) rows[i].freq = atoi(f);
			if (b && *b) snprintf(rows[i].bssid, sizeof rows[i].bssid, "%s", b);
		}
	qsort(rows, (size_t)n, sizeof *rows, netman_wifi__rowcmp);
	return n;
}

/* ---- wpa_supplicant rows ---- */

static int netman_wifi__wpa_status(const char *ifname, char *out, size_t n)
{
	if (netman_wifi__wpa_cmd(ifname, "STATUS", out, n)) return -1;
	return 0;
}
static const char *netman_wifi__kv_get(char *text, const char *key, char *dst, size_t n)
{
	size_t kl = strlen(key);
	for (char *p = text; p && *p; ) {
		char *e = strchr(p, '\n');
		if (!strncmp(p, key, kl) && p[kl] == '=') {
			size_t l = e ? (size_t)(e - p) - kl - 1 : strlen(p + kl + 1);
			if (l >= n) l = n - 1;
			memcpy(dst, p + kl + 1, l); dst[l] = 0;
			return dst;
		}
		p = e ? e + 1 : NULL;
	}
	dst[0] = 0;
	return NULL;
}

struct netman_wifi__knownset { char ssid[40][64]; int n; };
static void netman_wifi__collect_known(const char *id, const char *ssid, const char *flags, void *arg)
{
	struct netman_wifi__knownset *k = arg;
	(void)id; (void)flags;
	if (k->n < 40) { netman_wifi__wpa_decode(ssid, k->ssid[k->n], sizeof k->ssid[0]); k->n++; }
}

static int netman_wifi__wpa_rows(const char *ifname, netman_wifi__Row *rows, int max)
{
	static char out[32768], st[2048];
	static struct netman_wifi__knownset ks;
	int n = 0;
	char cbss[20] = "", cssid[64] = "", tmp[96];
	if (!netman_wifi_passive) {
		netman_wifi__wpa_cmd(ifname, "SCAN", out, sizeof out);
		{ const char *e = getenv("NETMAN_SCAN_SECS"); sleep((unsigned)(e && *e ? atoi(e) : 4)); }
	}
	if (netman_wifi__wpa_cmd(ifname, "SCAN_RESULTS", out, sizeof out)) { netman_wifi__seterr("cannot talk to wpa_supplicant (needs root or the netdev group)", 0); return -1; }
	if (!netman_wifi__wpa_status(ifname, st, sizeof st)) {
		if (netman_wifi__kv_get(st, "wpa_state", tmp, sizeof tmp) && !strcmp(tmp, "COMPLETED")) {
			netman_wifi__kv_get(st, "bssid", cbss, sizeof cbss);
			if (netman_wifi__kv_get(st, "ssid", tmp, sizeof tmp)) netman_wifi__wpa_decode(tmp, cssid, sizeof cssid);
		}
	}
	ks.n = 0;
	netman_wifi__wpa_each_net(ifname, netman_wifi__collect_known, &ks);
	for (char *l = strtok(out, "\n"); l && n < max; l = strtok(NULL, "\n")) {   /* bssid \t freq \t signal \t flags \t ssid */
		char *f[5] = {0}, *x = l, raw[80];
		netman_wifi__Row *w;
		int k = 0, found = -1, nraw;
		for (; k < 5; k++) { f[k] = x; if (!(x = strchr(x, '\t'))) break; *x++ = 0; }
		if (k < 3 || !strncmp(f[0], "bssid", 5)) continue;
		nraw = (int)netman_wifi__wpa_decode(f[4] ? f[4] : "", raw, sizeof raw);
		{ int blank = 1; for (int i = 0; i < nraw; i++) if (raw[i] && raw[i] != ' ') blank = 0; if (blank) nraw = 0; }
		if (nraw) for (int i = 0; i < n; i++) if (!rows[i].hidden && !strcmp(rows[i].raw, raw)) { found = i; break; }
		if (found >= 0) {   /* another access point of the same network: keep the strongest, count the rest */
			w = &rows[found];
			w->nbss++;
			if (atoi(f[2]) > w->dbm) { w->dbm = atoi(f[2]); w->pct = netman_wifi_pct(w->dbm); w->freq = atoi(f[1]); snprintf(w->bssid, sizeof w->bssid, "%s", f[0]); }
			if (!strcmp(f[0], cbss)) { w->conn = 1; snprintf(w->bssid, sizeof w->bssid, "%s", f[0]); w->freq = atoi(f[1]); }
			continue;
		}
		w = &rows[n++];
		memset(w, 0, sizeof *w);
		w->nbss = 1; w->dbm = atoi(f[2]); w->pct = netman_wifi_pct(w->dbm); w->freq = atoi(f[1]);
		snprintf(w->bssid, sizeof w->bssid, "%s", f[0]);
		snprintf(w->sec, sizeof w->sec, "%s", netman_wifi_sec_flags(f[3]));
		w->hidden = nraw == 0;
		if (!w->hidden) {
			snprintf(w->raw, sizeof w->raw, "%s", raw);
			netman_wifi_escape(raw, (size_t)nraw, w->name, sizeof w->name);
			w->conn = (cbss[0] && !strcmp(f[0], cbss)) || (cssid[0] && !strcmp(cssid, raw) && !cbss[0]);
			for (int i = 0; i < ks.n; i++) if (!strcmp(ks.ssid[i], raw)) w->known = 1;
		} else w->conn = cbss[0] && !strcmp(f[0], cbss);
	}
	qsort(rows, (size_t)n, sizeof *rows, netman_wifi__rowcmp);
	return n;
}

static int netman_wifi__rows(netman_wifi__Row *rows, int max)
{
	const char *st;
	int fd, n;
	if (netman_wifi_backend_is_wpa() && netman_wifi__wpa_present(netman_wifi__wpa_iface())) return netman_wifi__wpa_rows(netman_wifi__wpa_iface(), rows, max);
	if ((fd = netman_wifi__iwd_open(&st)) < 0) return -1;
	n = netman_wifi__iwd_rows(fd, st, rows, max);
	close(fd);
	return n;
}

int netman_wifi_scan(void)
{
	static netman_wifi__Row rows[NETMAN_WIFI_MAXROWS];
	int n = netman_wifi__rows(rows, NETMAN_WIFI_MAXROWS);
	if (n < 0) return 1;
	for (int i = 0; i < n; i++) {
		const char *band = "";
		int ch = netman_wifi_chan(rows[i].freq, &band);
		netman_wifi__Row *w = &rows[i];
		fprintf(NETMAN_OUT, "%c\t%d\t%s\t%d\t%d\t%s\t%s\t%s\t%d\t%d\t%s\n", w->conn ? '*' : ' ', w->known, w->sec, w->dbm, w->pct, w->bssid,
		        ch ? (snprintf(w->chan, sizeof w->chan, "%d", ch), w->chan) : "-", *band ? band : "-", w->nbss, w->hidden, w->hidden ? "" : w->name);
	}
	return 0;
}

int netman_wifi_list(void)
{
	static netman_wifi__Row rows[NETMAN_WIFI_MAXROWS];
	int n = netman_wifi__rows(rows, NETMAN_WIFI_MAXROWS);
	if (n < 0) return 1;
	for (int i = 0; i < n; i++) {
		netman_wifi__Row *w = &rows[i];
		fprintf(NETMAN_OUT, "%c %-32s %-6s %4d dBm %3d%%%s%s\n", w->conn ? '*' : ' ', w->hidden ? "<hidden>" : w->name, w->sec, w->dbm, w->pct,
		        w->known ? "  (known)" : "", w->hidden ? "  " : "");
		if (w->hidden) fprintf(NETMAN_OUT, "  %s\n", w->bssid);
	}
	return 0;
}

/* ---- details ---- */

static void netman_wifi__kv(const char *k, const char *fmt, ...)
{
	char v[300];
	va_list a;
	va_start(a, fmt);
	vsnprintf(v, sizeof v, fmt, a);
	va_end(a);
	if (v[0]) fprintf(NETMAN_OUT, "%-14s %s\n", k, v);
}

static void netman_wifi__info_freq(int mhz)
{
	const char *band = "";
	int ch = netman_wifi_chan(mhz, &band);
	if (mhz) netman_wifi__kv("channel", "%d (%d MHz, %s GHz)", ch, mhz, band);
}

static int netman_wifi__iwd_info(int fd, const char *st, const char *ssid)
{
	const char *conn = netman_dbus_pget(st, netman_wifi__IWD ".Station", "ConnectedNetwork"), *net, *k, *name, *esc;
	char nm[200];
	if (!ssid) {
		if (!conn) { netman_wifi__seterr("not connected: name a network (wifi info SSID)", 0); return 1; }
		net = conn;
	} else if (!(net = netman_wifi__net_by_name(ssid))) {
		if (netman_wifi__scan(fd, st)) return 1;
		if (!(net = netman_wifi__net_by_name(ssid))) { fprintf(NETMAN_ERR, "wifi: no network called %s in range (hidden networks need: wifi connect SSID)\n", ssid); return 1; }
	}
	name = netman_dbus_pget(net, netman_wifi__IWD ".Network", "Name") ?: "";
	esc = name;
	netman_wifi_escape(esc, strlen(esc), nm, sizeof nm);
	netman_wifi__kv("ssid", "%s", nm);
	netman_wifi__kv("backend", "iwd");
	netman_wifi__kv("security", "%s (iwd type %s)", netman_wifi_sec_iwd(netman_dbus_pget(net, netman_wifi__IWD ".Network", "Type")), netman_dbus_pget(net, netman_wifi__IWD ".Network", "Type") ?: "?");
	k = netman_dbus_pget(net, netman_wifi__IWD ".Network", "KnownNetwork");
	netman_wifi__kv("known", "%s", k ? "yes" : "no");
	if (k) {
		const char *ac = netman_dbus_pget(k, netman_wifi__IWD ".KnownNetwork", "AutoConnect"), *lc = netman_dbus_pget(k, netman_wifi__IWD ".KnownNetwork", "LastConnectedTime"), *hd = netman_dbus_pget(k, netman_wifi__IWD ".KnownNetwork", "Hidden");
		netman_wifi__kv("autoconnect", "%s", ac && !strcmp(ac, "true") ? "yes" : "no");
		netman_wifi__kv("last connected", "%s", lc ?: "never");
		if (hd && !strcmp(hd, "true")) netman_wifi__kv("hidden", "yes (not broadcast)");
	}
	netman_wifi__kv("state", "%s", conn && !strcmp(conn, net) ? "connected" : "not connected");
	{   /* the access points that make up this network */
		const char *ess = netman_dbus_pget(net, netman_wifi__IWD ".Network", "ExtendedServiceSet");
		if (ess && *ess) {
			char list[640], *p, *sv = NULL, bs[400] = "";
			snprintf(list, sizeof list, "%s", ess);
			for (p = strtok_r(list, ",", &sv); p; p = strtok_r(NULL, ",", &sv)) {
				const char *a = netman_dbus_pget(p, netman_wifi__IWD ".BasicServiceSet", "Address");
				if (a) { size_t l = strlen(bs); snprintf(bs + l, sizeof bs - l, "%s%s", l ? " " : "", a); }
			}
			netman_wifi__kv("bssid", "%s", bs);
		}
	}
	if (conn && !strcmp(conn, net) && !netman_wifi__iwd_diag(fd, st)) {
		const char *v;
		if ((v = netman_wifi__dget("ConnectedBss"))) netman_wifi__kv("bssid (now)", "%s", v);
		if ((v = netman_wifi__dget("Frequency"))) netman_wifi__info_freq(atoi(v));
		if ((v = netman_wifi__dget("RSSI"))) netman_wifi__kv("signal", "%d dBm  %d %%", atoi(v), netman_wifi_pct(atoi(v)));
		if ((v = netman_wifi__dget("AverageRSSI"))) netman_wifi__kv("signal avg", "%d dBm", atoi(v));
		if ((v = netman_wifi__dget("Security"))) netman_wifi__kv("auth", "%s", v);
		if ((v = netman_wifi__dget("RxMode"))) netman_wifi__kv("rx mode", "%s", v);
		if ((v = netman_wifi__dget("RxBitrate"))) netman_wifi__kv("rx rate", "%.1f Mbit/s", atoi(v) / 10.0);
		if ((v = netman_wifi__dget("TxBitrate"))) netman_wifi__kv("tx rate", "%.1f Mbit/s", atoi(v) / 10.0);
		if ((v = netman_wifi__dget("ExpectedThroughput"))) netman_wifi__kv("throughput", "%d Mbit/s (expected)", atoi(v) / 1000);
		netman_wifi__kv("noise / SNR", "not reported by iwd");
	} else {
		static netman_wifi__Row rows[NETMAN_WIFI_MAXROWS];
		int n = netman_wifi__iwd_rows(fd, st, rows, NETMAN_WIFI_MAXROWS);
		for (int i = 0; i < n; i++) if (!rows[i].hidden && !strcmp(rows[i].dev_path, net)) netman_wifi__kv("signal", "%d dBm  %d %%", rows[i].dbm, rows[i].pct);
	}
	return 0;
}

static int netman_wifi__wpa_info(const char *ifname, const char *ssid)
{
	static char st[2048], sp[1024], sr[32768], ln[8192];
	char v[200], cssid[64] = "", want[64] = "", esc[200], bs[400] = "";
	int connected, mhz = 0, nb = 0, best = -999;
	const char *flags = NULL;
	if (netman_wifi__wpa_status(ifname, st, sizeof st)) { netman_wifi__seterr("cannot talk to wpa_supplicant (needs root or the netdev group)", 0); return 1; }
	connected = netman_wifi__kv_get(st, "wpa_state", v, sizeof v) && !strcmp(v, "COMPLETED");
	if (netman_wifi__kv_get(st, "ssid", v, sizeof v)) netman_wifi__wpa_decode(v, cssid, sizeof cssid);
	if (ssid) snprintf(want, sizeof want, "%s", ssid); else if (connected) snprintf(want, sizeof want, "%s", cssid);
	else { netman_wifi__seterr("not connected: name a network (wifi info SSID)", 0); return 1; }
	connected = connected && !strcmp(want, cssid);
	netman_wifi_escape(want, strlen(want), esc, sizeof esc);
	netman_wifi__kv("ssid", "%s", esc);
	netman_wifi__kv("backend", "wpa_supplicant (%s)", ifname);
	if (!netman_wifi__wpa_cmd(ifname, "SCAN_RESULTS", sr, sizeof sr))
		for (char *l = strtok(sr, "\n"); l; l = strtok(NULL, "\n")) {
			char *f[5] = {0}, *x = l, raw[80];
			int k = 0;
			for (; k < 5; k++) { f[k] = x; if (!(x = strchr(x, '\t'))) break; *x++ = 0; }
			if (k < 4 || !strncmp(f[0], "bssid", 5)) continue;
			netman_wifi__wpa_decode(f[4], raw, sizeof raw);
			if (strcmp(raw, want)) continue;
			{ size_t bl = strlen(bs); snprintf(bs + bl, sizeof bs - bl, "%s%s", bl ? " " : "", f[0]); }
			nb++;
			if (atoi(f[2]) > best) { best = atoi(f[2]); mhz = atoi(f[1]); flags = NULL; snprintf(ln, sizeof ln, "%s", f[3]); flags = ln; }
		}
	if (flags) {
		netman_wifi__kv("security", "%s  %s", netman_wifi_sec_flags(flags), flags);
		netman_wifi__kv("bssid", "%s", bs);
		netman_wifi__kv("access points", "%d", nb);
		netman_wifi__info_freq(mhz);
		netman_wifi__kv("signal", "%d dBm  %d %%", best, netman_wifi_pct(best));
	}
	{
		struct netman_wifi__knownset ks = { .n = 0 };
		int known = 0;
		netman_wifi__wpa_each_net(ifname, netman_wifi__collect_known, &ks);
		for (int i = 0; i < ks.n; i++) if (!strcmp(ks.ssid[i], want)) known = 1;
		netman_wifi__kv("known", "%s", known ? "yes" : "no");
	}
	netman_wifi__kv("state", "%s", connected ? "connected" : "not connected");
	if (connected) {
		if (netman_wifi__kv_get(st, "bssid", v, sizeof v)) netman_wifi__kv("bssid (now)", "%s", v);
		if (netman_wifi__kv_get(st, "key_mgmt", v, sizeof v)) netman_wifi__kv("auth", "%s", v);
		if (netman_wifi__kv_get(st, "pairwise_cipher", v, sizeof v)) netman_wifi__kv("cipher", "%s", v);
		if (netman_wifi__kv_get(st, "ip_address", v, sizeof v)) netman_wifi__kv("ip (wpa)", "%s", v);
		if (!netman_wifi__wpa_cmd(ifname, "SIGNAL_POLL", sp, sizeof sp)) {
			int rssi = 0, noise = 0;
			if (netman_wifi__kv_get(sp, "RSSI", v, sizeof v)) { rssi = atoi(v); netman_wifi__kv("signal now", "%d dBm  %d %%", rssi, netman_wifi_pct(rssi)); }
			if (netman_wifi__kv_get(sp, "AVG_RSSI", v, sizeof v)) netman_wifi__kv("signal avg", "%d dBm", atoi(v));
			if (netman_wifi__kv_get(sp, "NOISE", v, sizeof v)) { noise = atoi(v); netman_wifi__kv("noise", "%d dBm   SNR %d dB", noise, rssi - noise); }
			if (netman_wifi__kv_get(sp, "LINKSPEED", v, sizeof v)) netman_wifi__kv("link speed", "%d Mbit/s", atoi(v));
			if (netman_wifi__kv_get(sp, "FREQUENCY", v, sizeof v)) netman_wifi__info_freq(atoi(v));
			if (netman_wifi__kv_get(sp, "WIDTH", v, sizeof v)) netman_wifi__kv("width", "%s", v);
		}
	}
	return 0;
}

int netman_wifi_info(const char *ssid)
{
	const char *st;
	int fd, rc;
	if (netman_wifi_backend_is_wpa() && netman_wifi__wpa_present(netman_wifi__wpa_iface())) return netman_wifi__wpa_info(netman_wifi__wpa_iface(), ssid);
	if ((fd = netman_wifi__iwd_open(&st)) < 0) return 1;
	rc = netman_wifi__iwd_info(fd, st, ssid);
	close(fd);
	return rc;
}

#endif /* NETMAN_WIFI_IMPLEMENTATION_DONE */
#endif /* NETMAN_WIFI_IMPLEMENTATION */
