/* cli.h - the netman command line: ONE table drives dispatch, root decision, disabled-module check, `netman help` and the man page.
 *
 * Each row: name (words), args spec, handler, root flag (priv.h), module (config section whose `enabled` gates it), summary, long help.
 * Matching is by words, longest name wins: "net profile save a b" runs the row "net profile save" with args {a, b}.
 * Included once by netman.c after all the mods (single translation unit, like the stb headers); help.c needs the table.
 */
#ifndef NETMAN_CLI_H
#define NETMAN_CLI_H

typedef struct nc_cmd {
	const char *name, *args;
	int (*fn)(const struct nc_cmd *c, int n, char **a);
	int root;               /* NM_ROOT_* */
	const char *module;     /* config section gating the row, or NULL */
	const char *sum, *help; /* one line; longer text (may be "") */
} nc_cmd;

static netman_config nc_cfg;      /* the effective configuration, loaded once in main */

static int nc_usage(const nc_cmd *c) { fprintf(stderr, "usage: netman %s %s\n", c->name, c->args); return 2; }

/* ---- config -> module globals ------------------------------------------------------------------------------------- */

static char nc_probe_host[160], nc_probe_path[256];
static void nc_apply(const netman_config *c)
{
	const char *b = netman_config_str(c, NC_WIFI_BACKEND), *d = netman_config_str(c, NC_NET_DNS_MODE), *u = netman_config_str(c, NC_ROAM_PROBE_URL);
	int port = 80;
	netman_vpn_env();
	{ const char *e = getenv("NETMAN_CONF_DIR"); if (e && *e) netman_conf_dir = e; }   /* profile directory (NAME.conf / NAME.ent), default /etc/netman: relocation and tests */
	netman_lic_dir = netman_config_str(c, NC_LIC_DIR);
	netman_lic_warn_days = netman_config_int(c, NC_LIC_WARN);
	netman_lic_renew_hook = netman_config_str(c, NC_LIC_RENEW_HOOK);
	netman_lic_apply_hook = netman_config_str(c, NC_LIC_APPLY_HOOK);
	netman_wifi_backend(!strcmp(b, "iwd") ? 1 : !strcmp(b, "wpa") ? 2 : 0);
	netman_dns_mode = !strcmp(d, "resolvconf") ? 1 : !strcmp(d, "resolved") ? 2 : 0;
	netman_state_dir = netman_config_str(c, NC_NET_STATE_DIR);
	netman_metrics_dir = netman_config_str(c, NC_METRICS_DIR);
	if (netman_config_bool(c, NC_ROAM_PROBE) && *u && !netman_config_split_url(u, nc_probe_host, sizeof nc_probe_host, &port, nc_probe_path, sizeof nc_probe_path)) {
		netman_probe_host = nc_probe_host; netman_probe_path = nc_probe_path; netman_probe_port = port;
	} else netman_probe_host = "";
}

/* 1 when the VPN profile dir holds a *.conf / *.ovpn, or cannot be listed by us (then there probably is one that needs root) */
static int nc_vpn_has_profiles(void)
{
	DIR *d = opendir(netman_vpn_conf_dir);
	struct dirent *e;
	int found = 0;
	if (!d) return errno == EACCES;
	while (!found && (e = readdir(d))) { const char *dot = strrchr(e->d_name, '.'); found = dot && (!strcmp(dot, ".conf") || !strcmp(dot, ".ovpn")); }
	closedir(d);
	return found;
}

static int nc_enabled(const char *module)
{
	static const struct { const char *m; int k; } t[] = { { "wifi", NC_WIFI_ENABLED }, { "bluetooth", NC_BT_ENABLED }, { "net", NC_NET_ENABLED }, { "roaming", NC_ROAM_ENABLED }, { "vpn", NC_VPN_ENABLED }, { "firewall", NC_FW_ENABLED }, { "metrics", NC_METRICS_ENABLED }, { "enterprise", NC_ENT_ENABLED }, { "licence", NC_LICENCE_ENABLED }, { "tether", NC_TETHER_ENABLED } };
	if (!module) return 1;
	for (unsigned i = 0; i < sizeof t / sizeof *t; i++) if (!strcmp(module, t[i].m)) return netman_config_bool(&nc_cfg, t[i].k);
	return 1;
}

/* ---- handlers: status / wifi / bt ---------------------------------------------------------------------------------- */

static int h_status(const nc_cmd *c, int n, char **a)
{
	(void)c; (void)n; (void)a;
	if (nc_enabled("net")) netman_print(stdout);
	if (nc_enabled("wifi")) netman_wifi_status();
	if (nc_enabled("bluetooth")) netman_bt_status();
	if (nc_enabled("vpn")) { puts("vpn"); netman_vpn_list(); }
	return 0;
}

