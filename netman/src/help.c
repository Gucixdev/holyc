/* help.c - help for the CLI, the man page and the TUI popup, all generated from the nc_table in cli.h (+ the NC_KEYS table in config.h).
 * Included by netman.c after cli.h (one translation unit); nothing here is typed twice.
 *
 *   netman help              every command, grouped
 *   netman help wifi         one group;  netman help wifi connect   one command in full;  netman help tui   the TUI keys;  netman help config   the settings
 *   netman help --man        the man page (src/netman.1 is this output; tests/cli_test.c checks they match)
 */

/* the TUI keys: the popup (?) shows exactly these lines; lines that start in column 0 are headings */
static const char *const nc_tui_help[] = {
	"GENERAL",
	"  tab / shift-tab, h l, 1-5   switch tab (tabs of modules disabled in config are hidden)",
	"  j k, arrows, PgUp PgDn      move;  g G  first / last",
	"  /                           filter by name",
	"  r                           refresh / rescan",
	"  i                           details popup for the selected row;  I  toggle the big \"all info\" view",
	"                              (replaces the list; j k pick another row, PgUp PgDn scroll, esc / I close)",
	"  ?                           this help;  q  quit",
	"WI-FI",
	"  enter    connect: known networks join at once; others ask for the passphrase",
	"           (masked; tab shows/hides it); open networks ask for confirmation;",
	"           802.1X asks identity then password; a wrong password says so and offers a retry",
	"  i        details: BSSIDs, security (pairwise / group / AKM), WPS, channel/width, signal, noise, rx/tx bitrate,",
	"           first/last seen, roam history, IP/gateway/DNS/lease, link speed, rx/tx;  I  the same plus the raw backend text",
	"  s        cycle sort: signal / name / secured first / open first;  o  all / only open / only secured",
	"  d x a    disconnect / forget / toggle autoconnect;  w  WPS;  H  hotspot",
	"  Signal is dBm and percent (green good, yellow ok, red weak); a lock marks security,",
	"  OPEN networks get a warning colour. Without Unicode: [*] and open.",
	"BLUETOOTH",
	"  enter conn/disc   p pair   t trust   x remove   s scan   o power   i details   I all info (adds the adapter:",
	"  powered, discoverable, pairable, paired/connected counts)",
	"ETHERNET / IP",
	"  addresses, gateway, DNS, lease, link speed and live rx/tx per interface;  i  details: driver, MAC, MTU,",
	"  speed/duplex, link detected, IPv4/IPv6, DHCP server and lease times, rx/tx rates and errors;  I  adds sysfs",
	"VPN",
	"  Profiles are <dir>/NAME.conf (WireGuard) or NAME.ovpn; dir = /etc/netman/vpn or env NETMAN_VPN_CONF_DIR.",
	"  An empty list names the directory; add a profile with  netman vpn import FILE [NAME].",
	"  enter    up / down the selected profile (also turns the kill switch on when vpn.killswitch = yes)",
	"  K        toggle the nftables kill switch;  i  details: endpoint, allowed ips, handshake age, transfer, split tunnel",
	"  States: up, down, connecting, failed; locked = unreadable (run as root), untrusted = file must be root/self",
	"  owned and not group/world writable. Creating tunnels needs root: with VPN profiles present the TUI asks sudo.",
	"  r        refresh",
	"ENTERPRISE (only with enterprise.enabled = yes)",
	"  read-only status: bonds, bridges, vlans, uplinks (ECMP), tethering (when tether.enabled) and licences",
	"  (when licence.enabled), from the saved .ent profiles and the live kernel state;  i  details of the selected",
	"  row;  I  the same plus the raw profile text. Changes are made with  netman ent ... / licence ... / key ...",
	"HEADER (row 2, every tab)",
	"  wifi / bt status, hostname, default route (iface via gateway), ip (pub = a public address), dns,",
	"  and the network score 0-100 with its reasons (rtt, loss, dBm). The score is measured in the background",
	"  (3 pings to the gateway); \"ping n/a\" = the kernel does not allow unprivileged ICMP. A field the system",
	"  does not report is shown as -. On a narrow terminal the lowest-priority fields are dropped.",
};
#define NC_NTUIHELP ((int)(sizeof nc_tui_help / sizeof *nc_tui_help))

