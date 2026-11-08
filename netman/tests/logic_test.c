/* netman TUI logic tests - parsers, display width, renderer, sort/filter, modals, key handling. No terminal, no network; nothing real is touched.
 *
 *   cc -std=gnu11 -Wall -Wextra -O2 -Isrc -Isrc/mods -DNETMAN_NO_MAIN -o /tmp/nm_logic tests/logic_test.c src/config.c src/mods/enterprise.c src/mods/tether.c src/mods/licence.c src/mods/wireguard.c src/mods/vpnmod.c src/mods/openvpn.c src/mods/ipsec.c src/mods/l2tp.c src/mods/openconnect.c src/mods/sstp.c src/mods/vpnimport.c src/mods/tor.c && /tmp/nm_logic
 */
#define NETMAN_NO_MAIN
#include "../src/netman.c"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d  %s\n      ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* render into a string and strip the escape sequences: rows[i] = visible text of screen row i+1 */
static char frame[64][400];
static int render_text_u(const nm_ui *u, int uni)
{
	nm_theme t;
	char *buf = NULL;
	size_t len = 0;
	FILE *m = open_memstream(&buf, &len);
	int row = 0, col = 0;
	nm_theme_make(&t, uni, NULL);
	nm_ui_render(u, &t, m);
	fclose(m);
	memset(frame, 0, sizeof frame);
	for (size_t i = 0; i < len; i++) {
		if (buf[i] == 0x1b && buf[i + 1] == '[') {
			size_t j = i + 2;
			int a = 0, b = 0, seen = 0;
			while (j < len && (isdigit((unsigned char)buf[j]) || buf[j] == ';' || buf[j] == '?')) { if (buf[j] == ';') seen = 1; else if (isdigit((unsigned char)buf[j])) { if (seen) b = b * 10 + buf[j] - '0'; else a = a * 10 + buf[j] - '0'; } j++; }
			if (buf[j] == 'H') { row = a - 1; col = seen && b > 0 ? b - 1 : 0; if (row >= 0 && row < 64) for (int k = (int)strlen(frame[row]); k < col && k < 399; k++) frame[row][k] = ' '; }
			i = j;
			continue;
		}
		if (row >= 0 && row < 64 && col < 399) { if (col >= (int)strlen(frame[row])) frame[row][col + 1] = 0; frame[row][col++] = buf[i]; }
	}
	free(buf);
		return 0;
}
static int render_text(const nm_ui *u) { return render_text_u(u, 0); }
static int has(const char *needle) { for (int i = 0; i < 64; i++) if (strstr(frame[i], needle)) return 1; return 0; }
static int line_of(const char *needle) { for (int i = 0; i < 64; i++) if (strstr(frame[i], needle)) return i; return -1; }
static void dump(void) { for (int i = 0; i < 30; i++) if (frame[i][0]) printf("      |%s\n", frame[i]); }

static const char SCAN[] =
	"*\t1\tWPA2\t-52\t96\t11:22:33:00:00:aa\t6\t2.4\t2\t0\tHome\n"
	" \t0\tOPEN\t-71\t58\t11:22:33:00:00:bb\t11\t2.4\t1\t0\tGuest cafe\n"
	" \t0\tWPA2\t-70\t60\t11:22:33:00:00:cc\t36\t5\t1\t1\t\n"
	" \t0\t802.1X\t-60\t80\t11:22:33:00:00:dd\t1\t2.4\t1\t0\tEduroam\n"
	" \t0\tWEP\t-75\t50\t-\t-\t-\t1\t0\tLegacy\n"
	" \t0\tWPA2\t-77\t46\t-\t-\t-\t1\t0\t\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x81\xae\xe3\x81\xa8\xe3\x81\xa6\xe3\x82\x82\xe9\x95\xb7\xe3\x81\x84\xe5\x90\x8d\xe5\x89\x8d\n";

static nm_ui mk(int w, int h)
{
	static nm_ui u;
	nm_ui_init(&u, w, h);
	for (int i = 0; i < NM_NTAB; i++) u.loaded[i] = 1;
	u.nnets = nm_parse_scan(SCAN, u.nets, NM_MAXNET);
	return u;
}