static int h_wifi_list(const nc_cmd *c, int n, char **a) { (void)c; (void)n; (void)a; return netman_wifi_list(); }
static int h_wifi_scan(const nc_cmd *c, int n, char **a) { (void)c; (void)n; (void)a; return netman_wifi_scan(); }
static int h_wifi_info(const nc_cmd *c, int n, char **a) { (void)c; return netman_wifi_info(n > 0 ? a[0] : NULL); }
static int h_wifi_connect(const nc_cmd *c, int n, char **a)
{
	if (n >= 1 && !strcmp(a[0], "--eap")) {   /* --eap SSID IDENTITY PASSWORD */
		if (n < 4) return nc_usage(c);
		return netman_wifi_connect_eap(a[1], a[2], a[3]);
	}
	if (n < 1) return nc_usage(c);
	return netman_wifi_connect(a[0], n > 1 ? a[1] : NULL);
}
static int h_wifi_disconnect(const nc_cmd *c, int n, char **a) { (void)c; (void)n; (void)a; return netman_wifi_disconnect(); }
static int h_wifi_forget(const nc_cmd *c, int n, char **a) { return n < 1 ? nc_usage(c) : netman_wifi_forget(a[0]); }
static int h_wifi_wps(const nc_cmd *c, int n, char **a)
{
	if (n < 1) return nc_usage(c);
	if (!strcmp(a[0], "pbc")) return netman_wifi_wps_pbc();
	if (!strcmp(a[0], "pin")) return netman_wifi_wps_pin(n > 1 ? a[1] : NULL);
	if (!strcmp(a[0], "cancel")) return netman_wifi_wps_cancel();
	return nc_usage(c);
}
static int h_wifi_hotspot(const nc_cmd *c, int n, char **a)
{
	if (n >= 2 && !strcmp(a[0], "start")) return netman_wifi_hotspot_start(a[1], n > 2 ? a[2] : NULL);
	if (n >= 1 && !strcmp(a[0], "stop")) return netman_wifi_hotspot_stop();
	return nc_usage(c);
}
static int h_wifi_known(const nc_cmd *c, int n, char **a)
{
	if (n == 0) return netman_wifi_known();
	if (n == 3 && !strcmp(a[0], "autoconnect") && (!strcmp(a[2], "on") || !strcmp(a[2], "off"))) return netman_wifi_autoconnect(a[1], !strcmp(a[2], "on"));
	return nc_usage(c);
}

static int h_bt_list(const nc_cmd *c, int n, char **a) { (void)c; (void)n; (void)a; return netman_bt_list(); }
static int h_bt_scan(const nc_cmd *c, int n, char **a) { (void)c; return netman_bt_scan(n > 0 ? atoi(a[0]) : 5); }
static int h_bt_info(const nc_cmd *c, int n, char **a) { return n < 1 ? nc_usage(c) : netman_bt_info(a[0]); }
static int h_bt_agent(const nc_cmd *c, int n, char **a) { (void)c; return netman_bt_agent(n > 0 ? atoi(a[0]) : 0); }
static int h_bt_power(const nc_cmd *c, int n, char **a) { return n < 1 || (strcmp(a[0], "on") && strcmp(a[0], "off")) ? nc_usage(c) : netman_bt_power(!strcmp(a[0], "on")); }
static int h_bt_action(const nc_cmd *c, int n, char **a)
{
	const char *act = strrchr(c->name, ' ') + 1;
	int rc;
	if (n < 1) return nc_usage(c);
	rc = netman_bt_action(act, a[0]);
	return rc == 2 ? nc_usage(c) : rc;
}

/* ---- net / roam ---------------------------------------------------------------------------------------------------- */