/* one-paragraph notes per command group: `netman help GROUP` and the man page */
static const struct { const char *group, *text; } nc_group_notes[] = {
	{ "ent", "enterprise networking: bonding/teaming, bridges with STP and vlan filtering, vlans and QinQ, macvlan/ipvlan, veth, routes, rules and tables, ECMP with failover, link settings, IPv6 prefix delegation, USB/Bluetooth tethering, wired 802.1X. "
	         "Profiles are NAME.ent files (\"key = value\", same syntax as net profiles) in the netman config directory. "
	         "team is bonding: the kernel team driver is deprecated, so kind=team maps runner= (activebackup, lacp, loadbalance, broadcast, roundrobin) onto the bonding driver. "
	         "Tethering share-out uses its own nft table `inet netman_tether` (firewall.h replaces `inet netman`, so the two never clobber each other); an accept there cannot override a drop in another table, so a strict firewall must allow the downstream interface itself. "
	         "Changing commands need enterprise.enabled = yes." },
	{ "licence", "licence files NAME.lic with expiry, the expiry check (exit 1 when anything expires soon), and hook-driven renewal. Switched by licence.enabled." },
	{ "key", "WireGuard key rotation (NAME.key in enterprise.licence_dir). Switched by licence.enabled." },
	{ "wol", "Wake-on-LAN magic packet. Switched by enterprise.enabled." },
};
#define NC_NGROUPNOTES ((int)(sizeof nc_group_notes / sizeof *nc_group_notes))
static const char *nc_group_note(const char *g, int l)
{
	for (int i = 0; i < NC_NGROUPNOTES; i++) if ((int)strlen(nc_group_notes[i].group) == l && !strncmp(nc_group_notes[i].group, g, (size_t)l)) return nc_group_notes[i].text;
	return NULL;
}

static int nc_group_len(const char *name) { const char *s = strchr(name, ' '); return s ? (int)(s - name) : (int)strlen(name); }

static void nc_help_row(FILE *o, const nc_cmd *c, int width)
{
	char left[200];
	snprintf(left, sizeof left, "netman %s%s%s", c->name, *c->args ? " " : "", c->args);
	if ((int)strlen(left) > width) fprintf(o, "  %s\n  %*s%s\n", left, width + 2, "", c->sum);
	else fprintf(o, "  %-*s  %s\n", width, left, c->sum);
}

static void nc_help_config(FILE *o)
{
	fprintf(o, "settings (netman config show; netman config set SECTION.KEY VALUE; files /etc/netman/netman.conf and ~/.config/netman/netman.conf; env NETMAN_SECTION_KEY)\n");
	for (int k = 0; k < NC_N; k++) {
		char full[64];
		netman_config_fullname(k, full, sizeof full);
		fprintf(o, "  %-24s %-28s default %s\n      %s\n", full, netman_config_type(k), netman_config_default(k), netman_config_doc(k));
	}
}

static void nc_help_all(FILE *o)
{
	const char *last = "";
	int lastl = 0;
	fputs("netman - network and Bluetooth manager (a NetworkManager + bluez drop-in)\n", o);
	for (int i = 0; i < NC_NTABLE; i++) {
		const nc_cmd *c = &nc_table[i];
		int l = nc_group_len(c->name);
		if (l != lastl || strncmp(last, c->name, l)) { fputc('\n', o); last = c->name; lastl = l; }
		nc_help_row(o, c, 52);
	}
	fputs("\nnetman help GROUP|COMMAND for details; help tui for the TUI keys; help config for every setting.\n"
	      "Commands that need root re-run themselves through sudo (privileges.sudo). A module switched off in config is skipped with a message.\n", o);
}

static int nc_help_topic(FILE *o, const char *t)
{
	int found = 0, tl = (int)strlen(t);
	if (!strcmp(t, "tui")) { for (int i = 0; i < NC_NTUIHELP; i++) fprintf(o, "%s\n", nc_tui_help[i]); return 0; }
	if (!strcmp(t, "config") || !strcmp(t, "settings")) { nc_help_config(o); return 0; }
	for (int i = 0; i < NC_NTABLE; i++) {   /* an exact command: full text */
		const nc_cmd *c = &nc_table[i];
		if (!strcmp(c->name, t)) {
			fprintf(o, "netman %s%s%s\n  %s\n", c->name, *c->args ? " " : "", c->args, c->sum);
			if (*c->help) fprintf(o, "\n  %s\n", c->help);
			fprintf(o, "\n  needs root: %s\n", c->root == NM_ROOT_YES ? "yes (re-runs through sudo)" : c->root == NM_ROOT_WPA ? "only with wpa_supplicant when its socket is closed to you" : "no");
			if (c->module) fprintf(o, "  switched by config key %s.enabled\n", c->module);
			return 0;
		}
	}
	for (int i = 0; i < NC_NTABLE; i++) {   /* a group */
		const nc_cmd *c = &nc_table[i];
		if (nc_group_len(c->name) == tl && !strncmp(c->name, t, tl)) {
			if (!found && nc_group_note(t, tl)) fprintf(o, "%s\n\n", nc_group_note(t, tl));
			nc_help_row(o, c, 52); found = 1;
		}
	}
	return found ? 0 : 1;
}

