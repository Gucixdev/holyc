/* netmanager TUI logic tests - parsers, renderer, key handling. No terminal, no network, nothing real is touched.
 *
 *   cc -std=gnu11 -Wall -Wextra -O2 -I.. -I../../notcoreutils -o /tmp/nm_logic logic_test.c && /tmp/nm_logic
 */
#define _GNU_SOURCE
#define NMB_IMPLEMENTATION
#include "netmanager_blue.h"
#define NM_TUI_IMPLEMENTATION
#include "tui.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d  %s\n      ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* render into a string and strip the escape sequences: rows[i] = visible text of screen row i+1 */
static char frame[64][400];
static int render_text(const nm_ui *u, int w, int h)
{
	nm_theme t;
	char *buf = NULL;
	size_t len = 0;
	FILE *m = open_memstream(&buf, &len);
	int row = 0, col = 0;
	nm_theme_default(&t, 0);
	nm_ui_render(u, &t, m);
	fclose(m);
	memset(frame, 0, sizeof frame);
	for (size_t i = 0; i < len; i++) {
		if (buf[i] == 0x1b && buf[i + 1] == '[') {
			size_t j = i + 2;
			int a = 0, b = 0, seen = 0;
			while (j < len && (isdigit((unsigned char)buf[j]) || buf[j] == ';' || buf[j] == '?')) { if (buf[j] == ';') seen = 1; else if (isdigit((unsigned char)buf[j])) { if (seen) b = b * 10 + buf[j] - '0'; else a = a * 10 + buf[j] - '0'; } j++; }
			if (buf[j] == 'H') { row = a - 1; col = 0; (void)b; }
			i = j;
			continue;
		}
		if (row >= 0 && row < 64 && col < 399) frame[row][col++] = buf[i];
	}
	free(buf);
	(void)w; (void)h;
	return 0;
}
static int width_of(const char *s) { int n = 0; for (; *s; s++) if (((unsigned char)*s & 0xC0) != 0x80) n++; return n; }