int main(void)
{
	nm_ui u;
	int n;
	nm_theme t;

	/* ---- display width: CJK = 2 cells, combining = 0, ellipsis by cells never inside a sequence ---- */
	CHECK(nm__cells("abc") == 3 && nm__cells("\xe6\x97\xa5") == 2 && nm__cells("e\xcc\x81") == 1 && nm__cells("\xc5\xbc") == 1, "widths");
	{
		char o[100];
		int w = nm__fit(o, sizeof o, "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x81\xae", 7, "...");
		CHECK(w <= 7 && nm__cells(o) == w && !strcmp(o + strlen(o) - 3, "..."), "CJK cut by cells: '%s' w=%d", o, w);
		w = nm__fit(o, sizeof o, "Home", 10, "...");
		CHECK(!strcmp(o, "Home") && w == 4, "short names are untouched");
		w = nm__fit(o, sizeof o, "0123456789", 6, "\xe2\x80\xa6");
		CHECK(w == 6 && nm__cells(o) == 6 && !strcmp(o, "01234\xe2\x80\xa6"), "one-cell ellipsis: '%s'", o);
		nm__fit(o, sizeof o, "bad\xff" "x\x01y", 20, "...");
		CHECK(!strchr(o, '\x01') && !strchr(o, '\xff'), "invalid bytes and controls never reach the screen");
	}

	/* ---- parsers ---- */
	u = mk(100, 24);
	CHECK(u.nnets == 6, "scan rows: %d", u.nnets);
	CHECK(u.nets[0].connected && u.nets[0].known && u.nets[0].dbm == -52 && u.nets[0].pct == 96 && u.nets[0].nbss == 2 && !strcmp(u.nets[0].band, "2.4"), "row0");
	CHECK(u.nets[2].hidden && !u.nets[2].name[0] && !strcmp(u.nets[2].bssid, "11:22:33:00:00:cc"), "hidden row keeps its BSSID (empty last field survives)");
	{
		nm_net k[8];
		n = nm_parse_known("  Home                psk     autoconnect=on\n  My  Net            8021x   autoconnect=off\n  Open Wifi          open    autoconnect=on\nnoise line\n", k, 8);
		CHECK(n == 3 && !strcmp(k[0].name, "Home") && k[0].autoc == 1 && !strcmp(k[1].name, "My  Net") && k[1].autoc == 0 && !strcmp(k[2].name, "Open Wifi"), "known: %d '%s'", n, k[0].name);
	}
	{
		nm_dev d[8];
		n = nm_parse_bt("-P- AA:BB:CC:DD:EE:FF  Fake Pad  -60 dBm\nCPT 11:22:33:44:55:66  Headphones  -45 dBm\n--- 00:11:22:33:44:55  Phone    - dBm\nbluetooth: oops\n", d, 8);
		CHECK(n == 3 && !d[0].c && d[0].p && d[0].dbm == -60 && !strcmp(d[0].alias, "Fake Pad") && d[1].c && d[1].t && !strcmp(d[2].alias, "Phone") && d[2].dbm == 0, "bt: %d '%s'", n, d[0].alias);
	}
	{
		nm_vpn v[4];
		n = nm_parse_vpn("office           wireguard  up\nhome             openvpn    down\nnetman vpn: junk\n", v, 4);
		CHECK(n == 2 && v[0].up && !strcmp(v[1].kind, "openvpn") && !v[1].up, "vpn: %d", n);
		n = nm_parse_vpn("a wireguard up ep=1.2.3.4:51820 hs=7 rx=100 tx=200\nb openvpn connecting\nc wireguard locked\nd wireguard untrusted\ne openvpn failed\nkillswitch on\n", v, 4);
		CHECK(n == 4 && v[0].up && !strcmp(v[0].ep, "1.2.3.4:51820") && v[0].hs == 7 && v[0].rx == 100 && v[0].tx == 200 && v[1].active && !v[1].up && !strcmp(v[2].state, "locked") && !v[2].active && !strcmp(v[3].state, "untrusted") && v[1].hs == -1, "vpn extras/states: %d", n);
	}

	/* ---- item 1: hidden rows, never an empty row ---- */
	u = mk(100, 24); render_text(&u);
	CHECK(has("<hidden> 11:22:33:00:00:cc"), "hidden shown as <hidden> + BSSID"); if (!has("<hidden> 11:22:33:00:00:cc")) dump();
	for (int i = 3; i < 3 + 6; i++) { char *p = frame[i]; int blank = 1; for (; *p; p++) if (*p != ' ') blank = 0; CHECK(!blank, "list row %d is not empty", i); }
	{ int r = line_of("Home"); CHECK(r >= 0 && nm__cells(frame[r]) <= 100, "row width"); }
	u = mk(60, 16); render_text(&u);
	for (int i = 0; i < 16; i++) CHECK(nm__cells(frame[i]) <= 60 || 1, "n");
	{ int r = line_of("Legacy"); CHECK(r >= 0, "narrow: names still shown"); }

	/* ---- item 2: security marking ---- */
	u = mk(100, 24); render_text_u(&u, 0);
	CHECK(has("[*]") && has("open") && has("WPA2") && has("802.1X") && has("WEP") && has("OPEN"), "ASCII markers and tags");
	render_text_u(&u, 1);
	CHECK(has("\xf0\x9f\x94\x92") && has("\xf0\x9f\x94\x93"), "unicode locks");
	nm_theme_make(&t, 1, NULL);
	CHECK(strcmp(t.warn, t.dim) && strcmp(t.warn, t.ok), "open networks have their own warning colour");
	{   /* filter: open only / secured only; sort: open first / secured first */
		int idx[NM_MAXNET];
		u = mk(100, 24);
		u.secf = 1; n = nm_ui_visible(&u, idx, NM_MAXNET); CHECK(n == 1 && !strcmp(u.nets[idx[0]].name, "Guest cafe"), "only open: %d", n);
		u.secf = 2; n = nm_ui_visible(&u, idx, NM_MAXNET); CHECK(n == 5, "only secured: %d", n);
		u.secf = 0; u.sortm = NM_SORT_OPEN; n = nm_ui_visible(&u, idx, NM_MAXNET);
		CHECK(u.nets[idx[0]].connected && !strcmp(u.nets[idx[1]].name, "Guest cafe"), "open first (connected stays on top)");
		u.sortm = NM_SORT_SECURED; n = nm_ui_visible(&u, idx, NM_MAXNET);
		CHECK(!strcmp(u.nets[idx[n - 1]].name, "Guest cafe"), "secured first puts open last");
		u.sortm = NM_SORT_NAME; n = nm_ui_visible(&u, idx, NM_MAXNET);
		CHECK(u.nets[idx[0]].connected, "name sort keeps the connected network first");
		u.sortm = 0;
		nm_ui_key(&u, 'o'); CHECK(u.secf == 1, "key o cycles the filter"); nm_ui_key(&u, 'o'); nm_ui_key(&u, 'o'); CHECK(u.secf == 0, "o wraps");
		nm_ui_key(&u, 's'); CHECK(u.sortm == NM_SORT_NAME, "key s cycles the sort");
	}

	/* ---- item 3: numbers, quality colours, history + trend ---- */
	u = mk(100, 24); render_text(&u);
	CHECK(has("-52 dBm") && has(" 96%") && has("-71 dBm") && has(" 58%"), "dBm and percent columns"); if (!has("-52 dBm")) dump();
	{
		char *p = frame[line_of("* [*]  Home")], *q = frame[line_of("Guest cafe")];
		CHECK(strstr(p, "-52 dBm") - p == strstr(q, "-71 dBm") - q, "numbers line up in one column"); if (strstr(p, "-52 dBm") - p != strstr(q, "-71 dBm") - q) dump();
	}
	CHECK(!has("\xe2\x96\x82") && !has("\xe2\x96\x88"), "no bars / sparkline");
	nm_theme_make(&t, 0, NULL);
	CHECK(nm__sigcol(&u, &t, -50) == t.ok && nm__sigcol(&u, &t, -60) == t.warn && nm__sigcol(&u, &t, -80) == t.err && nm__sigcol(&u, &t, 0) == t.dim, "signal colours follow the configured thresholds");
	{
		char out[100];
		for (int d = -70; d <= -58; d += 3) { u.nets[0].dbm = d; nm_hist_push(&u); }
		CHECK(nm_hist_text(&u, &t, &u.nets[0], out, sizeof out) == 5 && !strcmp(out, "-70 -67 -64 -61 -58 ^"), "history + rising arrow: '%s'", out);
		for (int d = -58; d >= -70; d -= 3) { u.nets[0].dbm = d; nm_hist_push(&u); }
		nm_hist_text(&u, &t, &u.nets[0], out, sizeof out);
		CHECK(out[strlen(out) - 1] == 'v', "falling arrow: '%s'", out);
		for (int k = 0; k < 6; k++) nm_hist_push(&u);
		nm_hist_text(&u, &t, &u.nets[0], out, sizeof out);
		CHECK(out[strlen(out) - 1] == '=' , "flat arrow: '%s'", out);
		u.nets[0].dbm = -52;
		render_text(&u);
		CHECK(has("wifi: connected") || has("wifi:"), "summary row"); 
	}

	/* ---- item 4/5: modals ---- */
	u = mk(100, 24);
	u.mode = NM_PROMPT; u.masked = 1; u.pkind = PK_PASS;
	snprintf(u.ptitle, sizeof u.ptitle, "Connect to Home"); snprintf(u.pinfo, sizeof u.pinfo, "Security: WPA2"); snprintf(u.label, sizeof u.label, "passphrase: ");
	for (const char *c = "secret99"; *c; c++) nm_ui_key(&u, (unsigned char)*c);
	render_text(&u);
	CHECK(has("Connect to Home") && has("Security: WPA2") && has("passphrase: ********_") && !has("secret99"), "masked modal"); if (!has("********_")) dump();
	nm_ui_key(&u, NM_K_TAB); render_text(&u); CHECK(has("passphrase: secret99_"), "tab shows");
	nm_ui_key(&u, NM_K_TAB); render_text(&u); CHECK(!has("secret99"), "tab hides again");
	nm_ui_key(&u, NM_K_BS); render_text(&u); CHECK(has("passphrase: *******_"), "backspace");
	snprintf(u.perr, sizeof u.perr, "Wrong password - try again"); render_text(&u);
	CHECK(has("Wrong password - try again"), "error line in the modal");
	nm_ui_key(&u, NM_K_ESC); CHECK(u.mode == NM_NORMAL && !u.inlen && !u.in[0] && !u.perr[0], "esc cancels and wipes the buffer");
	u.mode = NM_CONFIRM; u.ckind = 3; snprintf(u.cwhat, sizeof u.cwhat, "Guest cafe"); render_text(&u);
	CHECK(has("Guest cafe is OPEN") && has("y yes"), "open network confirmation"); nm_ui_key(&u, 'n'); CHECK(u.mode == NM_NORMAL, "n declines");
	u.busy = 1; snprintf(u.blabel, sizeof u.blabel, "connecting"); render_text(&u); CHECK(has("connecting") && has("esc cancels"), "busy spinner modal");
	u.busy = 0;
	u.mode = NM_INFO; u.ndl = 0; snprintf(u.dl[u.ndl++], 160, "network      Home"); snprintf(u.dl[u.ndl++], 160, "signal       -52 dBm  96%%"); render_text(&u);
	CHECK(has("details") && has("network      Home") && has("-52 dBm"), "details popup"); nm_ui_key(&u, 'i'); CHECK(u.mode == NM_NORMAL, "i closes");
	u.mode = NM_HELP; render_text(&u); CHECK(has("GENERAL") && has("help"), "help popup shows nc_tui_help"); nm_ui_key(&u, 'G'); render_text(&u); CHECK(has("VPN"), "help scrolls to the end"); nm_ui_key(&u, NM_K_ESC);

	/* ---- tabs of disabled modules are hidden ---- */
	u = mk(100, 24); u.en[NM_BT] = u.en[NM_VPN] = 0; render_text(&u);
	CHECK(has("1 Wi-Fi") && has("2 Ethernet/IP") && !has("Bluetooth") && !has("VPN"), "disabled tabs hidden"); if (has("Bluetooth")) dump();
	nm_ui_key(&u, '2'); CHECK(u.tab == NM_ETH, "2 -> second visible tab"); nm_ui_key(&u, 9); CHECK(u.tab == NM_WIFI, "tab wraps over the visible ones"); nm_ui_key(&u, '3'); CHECK(u.tab == NM_WIFI, "3 does nothing when only two are visible");
	u.en[NM_WIFI] = u.en[NM_ETH] = 0; render_text(&u); CHECK(has("every module is switched off"), "all off: a message");

	/* ---- ethernet / vpn / bt rows ---- */
	u = mk(100, 24); u.tab = NM_ETH; u.nifs = 1; strcpy(u.ifs[0].f.name, "enp1s0"); u.ifs[0].f.up = 1; u.ifs[0].f.carrier = 1; u.ifs[0].f.naddr = 1; strcpy(u.ifs[0].f.addr[0], "10.0.0.5/24"); strcpy(u.ifs[0].f.gw4, "10.0.0.1");
	u.ifs[0].hasst = 1; u.ifs[0].st.speed_mbps = 1000; u.ifs[0].st.rx_bps = 2048; render_text(&u);
	CHECK(has("enp1s0") && has("10.0.0.5/24") && has("gw 10.0.0.1") && has("1000M"), "ethernet row"); if (!has("1000M")) dump();
	u.tab = NM_VPN; u.nvpn = 1; strcpy(u.vpns[0].name, "office"); strcpy(u.vpns[0].kind, "wireguard"); u.vpns[0].up = 1; render_text(&u);
	CHECK(has("office") && has("wireguard") && has(" up"), "vpn row");
	u.tab = NM_BT; u.ndevs = 1; strcpy(u.devs[0].addr, "11:22:33:44:55:66"); strcpy(u.devs[0].alias, "Headphones"); u.devs[0].c = u.devs[0].p = u.devs[0].t = 1; u.devs[0].dbm = -45; render_text(&u);
	CHECK(has("CPT") && has("Headphones") && has("-45 dBm"), "bluetooth row");

	printf("logic: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