/* roff text: escape \ and -, protect a leading . or ' */
static void nc_roff(FILE *o, const char *s)
{
	if (*s == '.' || *s == '\'') fputs("\\&", o);
	for (; *s; s++) { if (*s == '\\') fputs("\\e", o); else if (*s == '-') fputs("\\-", o); else fputc(*s, o); }
}

static void nc_help_man(FILE *o)
{
	fputs(".TH NETMAN 1 \"\" \"netman\" \"User Commands\"\n.SH NAME\nnetman \\- network and Bluetooth manager, a NetworkManager and bluez drop-in\n"
	      ".SH SYNOPSIS\n.B netman\n[\\fICOMMAND\\fR [\\fIARGS\\fR]]\n.br\n.B netman \\-tui\n"
	      ".SH DESCRIPTION\nWithout a command netman prints the status of interfaces, Wi\\-Fi, Bluetooth and VPN.\n"
	      "Commands that need root re\\-run themselves through sudo (config key privileges.sudo).\n"
	      "A module switched off in the config file is skipped with a message such as \\(lqwifi disabled in config\\(rq.\n"
	      "This page is generated from the command table (\\fBnetman help \\-\\-man\\fR).\n.SH COMMANDS\n", o);
	for (int i = 0; i < NC_NTABLE; i++) {
		const nc_cmd *c = &nc_table[i];
		fprintf(o, ".TP\n.B netman ");
		nc_roff(o, c->name);
		if (*c->args) { fputs(" ", o); nc_roff(o, c->args); }
		fputs("\n", o);
		nc_roff(o, c->sum);
		if (*c->help) { fputs(". ", o); nc_roff(o, c->help); }
		fputs("\n", o);
	}
	fputs(".SH COMMAND GROUPS\n", o);
	for (int i = 0; i < NC_NGROUPNOTES; i++) { fputs(".TP\n.B ", o); nc_roff(o, nc_group_notes[i].group); fputs("\n", o); nc_roff(o, nc_group_notes[i].text); fputs("\n", o); }
	fputs(".SH TUI\nStart it with \\fBnetman \\-tui\\fR; \\fB?\\fR inside shows the same text:\n.nf\n", o);
	for (int i = 0; i < NC_NTUIHELP; i++) { nc_roff(o, nc_tui_help[i]); fputs("\n", o); }
	fputs(".fi\n.SH CONFIGURATION\nSettings come from defaults, \\fI/etc/netman/netman.conf\\fR, \\fI~/.config/netman/netman.conf\\fR and the environment variables "
	      "\\fBNETMAN_\\fISECTION\\fB_\\fIKEY\\fR, later ones win. \\fBnetman config show\\fR lists every value and where it came from; "
	      "\\fBnetman config check\\fR validates; \\fBnetman config set\\fR and \\fBtoggle\\fR edit the user file.\n", o);
	for (int k = 0; k < NC_N; k++) {
		char full[64];
		netman_config_fullname(k, full, sizeof full);
		fprintf(o, ".TP\n.B %s\n", full);
		nc_roff(o, netman_config_doc(k));
		fputs(" Default: ", o); nc_roff(o, netman_config_default(k)); fputs(".\n", o);
	}
	fputs(".SH FILES\n.I /etc/netman/netman.conf\n.br\n.I ~/.config/netman/netman.conf\n.br\n.I /etc/netman/NAME.conf\n(network profiles)\n.SH SEE ALSO\nnetman.conf.example\n", o);
}

static int h_help(const nc_cmd *c, int n, char **a)
{
	char t[128] = "";
	(void)c;
	if (n > 0 && !strcmp(a[0], "--man")) { nc_help_man(stdout); return 0; }
	if (n == 0) { nc_help_all(stdout); return 0; }
	for (int i = 0; i < n; i++) { if (i) strncat(t, " ", sizeof t - strlen(t) - 1); strncat(t, a[i], sizeof t - strlen(t) - 1); }
	if (nc_help_topic(stdout, t)) { fprintf(stderr, "netman: no help for '%s'\n", t); nc_help_all(stderr); return 2; }
	return 0;
}