int main(void)
{
	nm_net nets[16];
	nm_dev devs[8];
	nm_if ifs[8];
	char wifi[200], bt[200];
	int n;

	/* ---- parsers: the exact formats the library prints ---- */
	n = nm_parse_wifi("* Home                             psk     -52 dBm  (known)\n"
	                  "  Guest cafe                       open    -71 dBm\n"
	                  "  Odd  Name x                      8021x   -80 dBm\n"
	                  "wifi: cannot talk to wpa_supplicant (needs root or the netdev group)\n", nets, 16);
	CHECK(n == 3, "iwd list: %d rows", n);
	CHECK(!strcmp(nets[0].name, "Home") && nets[0].connected && nets[0].known && nets[0].dbm == -52 && !strcmp(nets[0].type, "psk"), "row0 %s %d", nets[0].name, nets[0].dbm);
	CHECK(!strcmp(nets[1].name, "Guest cafe") && !nets[1].connected && !strcmp(nets[1].type, "open") && nets[1].dbm == -71, "row1 %s", nets[1].name);
	CHECK(!strcmp(nets[2].name, "Odd  Name x") && !strcmp(nets[2].type, "8021x"), "row2 keeps inner spaces: '%s'", nets[2].name);
	n = nm_parse_wifi("  HomeNet                          psk  -45 dBm\n  Open one                         open -60 dBm\n  Hidden                           psk  -90 dBm\n", nets, 16);
	CHECK(n == 3 && !strcmp(nets[0].name, "HomeNet") && nets[0].dbm == -45 && !nets[0].connected, "wpa form: %d", n);
	n = nm_parse_wifi("  ghost psk -1 dBm\n", nets, 16);
	CHECK(n == 1, "short line");
	n = nm_parse_wifi("garbage\n\n   \n  no unit here psk -60\n  x -60 dBm\n", nets, 16);
	CHECK(n == 0, "garbage rejected, got %d", n);
	n = nm_parse_wifi("  a psk -60 dBm\n  b psk -61 dBm\n  c psk -62 dBm\n", nets, 2);
	CHECK(n == 2, "max respected");

	n = nm_parse_known("  Home                             psk    autoconnect=on\n  Café                             open   autoconnect=off\nno saved networks\n", nets, 16);
	CHECK(n == 2 && !strcmp(nets[0].name, "Home") && nets[0].autoc == 1 && !strcmp(nets[1].name, "Café") && nets[1].autoc == 0, "known iwd: %d", n);
	n = nm_parse_known("  Work                             autoconnect=off\n", nets, 16);
	CHECK(n == 1 && !strcmp(nets[0].name, "Work") && nets[0].autoc == 0 && nets[0].known, "known wpa form");

	n = nm_parse_bt("CPT AA:BB:CC:DD:EE:FF  Fake Pad  -60 dBm\n--- 11:22:33:44:55:66  Some Phone    - dBm\n-P- 00:11:22:33:44:55  Weird  name    - dBm\nnonsense\n", devs, 8);
	CHECK(n == 3, "bt rows %d", n);
	CHECK(devs[0].c && devs[0].p && devs[0].t && !strcmp(devs[0].addr, "AA:BB:CC:DD:EE:FF") && !strcmp(devs[0].alias, "Fake Pad") && devs[0].dbm == -60, "bt0 '%s' %d", devs[0].alias, devs[0].dbm);
	CHECK(!devs[1].c && !strcmp(devs[1].alias, "Some Phone") && devs[1].dbm == 0, "bt1 '%s' %d", devs[1].alias, devs[1].dbm);
	CHECK(devs[2].p && !devs[2].c && !strcmp(devs[2].alias, "Weird  name"), "bt2 '%s'", devs[2].alias);

	n = nm_parse_status("enp2s0   eth   down       94:c6:91:f1:f4:59   no cable/link\n"
	                    "wlan0    wifi  up         38:00:25:04:4e:e9 192.168.1.58\n"
	                    "wifi     connected to Home\n"
	                    "wifi     wlan0 is driven by wpa_supplicant (not iwd)\n"
	                    "bt       connected: Fake Pad\n"
	                    "bt       adapter on\n", ifs, 8, wifi, bt, sizeof wifi);
	CHECK(n == 2 && !strcmp(ifs[0].name, "enp2s0") && ifs[0].nolink && !ifs[0].ip[0] && !strcmp(ifs[1].kind, "wifi") && !strcmp(ifs[1].ip, "192.168.1.58"), "status ifs %d", n);
	CHECK(!strcmp(wifi, "connected to Home; wlan0 is driven by wpa_supplicant (not iwd)") && !strcmp(bt, "adapter on; connected: Fake Pad"), "summaries '%s' | '%s'", wifi, bt);

	CHECK(nm_bars(-40) == 4 && nm_bars(-60) == 3 && nm_bars(-70) == 2 && nm_bars(-80) == 1 && nm_bars(-95) == 0, "bars");

	/* ---- render: 80x24 and 40x12 ---- */
	{
		static nm_ui u;
		nm_ui_init(&u, 80, 24);
		u.tab = NM_WIFI;
		u.nnets = nm_parse_wifi("* Home                             psk     -52 dBm  (known)\n  Guest cafe                       open    -71 dBm\n", u.nets, NM_MAXNET);
		u.nets[0].autoc = 1;
		u.loaded[NM_WIFI] = 1;
		snprintf(u.swifi, sizeof u.swifi, "connected to Home");
		snprintf(u.sbt, sizeof u.sbt, "adapter on");
		render_text(&u, 80, 24);
		CHECK(strstr(frame[0], "1 Wi-Fi") && strstr(frame[0], "2 Ethernet/IP") && strstr(frame[0], "3 Bluetooth") && strstr(frame[0], "4 VPN") && strstr(frame[0], "netmanager"), "tabs: '%s'", frame[0]);
		CHECK(strstr(frame[1], "wifi: connected to Home") && strstr(frame[1], "bt: adapter on"), "summary '%s'", frame[1]);
		CHECK(strstr(frame[3], "Home") && strstr(frame[3], "psk") && strstr(frame[3], "-52 dBm") && strstr(frame[3], "saved auto") && frame[3][0] == '*', "row '%s'", frame[3]);
		CHECK(strstr(frame[4], "Guest cafe") && strstr(frame[4], "open"), "row2 '%s'", frame[4]);
		CHECK(strstr(frame[23], "enter connect") && strstr(frame[23], "q quit") && strstr(frame[23], "? help"), "keybar '%s'", frame[23]);
		for (int i = 0; i < 24; i++) CHECK(width_of(frame[i]) <= 80, "row %d too wide: %d", i, width_of(frame[i]));
		CHECK(!frame[24][0], "nothing past row 24");

		nm_ui_init(&u, 40, 12);   /* same data, small terminal */
		u.tab = NM_WIFI;
		u.nnets = nm_parse_wifi("* A very long network name that cannot fit psk -52 dBm  (known)\n  Guest cafe                       open    -71 dBm\n", u.nets, NM_MAXNET);
		u.loaded[NM_WIFI] = 1;
		snprintf(u.swifi, sizeof u.swifi, "connected to a very long network name");
		render_text(&u, 40, 12);
		for (int i = 0; i < 12; i++) CHECK(width_of(frame[i]) <= 40, "40x12 row %d too wide: %d '%s'", i, width_of(frame[i]), frame[i]);
		CHECK(!frame[12][0], "40x12: nothing past row 12");
		CHECK(strstr(frame[0], "1 Wi-Fi") && strstr(frame[0], "3 BT"), "short tab names '%s'", frame[0]);
		CHECK(strstr(frame[3], "psk") && strstr(frame[3], "-52"), "40x12 row keeps type+signal '%s'", frame[3]);
		CHECK(strstr(frame[11], "q quit"), "40x12 keybar keeps quit '%s'", frame[11]);

		/* other tabs */
		nm_ui_init(&u, 80, 24);
		u.nifs = nm_parse_status("enp2s0   eth   down       94:c6:91:f1:f4:59   no cable/link\nwlan0    wifi  up         38:00:25:04:4e:e9 192.168.1.58\n", u.ifs, NM_MAXIF, NULL, NULL, 0);
		u.tab = NM_ETH; u.loaded[NM_ETH] = 1;
		render_text(&u, 80, 24);
		CHECK(strstr(frame[3], "enp2s0") && strstr(frame[3], "no link") && strstr(frame[4], "192.168.1.58"), "eth rows '%s' / '%s'", frame[3], frame[4]);
		CHECK(strstr(frame[22 - 1], "not wired yet") || strstr(frame[19], "not wired yet"), "eth placeholder note");
		u.tab = NM_VPN;
		render_text(&u, 80, 24);
		CHECK(strstr(frame[4], "VPN: backend not wired yet"), "vpn placeholder '%s'", frame[4]);
		u.ndevs = nm_parse_bt("CPT AA:BB:CC:DD:EE:FF  Fake Pad  -60 dBm\n", u.devs, NM_MAXDEV);
		u.tab = NM_BT; u.loaded[NM_BT] = 1;
		render_text(&u, 80, 24);
		CHECK(strstr(frame[3], "CPT") && strstr(frame[3], "AA:BB:CC:DD:EE:FF") && strstr(frame[3], "Fake Pad") && strstr(frame[3], "-60 dBm"), "bt row '%s'", frame[3]);

		/* masked prompt never shows the typed text */
		u.tab = NM_WIFI;
		u.mode = NM_PROMPT; u.masked = 1; u.pkind = 1; snprintf(u.label, sizeof u.label, "passphrase: ");
		nm_ui_key(&u, 's'); nm_ui_key(&u, 'e'); nm_ui_key(&u, 'c');
		render_text(&u, 80, 24);
		CHECK(strstr(frame[22], "passphrase: ***") && !strstr(frame[22], "sec"), "masked '%s'", frame[22]);
		nm_ui_key(&u, NM_K_BS);
		CHECK(u.inlen == 2, "backspace");
		nm_ui_key(&u, NM_K_ESC);
		CHECK(u.mode == NM_NORMAL && u.inlen == 0 && !u.in[0], "esc cancels and wipes the buffer");
	}

	/* ---- keys that do not touch the library ---- */
	{
		static nm_ui u;
		nm_ui_init(&u, 80, 24);
		u.tab = NM_WIFI; u.loaded[NM_WIFI] = u.loaded[NM_ETH] = u.loaded[NM_BT] = u.loaded[NM_VPN] = 1;
		u.nnets = nm_parse_wifi("  alpha psk -50 dBm\n  beta psk -60 dBm\n  gamma open -70 dBm\n", u.nets, NM_MAXNET);
		nm_ui_key(&u, 'j'); nm_ui_key(&u, NM_K_DOWN);
		CHECK(u.sel[NM_WIFI] == 2, "down twice");
		nm_ui_key(&u, 'j');
		CHECK(u.sel[NM_WIFI] == 2, "clamped at the end");
		nm_ui_key(&u, 'g');
		CHECK(u.sel[NM_WIFI] == 0, "g");
		nm_ui_key(&u, 'G');
		CHECK(u.sel[NM_WIFI] == 2, "G");
		u.masked = 1;   /* a passphrase prompt was open earlier: the filter must not inherit its masking */
		nm_ui_key(&u, '/'); nm_ui_key(&u, 'B'); nm_ui_key(&u, 'e');
		render_text(&u, 80, 24);
		CHECK(strstr(frame[22], "/Be") && !strstr(frame[22], "*"), "filter prompt is plain text: '%s'", frame[22]);
		{ int idx[NM_MAXNET]; CHECK(u.mode == NM_FILTER && nm_ui_visible(&u, idx, NM_MAXNET) == 1 && idx[0] == 1, "filter 'Be' -> beta only (case-insensitive)"); }
		nm_ui_key(&u, NM_K_ENTER);
		CHECK(u.mode == NM_NORMAL && !strcmp(u.filter, "Be"), "enter keeps the filter");
		nm_ui_key(&u, '/'); nm_ui_key(&u, NM_K_ESC);
		{ int idx[NM_MAXNET]; CHECK(!u.filter[0] && nm_ui_visible(&u, idx, NM_MAXNET) == 3, "esc clears the filter"); }
		nm_ui_key(&u, '?');
		CHECK(u.mode == NM_HELP, "help");
		nm_ui_key(&u, 'x');
		CHECK(u.mode == NM_NORMAL, "any key closes help");
		nm_ui_key(&u, '3');
		CHECK(u.tab == NM_BT, "3 -> bluetooth");
		nm_ui_key(&u, 9); nm_ui_key(&u, 9);
		CHECK(u.tab == NM_WIFI, "tab wraps");
		nm_ui_key(&u, NM_K_BTAB);
		CHECK(u.tab == NM_VPN, "shift-tab wraps back");
		nm_ui_key(&u, 'q');
		CHECK(u.quit, "q quits");
		/* forget asks first; anything but y backs out without calling the library */
		nm_ui_init(&u, 80, 24);
		u.tab = NM_WIFI; u.loaded[NM_WIFI] = 1;
		u.nnets = nm_parse_wifi("  alpha psk -50 dBm\n", u.nets, NM_MAXNET);
		nm_ui_key(&u, 'x');
		CHECK(u.mode == NM_CONFIRM && !strcmp(u.cwhat, "alpha"), "x -> confirm");
		nm_ui_key(&u, 'n');
		CHECK(u.mode == NM_NORMAL, "n backs out");
	}

	printf("%d checks, %d failed\n", checks, fails);
	return fails != 0;
}