static int nc_neterr(const char *what, int r)
{
	if (r >= 0) return 0;
	fprintf(stderr, "netman: %s: %s%s%s\n", what, strerror(-r), netman_error() && *netman_error() ? ": " : "", netman_error() ? netman_error() : "");
	return 1;
}
static void nc_if_show(const netman_if *x, const netman_ifstat *st)
{
	char r[32], t[32], sp[32];
	printf("%-12s %-4s %-10s %02x:%02x:%02x:%02x:%02x:%02x mtu %d\n", x->name, x->up ? "UP" : "DOWN", x->carrier ? "carrier" : "no-carrier", x->mac[0], x->mac[1], x->mac[2], x->mac[3], x->mac[4], x->mac[5], x->mtu);
	for (int j = 0; j < x->naddr; j++) printf("  addr  %s\n", x->addr[j]);
	if (*x->gw4) printf("  gw    %s\n", x->gw4);
	if (*x->gw6) printf("  gw    %s\n", x->gw6);
	for (int j = 0; j < x->nroute; j++) printf("  route %s\n", x->route[j]);
	if (st) {
		netman_metrics_fmt_rate(r, sizeof r, st->rx_bps); netman_metrics_fmt_rate(t, sizeof t, st->tx_bps);
		if (st->speed_mbps > 0) snprintf(sp, sizeof sp, "%d Mb/s", st->speed_mbps); else snprintf(sp, sizeof sp, "unknown");
		printf("  speed %s\n  rate  rx %s  tx %s\n", sp, r, t);
	}
}
static int h_net_list(const nc_cmd *c, int n, char **a) { (void)c; (void)n; (void)a; netman_print(stdout); return 0; }
static int h_net_show(const nc_cmd *c, int n, char **a)
{
	netman_if x; netman_metrics m; const netman_ifstat *st;
	int r;
	if (n < 1) return nc_usage(c);
	if ((r = netman_find(a[0], &x)) < 0) return nc_neterr(a[0], r);
	memset(&m, 0, sizeof m);
	netman_metrics_poll(&m, 0); usleep(300000); netman_metrics_poll(&m, 300);
	st = netman_metrics_find(&m, a[0]);
	nc_if_show(&x, st);
	return 0;
}
static int h_net_up(const nc_cmd *c, int n, char **a) { return n < 1 ? nc_usage(c) : nc_neterr("up", netman_link_set(a[0], 1)); }
static int h_net_down(const nc_cmd *c, int n, char **a) { return n < 1 ? nc_usage(c) : nc_neterr("down", netman_link_set(a[0], 0)); }
static int h_net_set(const nc_cmd *c, int n, char **a)   /* IFACE ADDR/LEN [GATEWAY] */
{
	int r;
	if (n < 2) return nc_usage(c);
	if ((r = netman_addr_add(a[0], a[1])) < 0) return nc_neterr("addr", r);
	if (n > 2 && (r = netman_route_add(a[0], "default", a[2], netman_metric(a[0]))) < 0) return nc_neterr("route", r);
	return 0;
}
static int nc_dhcp_off(void) { if (netman_config_bool(&nc_cfg, NC_NET_DHCP)) return 0; fputs("netman: the DHCP client is disabled in config (net.dhcp = no)\n", stderr); return 1; }
static int h_net_dhcp(const nc_cmd *c, int n, char **a)
{
	netman_lease l; int r;
	if (n < 1) return nc_usage(c);
	if (nc_dhcp_off()) return 3;
	if ((r = netman_dhcp_acquire(a[0], netman_dhcp_timeout_ms, &l)) < 0) return nc_neterr("dhcp", r);
	if ((r = netman_lease_apply(&l)) < 0) return nc_neterr("apply lease", r);
	netman_lease_print(stdout, &l);
	return 0;
}
static int h_net_dns(const nc_cmd *c, int n, char **a)   /* IFACE NS... | IFACE clear */
{
	if (n < 2) return nc_usage(c);
	if (!strcmp(a[1], "clear")) return nc_neterr("dns", netman_dns_clear(a[0]));
	return nc_neterr("dns", netman_dns_set(a[0], (const char **)a + 1, n - 1, NULL));
}
static int h_prof_list(const nc_cmd *c, int n, char **a)
{
	char names[32][64]; int k = netman_profile_list(names, 32);
	(void)c; (void)n; (void)a;
	for (int i = 0; i < k; i++) {
		netman_profile p;
		if (netman_profile_load(names[i], &p) == 0) printf("%-16s %-8s %-6s %s prio %d%s\n", p.name, p.iface, p.dhcp ? "dhcp" : "static", p.dhcp ? "" : p.addr, p.priority, p.autoconnect ? " auto" : "");
	}
	return 0;
}
static int h_prof_save(const nc_cmd *c, int n, char **a)   /* NAME IFACE dhcp|ADDR/LEN [GATEWAY [DNS...]] */
{
	netman_profile p;
	if (n < 3) return nc_usage(c);
	memset(&p, 0, sizeof p);
	snprintf(p.name, sizeof p.name, "%s", a[0]); snprintf(p.iface, sizeof p.iface, "%s", a[1]);
	if (!strcmp(a[2], "dhcp")) p.dhcp = 1;
	else { snprintf(p.addr, sizeof p.addr, "%s", a[2]); if (n > 3) snprintf(p.gateway, sizeof p.gateway, "%s", a[3]); }
	for (int i = 4; i < n && p.ndns < 3; i++) snprintf(p.dns[p.ndns++], sizeof p.dns[0], "%s", a[i]);
	p.autoconnect = 1;
	return nc_neterr("profile", netman_profile_save(&p));
}
static int h_prof_apply(const nc_cmd *c, int n, char **a)
{
	netman_profile p; netman_lease l; int r;
	if (n < 1) return nc_usage(c);
	if ((r = netman_profile_load(a[0], &p)) < 0) return nc_neterr(a[0], r);
	if (p.dhcp && nc_dhcp_off()) return 3;
	return nc_neterr("apply", netman_profile_apply(&p, &l));
}
static int h_roam_status(const nc_cmd *c, int n, char **a)
{
	netman_if t[NETMAN_MAXIF]; int k = netman_list(t, NETMAN_MAXIF);
	(void)c; (void)n; (void)a;
	printf("probe     %s\n", *netman_probe_host ? netman_probe_host : "off");
	printf("threshold %d\n", netman_config_int(&nc_cfg, NC_ROAM_SCORE));
	for (int i = 0; i < k; i++) {
		if (!t[i].up || !strcmp(t[i].name, "lo")) continue;
		printf("%-12s score %3d  %s\n", t[i].name, netman_score_iface(t[i].name, -1), netman_is_wifi(t[i].name) ? "wifi" : "wired");
	}
	return 0;
}
static int h_roam_prefer(const nc_cmd *c, int n, char **a)   /* PROFILE PRIORITY */
{
	netman_profile p; int r;
	if (n < 2) return nc_usage(c);
	if ((r = netman_profile_load(a[0], &p)) < 0) return nc_neterr(a[0], r);
	p.priority = atoi(a[1]);
	return nc_neterr("prefer", netman_profile_save(&p));
}

/* ---- vpn ----------------------------------------------------------------------------------------------------------- */

static int h_vpn(const nc_cmd *c, int n, char **a)   /* the library's own front end: "vpn up X" -> {vpn, up, X} */
{
	char *av[40]; int k = 0;
	const char *sub = strchr(c->name, ' ') + 1;
	if (n > 36) n = 36;
	av[k++] = "vpn"; av[k++] = (char *)sub;
	for (int i = 0; i < n; i++) av[k++] = a[i];
	av[k] = NULL;
	if (!strcmp(sub, "up") && n >= 1 && netman_config_bool(&nc_cfg, NC_VPN_KILLSWITCH)) {
		int rc = netman_vpn_cmd(k, av);
		return rc ? rc : netman_vpn_killswitch(a[0], 1);
	}
	return netman_vpn_cmd(k, av);
}

/* ---- firewall ------------------------------------------------------------------------------------------------------ */

static int nc_fw_build(nmfw_cfg *f, const char *preset)
{
	netman_if t[NETMAN_MAXIF]; int k;
	nmfw_cfg_init(f);
	if (nmfw_preset(f, preset && *preset ? preset : netman_config_str(&nc_cfg, NC_FW_PRESET))) { fprintf(stderr, "netman: %s\n", nmfw_error()); return 1; }
	k = netman_list(t, NETMAN_MAXIF);
	for (int i = 0; i < k; i++) if (strcmp(t[i].name, "lo")) nmfw_set_zone(f, t[i].name, netman_config_str(&nc_cfg, NC_FW_ZONE));
	return 0;
}
static int nc_fw_off(void) { if (netman_config_bool(&nc_cfg, NC_FW_ENABLED)) return 0; fputs("netman: firewall changes are disabled in config (netman config set firewall.enabled yes)\n", stderr); return 3; }
static int h_fw_plan(const nc_cmd *c, int n, char **a)
{
	static char buf[65536]; nmfw_cfg f;
	(void)c;
	if (nc_fw_build(&f, n > 0 ? a[0] : NULL)) return 1;
	if (nmfw_plan(&f, buf, sizeof buf) < 0) { fprintf(stderr, "netman: %s\n", nmfw_error()); return 1; }
	fputs(buf, stdout);
	return 0;
}
static int h_fw_show(const nc_cmd *c, int n, char **a)
{
	(void)c; (void)n; (void)a;
	return system("nft list ruleset 2>&1 | sed -n '/netman/,$p' | head -200") ? 1 : 0;
}
static int h_fw_apply(const nc_cmd *c, int n, char **a)
{
	nmfw_cfg f; int rc, cs = netman_config_int(&nc_cfg, NC_FW_CONFIRM);
	(void)c;
	if (nc_fw_off()) return 3;
	if (nc_fw_build(&f, n > 0 ? a[0] : NULL)) return 1;
	rc = nmfw_apply(&f, cs);
	if (rc) fprintf(stderr, "netman: %s\n", nmfw_error());
	else if (cs > 0) printf("applied; run `netman fw confirm` within %d s or it rolls back\n", cs);
	return rc ? 1 : 0;
}
static int h_fw_diff(const nc_cmd *c, int n, char **a)
{
	static char buf[65536]; nmfw_cfg f;
	(void)c;
	if (nc_fw_build(&f, n > 0 ? a[0] : NULL)) return 1;
	if (nmfw_diff(&f, buf, sizeof buf) < 0) { fprintf(stderr, "netman: %s\n", nmfw_error()); return 1; }
	fputs(buf, stdout);
	return 0;
}
static int h_fw_confirm(const nc_cmd *c, int n, char **a) { (void)c; (void)n; (void)a; if (nc_fw_off()) return 3; if (nmfw_confirm()) { fprintf(stderr, "netman: %s\n", nmfw_error()); return 1; } return 0; }
static int h_fw_rollback(const nc_cmd *c, int n, char **a) { (void)c; (void)n; (void)a; if (nc_fw_off()) return 3; if (nmfw_rollback()) { fprintf(stderr, "netman: %s\n", nmfw_error()); return 1; } return 0; }

/* ---- metrics ------------------------------------------------------------------------------------------------------- */

static void nc_metrics_table(const netman_metrics *m)
{
	printf("%-12s %-8s %10s %10s %12s %12s\n", "iface", "state", "rx/s", "tx/s", "rx total", "tx total");
	for (int i = 0; i < m->n; i++) {
		const netman_ifstat *s = &m->s[i];
		char a[32], b[32], c[32], d[32];
		netman_metrics_fmt_rate(a, sizeof a, s->rx_bps); netman_metrics_fmt_rate(b, sizeof b, s->tx_bps);
		netman_metrics_fmt_bytes(c, sizeof c, s->rx_bytes); netman_metrics_fmt_bytes(d, sizeof d, s->tx_bytes);
		printf("%-12s %-8s %10s %10s %12s %12s\n", s->name, s->oper, a, b, c, d);
	}
}
static int h_metrics_show(const nc_cmd *c, int n, char **a)
{
	netman_metrics m; (void)c; (void)n; (void)a;
	memset(&m, 0, sizeof m);
	netman_metrics_poll(&m, 0); usleep(500000); netman_metrics_poll(&m, 500);
	nc_metrics_table(&m);
	return 0;
}
static int h_metrics_record(const nc_cmd *c, int n, char **a)
{
	netman_metrics m; int k;
	(void)c; (void)n; (void)a;
	memset(&m, 0, sizeof m);
	netman_metrics_poll(&m, 0); usleep(500000); netman_metrics_poll(&m, 500);
	k = netman_metrics_record(&m, (long)time(NULL));
	if (k < 0) { fprintf(stderr, "netman: cannot write %s: %s\n", netman_metrics_dir, strerror(errno)); return 1; }
	printf("recorded %d interfaces to %s\n", k, netman_metrics_dir);
	return 0;
}
static int h_metrics_watch(const nc_cmd *c, int n, char **a)   /* [COUNT]: none = until ^C */
{
	netman_metrics m; int cnt = n > 0 ? atoi(a[0]) : 0, iv = netman_config_int(&nc_cfg, NC_METRICS_INTERVAL);
	(void)c;
	memset(&m, 0, sizeof m);
	netman_metrics_poll(&m, 0);
	for (int i = 0; !cnt || i < cnt; i++) {
		sleep((unsigned)iv);
		netman_metrics_poll(&m, (long)(i + 1) * iv * 1000);
		nc_metrics_table(&m);
		fflush(stdout);
	}
	return 0;
}

/* ---- enterprise ---------------------------------------------------------------------------------------------- */
static int nc_ent_off(void) { if (netman_config_bool(&nc_cfg, NC_ENT_ENABLED)) return 0; fputs("netman: enterprise changes are disabled in config (netman config set enterprise.enabled yes)\n", stderr); return 3; }
static int nc_ent_err(int r) { if (r >= 0) return 0; fprintf(stderr, "netman: %s\n", r == -22 && *netman_ent_error() ? netman_ent_error() : *netman_ent_error() ? netman_ent_error() : strerror(-r)); return 1; }
static int h_ent_list(const nc_cmd *c, int n, char **a)
{
	char names[64][64]; int k = netman_ent_list(names, 64);
	(void)c; (void)n; (void)a;
	for (int i = 0; i < k; i++) { netman_ent e; if (netman_ent_load(names[i], &e) == 0) printf("%-20s %s\n", names[i], e.kind); else printf("%-20s (invalid: %s)\n", names[i], netman_ent_error()); }
	return 0;
}
static int h_ent_show(const nc_cmd *c, int n, char **a)
{
	netman_ent e; (void)c; (void)n;
	if (nc_ent_err(netman_ent_load(a[0], &e))) return 1;
	return nc_ent_err(netman_ent_show(&e, stdout));
}
static int h_ent_add(const nc_cmd *c, int n, char **a)
{
	netman_ent e; (void)c; (void)n;
	if (nc_ent_off()) return 3;
	if (nc_ent_err(netman_ent_load(a[0], &e))) return 1;
	if (!strcmp(e.kind, "tether") && !nc_enabled("tether")) { fputs("tether disabled in config (netman config toggle tether.enabled)\n", stderr); return 3; }
	return nc_ent_err(netman_ent_add(&e));
}
static int h_ent_del(const nc_cmd *c, int n, char **a)
{
	netman_ent e; (void)c; (void)n;
	if (nc_ent_off()) return 3;
	if (nc_ent_err(netman_ent_load(a[0], &e))) return 1;
	return nc_ent_err(netman_ent_del(&e));
}
static int h_ent_save(const nc_cmd *c, int n, char **a)   /* ent save NAME KIND [KEY=VALUE...] */
{
	char text[8192] = ""; netman_ent e; (void)c;
	snprintf(text, sizeof text, "name=%s\nkind=%s\n", a[0], a[1]);
	for (int i = 2; i < n; i++) { strncat(text, a[i], sizeof text - strlen(text) - 2); strcat(text, "\n"); }
	if (nc_ent_err(netman_ent_parse(text, &e))) return 1;
	return nc_ent_err(netman_ent_save(&e));
}
static int h_ent_apply(const nc_cmd *c, int n, char **a)
{
	(void)c; (void)n; (void)a;
	if (nc_ent_off()) return 3;
	return netman_ent_apply_all() ? 1 : 0;
}
static volatile sig_atomic_t nc_ent_stop;
static void nc_ent_sig(int s) { (void)s; nc_ent_stop = 1; }
static int h_ent_ecmp(const nc_cmd *c, int n, char **a)   /* foreground failover loop; the daemon can call the same */
{
	netman_ent e; int ms = netman_config_int(&nc_cfg, NC_ENT_PROBE_MS); (void)c; (void)n;
	if (nc_ent_off()) return 3;
	if (nc_ent_err(netman_ent_load(a[0], &e))) return 1;
	if (strcmp(e.kind, "ecmp")) { fputs("netman: not an ecmp profile\n", stderr); return 1; }
	signal(SIGINT, nc_ent_sig); signal(SIGTERM, nc_ent_sig);
	return nc_ent_err(netman_ent_ecmp_run(&e, ms, &nc_ent_stop));
}
static int h_ent_tether(const nc_cmd *c, int n, char **a)
{
	netman_ent_tether_dev d[16]; int k = netman_ent_tether_detect(d, 16);
	(void)c; (void)n; (void)a;
	if (k <= 0) { puts("no USB or Bluetooth tethering device found"); return k < 0 ? 1 : 0; }
	for (int i = 0; i < k; i++) printf("%-10s %-4s %s\n", d[i].ifn, d[i].kind, d[i].driver);
	return 0;
}
static int h_wol(const nc_cmd *c, int n, char **a)
{
	(void)c; (void)n;
	return nc_ent_err(netman_ent_wol_send(a[0], a[1]));
}
static int h_lic_status(const nc_cmd *c, int n, char **a)
{
	(void)c; (void)n; (void)a;
	return netman_lic_status(stdout) > 0 ? 1 : 0;      /* exit 1 when anything is expiring/expired: usable in cron */
}
static int h_lic_renew(const nc_cmd *c, int n, char **a)
{
	(void)c; (void)n;
	return nc_ent_err(netman_lic_renew(a[0]));
}
static int h_key_rotate(const nc_cmd *c, int n, char **a)
{
	char pub[45]; (void)c; (void)n;
	if (nc_ent_err(netman_key_rotate(a[0], pub))) return 1;
	printf("%s\n", pub);                              /* new public key: hand it to the peer */
	return 0;
}
static int h_key_show(const nc_cmd *c, int n, char **a)
{
	char pub[45]; (void)c; (void)n;
	if (nc_ent_err(netman_key_show(a[0], pub))) return 1;
	printf("%s\n", pub);
	return 0;
}

/* ---- config -------------------------------------------------------------------------------------------------------- */

static int h_cfg_show(const nc_cmd *c, int n, char **a) { (void)c; (void)n; (void)a; netman_config_show(&nc_cfg, stdout); return 0; }
static int h_cfg_example(const nc_cmd *c, int n, char **a) { (void)c; (void)n; (void)a; netman_config_example(stdout); return 0; }
static int h_cfg_check(const nc_cmd *c, int n, char **a)
{
	netman_config t; char err[512];
	(void)c; (void)n; (void)a;
	if (netman_config_load(&t, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
	puts("config ok");
	return 0;
}
static const char *nc_cfg_path(char *buf, size_t n)
{
	const char *e = getenv("NETMAN_CONFIG");
	if (e && *e) { snprintf(buf, n, "%s", e); return buf; }
	return netman_config_user_path(buf, n);
}
static int h_cfg_set(const nc_cmd *c, int n, char **a)
{
	char path[512], err[512];
	if (n < 2) return nc_usage(c);
	if (netman_config_set_file(nc_cfg_path(path, sizeof path), a[0], a[1], err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
	printf("%s = %s  (%s)\n", a[0], a[1], path);
	return 0;
}
static int h_cfg_toggle(const nc_cmd *c, int n, char **a)
{
	char path[512], err[512];
	if (n < 1) return nc_usage(c);
	if (netman_config_toggle_file(nc_cfg_path(path, sizeof path), a[0], err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
	printf("%s toggled  (%s)\n", a[0], path);
	return 0;
}
static int h_fw_preset(const nc_cmd *c, int n, char **a)
{
	char *v[2] = { "firewall.preset", NULL };
	if (n < 1) return nc_usage(c);
	v[1] = a[0];
	return h_cfg_set(c, 2, v);
}

static int h_help(const nc_cmd *c, int n, char **a);   /* help.c */

/* ---- the table ----------------------------------------------------------------------------------------------------- */

#define R_NO NM_ROOT_NO
#define R_YES NM_ROOT_YES
#define R_WPA NM_ROOT_WPA
#define R_NEV NM_ROOT_NEVER
static const nc_cmd nc_table[] = {
	{ "status", "", h_status, R_NEV, NULL, "interfaces, Wi-Fi, Bluetooth and VPN at a glance (the default)", "" },
	{ "wifi list", "", h_wifi_list, R_WPA, "wifi", "scan and list visible networks (hidden ones as <hidden> + BSSID)", "Security is shown as OPEN, WEP, WPA, WPA2, WPA3 or 802.1X; the signal as dBm and percent." },
	{ "wifi scan", "", h_wifi_scan, R_WPA, "wifi", "the same list as TSV for scripts", "Columns: FLAG KNOWN SEC DBM PCT BSSID CHAN BAND NBSS HIDDEN NAME. Names are escaped (\\xNN for control bytes)." },
	{ "wifi info", "[SSID]", h_wifi_info, R_WPA, "wifi", "details of a network (default: the connected one)", "SSID, BSSIDs, security, channel and band, signal, rates, known/autoconnect, last connected." },
	{ "wifi connect", "SSID [PASSPHRASE] | --eap SSID IDENTITY PASSWORD", h_wifi_connect, R_WPA, "wifi", "join a network; 802.1X with --eap", "Without a passphrase a known or open network is joined. A wrong passphrase is reported as such." },
	{ "wifi disconnect", "", h_wifi_disconnect, R_WPA, "wifi", "leave the current network", "" },
	{ "wifi forget", "SSID", h_wifi_forget, R_WPA, "wifi", "remove a saved network (iwd only)", "" },
	{ "wifi wps", "pbc|pin [PIN]|cancel", h_wifi_wps, R_WPA, "wifi", "WPS push-button or PIN", "" },
	{ "wifi hotspot", "start SSID [PASSPHRASE] | stop", h_wifi_hotspot, R_WPA, "wifi", "share a connection as an access point", "" },
	{ "wifi known", "[autoconnect SSID on|off]", h_wifi_known, R_WPA, "wifi", "saved networks, and their autoconnect switch", "" },
	{ "bt list", "", h_bt_list, R_NO, "bluetooth", "known and discovered devices", "" },
	{ "bt scan", "[SECONDS]", h_bt_scan, R_NO, "bluetooth", "discover devices, then list", "" },
	{ "bt info", "NAME_OR_MAC", h_bt_info, R_NO, "bluetooth", "details of a device: class, RSSI, battery, profiles", "" },
	{ "bt connect", "NAME_OR_MAC", h_bt_action, R_NO, "bluetooth", "connect a paired device", "" },
	{ "bt disconnect", "NAME_OR_MAC", h_bt_action, R_NO, "bluetooth", "disconnect a device", "" },
	{ "bt pair", "NAME_OR_MAC [--pin PIN]", h_bt_action, R_NO, "bluetooth", "pair a device (--pin answers every prompt)", "" },
	{ "bt trust", "NAME_OR_MAC", h_bt_action, R_NO, "bluetooth", "trust a device", "" },
	{ "bt remove", "NAME_OR_MAC", h_bt_action, R_NO, "bluetooth", "forget a device", "" },
	{ "bt power", "on|off", h_bt_power, R_NO, "bluetooth", "switch the adapter", "" },
	{ "bt agent", "[SECONDS]", h_bt_agent, R_NO, "bluetooth", "run the pairing agent (PIN/passkey prompts on this terminal)", "" },
	{ "net list", "", h_net_list, R_NO, "net", "interfaces with addresses, gateways, routes", "" },
	{ "net show", "IFACE", h_net_show, R_NO, "net", "one interface with link speed and live rx/tx rate", "" },
	{ "net up", "IFACE", h_net_up, R_YES, "net", "set the link up", "" },
	{ "net down", "IFACE", h_net_down, R_YES, "net", "set the link down", "" },
	{ "net set", "IFACE ADDR/LEN [GATEWAY]", h_net_set, R_YES, "net", "static address (and default gateway)", "" },
	{ "net dhcp", "IFACE", h_net_dhcp, R_YES, "net", "get a lease with the built-in DHCP client and apply it", "Needs net.dhcp = yes." },
	{ "net dns", "IFACE NS... | IFACE clear", h_net_dns, R_YES, "net", "set or clear DNS servers", "Where they go is config net.dns_mode." },
	{ "net profile list", "", h_prof_list, R_NO, "net", "saved profiles (/etc/netman/NAME.conf)", "" },
	{ "net profile save", "NAME IFACE dhcp|ADDR/LEN [GATEWAY [DNS...]]", h_prof_save, R_YES, "net", "create or replace a profile", "" },
	{ "net profile apply", "NAME", h_prof_apply, R_YES, "net", "bring a profile up", "" },
	{ "roam status", "", h_roam_status, R_NO, "roaming", "probe setting and the score of each live interface", "" },
	{ "roam prefer", "PROFILE PRIORITY", h_roam_prefer, R_YES, "roaming", "set a profile priority (higher wins)", "" },
	{ "vpn list", "", h_vpn, R_NO, "vpn", "WireGuard and OpenVPN profiles and their state", "" },
	{ "vpn import", "FILE [NAME]", h_vpn, R_YES, "vpn", "copy a WireGuard / OpenVPN config into the profile dir (/etc/netman/vpn, mode 0600)", "Refuses to overwrite. Directory override: env NETMAN_VPN_CONF_DIR." },
	{ "vpn info", "NAME", h_vpn, R_YES, "vpn", "everything known about one profile: endpoint, handshake, transfer, allowed ips, split or full tunnel, kill switch", "" },
	{ "vpn up", "NAME [WAIT_S]", h_vpn, R_YES, "vpn", "bring a tunnel up (plus the kill switch if vpn.killswitch)", "" },
	{ "vpn down", "NAME", h_vpn, R_YES, "vpn", "take a tunnel down", "" },
	{ "vpn status", "[NAME]", h_vpn, R_NO, "vpn", "tunnel state, handshake, transfer", "" },
	{ "vpn run", "NAME COMMAND...", h_vpn, R_YES, "vpn", "run one command through the tunnel in its own namespace", "" },
	{ "vpn killswitch", "NAME on|off", h_vpn, R_YES, "vpn", "nftables kill switch for a tunnel", "" },
	{ "vpn providers", "", h_vpn, R_NO, "vpn", "supported providers (Mullvad, NordVPN)", "" },
	{ "vpn relays", "PROVIDER [COUNTRY]", h_vpn, R_NO, "vpn", "list a provider's relays (fetches with curl)", "" },
	{ "vpn connect", "PROVIDER PROFILE [COUNTRY [CITY]] [--fastest] [--via COUNTRY]", h_vpn, R_YES, "vpn", "write a profile for a provider relay", "" },
	{ "fw show", "", h_fw_show, R_YES, NULL, "the ruleset netman owns, as loaded", "" },
	{ "fw plan", "[PRESET]", h_fw_plan, R_NO, NULL, "print the ruleset that would be applied", "" },
	{ "fw apply", "[PRESET]", h_fw_apply, R_YES, "firewall", "apply atomically; rolls back unless confirmed", "Needs firewall.enabled = yes. firewall.confirm_s sets the window." },
	{ "fw diff", "[PRESET]", h_fw_diff, R_YES, NULL, "compare the plan with what was last applied", "" },
	{ "fw confirm", "", h_fw_confirm, R_YES, "firewall", "keep the applied rules", "" },
	{ "fw rollback", "", h_fw_rollback, R_YES, "firewall", "restore the previous rules now", "" },
	{ "fw preset", "workstation|lockdown", h_fw_preset, R_NEV, NULL, "set the default preset (config firewall.preset)", "" },
	{ "metrics show", "", h_metrics_show, R_NO, "metrics", "per-interface rates and totals", "" },
	{ "metrics record", "", h_metrics_record, R_YES, "metrics", "append one sample per interface to metrics.dir", "" },
	{ "metrics watch", "[COUNT]", h_metrics_watch, R_NO, "metrics", "print rates every metrics.interval_s seconds", "" },
	{ "ent list", "", h_ent_list, R_NO, "enterprise", "saved enterprise profiles (bond, bridge, vlan, route, ecmp, tether ...)", "" },
	{ "ent show", "NAME", h_ent_show, R_YES, "enterprise", "live state of a profile's device", "" },
	{ "ent save", "NAME KIND [KEY=VALUE...]", h_ent_save, R_YES, "enterprise", "create or replace a profile", "KIND: bond team bridge vlan macvlan ipvlan veth dummy tether dot1x route rule ecmp link ipv6 wol. Keys are the profile keys, e.g. `ent save bond0 bond mode=802.3ad member=eth0 member=eth1 miimon=100`. Stored as NAME.ent in the netman config directory." },
	{ "ent add", "NAME", h_ent_add, R_YES, "enterprise", "create the device / install the routes of a profile", "Needs enterprise.enabled = yes. Everything goes through rtnetlink, ethtool ioctls and /proc/sys; only nft, dnsmasq and wpa_supplicant are ever exec'd, and only for tether and 802.1X." },
	{ "ent del", "NAME", h_ent_del, R_YES, "enterprise", "tear a profile's device / routes down", "" },
	{ "ent apply", "", h_ent_apply, R_YES, "enterprise", "apply every saved profile in dependency order", "dummy/veth, bond/team, bridge, vlan/macvlan/ipvlan, link/ipv6/wol, route/rule/ecmp, tether/dot1x." },
	{ "ent ecmp", "NAME", h_ent_ecmp, R_YES, "enterprise", "run the multi-uplink failover loop for an ecmp profile", "Probes each uplink with an ICMP echo bound to its interface; fail_after / ok_after consecutive results flip it, the default route (metric 50) follows. Ctrl-C stops." },
	{ "ent tether", "", h_ent_tether, R_NO, "tether", "detected USB (RNDIS/NCM/ECM/ipheth) and Bluetooth PAN tethering devices", "" },
	{ "wol", "IFACE MAC", h_wol, R_YES, "enterprise", "send a Wake-on-LAN magic packet out of IFACE", "UDP broadcast to port 9. Setting the NIC itself to wake on LAN is `ent save NAME wol wol=g`." },
	{ "licence status", "", h_lic_status, R_NO, "licence", "licences and their expiry (exit 1 when any expires soon or has)", "Files NAME.lic in enterprise.licence_dir. Warns inside enterprise.licence_warn_days." },
	{ "licence renew", "NAME", h_lic_renew, R_YES, "licence", "renew a licence through enterprise.renew_hook", "The hook gets NAME CURRENT_PATH NEW_PATH and writes the new licence; netman checks it names the same licence and expires later, then swaps atomically and keeps NAME.lic.prev. netman itself never contacts a network." },
	{ "key rotate", "NAME", h_key_rotate, R_YES, "licence", "new WireGuard key for NAME; prints the new public key", "Writes NAME.key mode 0600, keeps the old one as NAME.key.prev, then runs enterprise.key_apply_hook (NAME PUBKEY); if the hook fails the old key is restored. No `wg` binary needed." },
	{ "key show", "NAME", h_key_show, R_YES, "licence", "public key of NAME.key", "" },
	{ "config show", "", h_cfg_show, R_NEV, NULL, "every setting with its value and where it came from", "" },
	{ "config check", "", h_cfg_check, R_NEV, NULL, "validate the config files and environment", "Errors are FILE:LINE with a did-you-mean for unknown keys." },
	{ "config set", "SECTION.KEY VALUE", h_cfg_set, R_NEV, NULL, "edit the user config file (atomic, comments kept)", "" },
	{ "config toggle", "SECTION.KEY", h_cfg_toggle, R_NEV, NULL, "flip a yes/no setting", "" },
	{ "config example", "", h_cfg_example, R_NEV, NULL, "print the documented example config", "" },
	{ "help", "[TOPIC]", h_help, R_NEV, NULL, "this list, or the help for one command or group", "" },
	{ "-tui", "", NULL, R_WPA, NULL, "the terminal UI (also --tui, -t); ? inside shows help", "" },
};
#define NC_NTABLE ((int)(sizeof nc_table / sizeof *nc_table))

/* number of words of c->name that match argv[0..] (all of them) or 0 */
static int nc_words(const nc_cmd *c, int argc, char **argv)
{
	const char *p = c->name; int w = 0;
	while (*p) {
		size_t l = strcspn(p, " ");
		if (w >= argc || strlen(argv[w]) != l || strncmp(argv[w], p, l)) return 0;
		w++; p += l; if (*p == ' ') p++;
	}
	return w;
}
static const nc_cmd *nc_match(int argc, char **argv, int *words)
{
	const nc_cmd *best = NULL; int bw = 0;
	for (int i = 0; i < NC_NTABLE; i++) { int w = nc_words(&nc_table[i], argc, argv); if (w > bw) { bw = w; best = &nc_table[i]; } }
	*words = bw;
	return best;
}

#endif
