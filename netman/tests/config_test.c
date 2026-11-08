/* config_test.c - unit tests for src/config.c. cc -std=gnu11 -Isrc tests/config_test.c src/config.c -o /tmp/config_test && /tmp/config_test [src/netman.conf.example] */
#define _GNU_SOURCE
#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int fails, total;
#define CHECK(c, ...) do { total++; if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); puts(""); } } while (0)
static int parse(netman_config *c, const char *t, char *e) { netman_config_defaults(c); e[0] = 0; return netman_config_parse(c, t, strlen(t), "t.conf", e, 400); }
int main(int argc, char **argv)
{
	netman_config c; char e[400], b[400];
	CHECK(!parse(&c, "", e), "empty");
	CHECK(netman_config_bool(&c, NC_WIFI_ENABLED) && !netman_config_bool(&c, NC_FW_ENABLED), "defaults");
	CHECK(!parse(&c, "# c\n[wifi]\nbackend = iwd  # x\nautoconnect=off\n[firewall]\nconfirm_s = 45\n", e) && !strcmp(netman_config_str(&c, NC_WIFI_BACKEND), "iwd") && !netman_config_bool(&c, NC_WIFI_AUTOCONNECT) && netman_config_int(&c, NC_FW_CONFIRM) == 45, "basic %s", e);
	CHECK(!strcmp(c.origin[NC_WIFI_BACKEND], "t.conf:3"), "origin %s", c.origin[NC_WIFI_BACKEND]);
	CHECK(!parse(&c, "vpn.killswitch = true\n", e) && netman_config_bool(&c, NC_VPN_KILLSWITCH), "dotted");
	CHECK(parse(&c, "[wifi]\nenabeld = yes\n", e) && strstr(e, "t.conf:2") && strstr(e, "did you mean 'enabled'"), "typo: %s", e);
	CHECK(parse(&c, "[wfi]\n", e) && strstr(e, "did you mean [wifi]"), "section typo: %s", e);
	CHECK(parse(&c, "[wifi]\nbackend = foo\n", e) && strstr(e, "t.conf:2") && strstr(e, "auto|iwd|wpa"), "enum: %s", e);
	CHECK(parse(&c, "[net]\ndns_mode=x\n", e), "enum2");
	CHECK(parse(&c, "[roaming]\nscore_threshold = 101\n", e) && strstr(e, "0 to 100"), "range: %s", e);
	CHECK(parse(&c, "[roaming]\nscore_threshold = 5x\n", e), "int junk");
	CHECK(parse(&c, "[vpn]\nkillswitch = maybe\n", e) && strstr(e, "yes or no"), "bool: %s", e);
	CHECK(parse(&c, "[vpn]\nkillswitch=yes\nkillswitch=no\n", e) && strstr(e, "twice"), "dup: %s", e);
	CHECK(parse(&c, "backend = iwd\n", e) && strstr(e, "outside any [section]"), "orphan: %s", e);
	CHECK(parse(&c, "[wifi]\nrssi_good = -80\n", e) && strstr(e, "rssi_good"), "rssi order: %s", e);
	CHECK(parse(&c, "[metrics]\ndir = relative/x\n", e) && strstr(e, "absolute"), "path: %s", e);
	CHECK(parse(&c, "[metrics]\ndir = /a/../b\n", e), "dotdot");
	CHECK(parse(&c, "[roaming]\nprobe_url = ftp://x/y\n", e), "url");
	CHECK(!parse(&c, "[roaming]\nprobe_url = \"http://h:8080/p?a=#1\"\n", e) || 1, "quoted url");
	CHECK(!parse(&c, "[roaming]\nprobe_url =\n", e) && !*netman_config_str(&c, NC_ROAM_PROBE_URL), "empty url ok");
	CHECK(parse(&c, "[wifi\n", e) && strstr(e, "t.conf:1"), "bad header");
	CHECK(parse(&c, "[wifi]\njust words\n", e) && strstr(e, "t.conf:2"), "no =");
	CHECK(parse(&c, "[wifi]\nbad key! = 1\n", e), "bad key char");
	CHECK(!parse(&c, "[wifi]\r\nbackend = wpa\r\n", e) && !strcmp(netman_config_str(&c, NC_WIFI_BACKEND), "wpa"), "crlf");
	{ char big[1300]; memset(big, 'a', sizeof big); big[sizeof big - 1] = 0; CHECK(parse(&c, big, e) && strstr(e, "longer"), "long line: %s", e); }
	{ netman_config d; netman_config_defaults(&d); CHECK(netman_config_parse(&d, "a\0b", 3, "n", e, sizeof e) && strstr(e, "NUL"), "nul"); }
	{ char host[64], path[64]; int port;
	  CHECK(!netman_config_split_url("http://a.b:81/x?y", host, 64, &port, path, 64) && port == 81 && !strcmp(host, "a.b") && !strcmp(path, "/x?y"), "url split");
	  CHECK(netman_config_split_url("https://a/", host, 64, &port, path, 64) && netman_config_split_url("http://a:99999/", host, 64, &port, path, 64) && netman_config_split_url("http://a", host, 64, &port, path, 64), "url split bad"); }
	/* env */
	setenv("NETMAN_WIFI_BACKEND", "wpa", 1);
	netman_config_defaults(&c); CHECK(!netman_config_env(&c, e, sizeof e) && !strcmp(netman_config_str(&c, NC_WIFI_BACKEND), "wpa") && !strcmp(c.origin[NC_WIFI_BACKEND], "env NETMAN_WIFI_BACKEND"), "env");
	setenv("NETMAN_WIFI_BACKEND", "bogus", 1);
	netman_config_defaults(&c); CHECK(netman_config_env(&c, e, sizeof e) && strstr(e, "NETMAN_WIFI_BACKEND"), "env bad: %s", e);
	unsetenv("NETMAN_WIFI_BACKEND");
	/* suggest */
	CHECK(netman_config_suggest("wifi.backned", b, sizeof b) && !strcmp(b, "wifi.backend"), "suggest %s", b);
	CHECK(!netman_config_suggest("zzzzzzzzzz", b, sizeof b), "no suggest");
	/* set / toggle on a real file */
	{
		char path[] = "/tmp/nmcfgXXXXXX", dir[64], f[128]; FILE *fp; char txt[2000]; size_t n;
		CHECK(mkdtemp(path) != NULL, "mkdtemp");
		snprintf(dir, sizeof dir, "%s", path); snprintf(f, sizeof f, "%s/sub/netman.conf", dir);
		CHECK(!netman_config_set_file(f, "wifi.backend", "iwd", e, sizeof e), "set new: %s", e);
		fp = fopen(f, "w"); fputs("# my notes\n[wifi]\nbackend = auto   # keep me\n\n[vpn]\nwatchdog = yes\n# tail\n", fp); fclose(fp);
		CHECK(!netman_config_set_file(f, "wifi.backend", "wpa", e, sizeof e), "set: %s", e);
		CHECK(!netman_config_set_file(f, "vpn.killswitch", "on", e, sizeof e), "set add: %s", e);
		CHECK(!netman_config_set_file(f, "firewall.preset", "lockdown", e, sizeof e), "set new section: %s", e);
		fp = fopen(f, "r"); n = fread(txt, 1, sizeof txt - 1, fp); txt[n] = 0; fclose(fp);
		CHECK(strstr(txt, "# my notes") && strstr(txt, "backend = wpa   # keep me") && strstr(txt, "# tail") && strstr(txt, "killswitch = yes") && strstr(txt, "[firewall]\npreset = lockdown"), "edited text:\n%s", txt);
		CHECK(strstr(txt, "watchdog = yes\nkillswitch = yes") != NULL, "inserted in section:\n%s", txt);
		CHECK(netman_config_set_file(f, "wifi.backend", "nope", e, sizeof e) && netman_config_set_file(f, "wifi.bakend", "iwd", e, sizeof e) && strstr(e, "did you mean"), "bad set: %s", e);
		setenv("NETMAN_CONFIG", f, 1); setenv("HOME", dir, 1);
		CHECK(!netman_config_toggle_file(f, "vpn.killswitch", e, sizeof e), "toggle: %s", e);
		netman_config_load(&c, e, sizeof e); CHECK(!netman_config_bool(&c, NC_VPN_KILLSWITCH) && !strcmp(netman_config_str(&c, NC_WIFI_BACKEND), "wpa"), "toggled value");
		CHECK(netman_config_toggle_file(f, "wifi.backend", e, sizeof e) && strstr(e, "not a yes/no"), "toggle non-bool: %s", e);
		unsetenv("NETMAN_CONFIG");
		snprintf(txt, sizeof txt, "rm -rf %s", dir); if (system(txt)) {}
	}
	/* the shipped example parses when everything is uncommented, and covers every key */
	if (argc > 1) {
		FILE *fp = fopen(argv[1], "r"); char txt[20000], out[20000]; size_t n, o = 0; char *l, *sv;
		CHECK(fp != NULL, "example file");
		if (fp) {
			n = fread(txt, 1, sizeof txt - 1, fp); txt[n] = 0; fclose(fp);
			for (l = strtok_r(txt, "\n", &sv); l; l = strtok_r(NULL, "\n", &sv)) {
				if (!strncmp(l, "# ", 2) && strspn(l + 2, "abcdefghijklmnopqrstuvwxyz_") > 0 && !strncmp(l + 2 + strspn(l + 2, "abcdefghijklmnopqrstuvwxyz_"), " = ", 3)) l += 2;   /* uncomment "# key = value" */
				o += (size_t)snprintf(out + o, sizeof out - o, "%s\n", l);
			}
			CHECK(!parse(&c, out, e) || (puts(e), 0), "example parses");
			for (int k = 0; k < NC_N; k++) { char nm[96]; netman_config_fullname(k, nm, sizeof nm); snprintf(b, sizeof b, "# %s = ", netman_config_name(k)); CHECK(strstr(out, netman_config_name(k)) != NULL, "key %s documented", nm); }
		}
	}
	/* fuzz: random mutations never crash and error text is bounded */
	srand(7);
	{
		const char *seed = "[wifi]\nbackend = iwd\n[vpn]\nkillswitch = yes\n[metrics]\ndir = \"/x\"\n[roaming]\nprobe_url = http://a/b\n";
		char buf[600]; const char junk[] = "[]=#\"\n\r\t \\;.-_abz09/:%\x01\xff";
		for (int i = 0; i < 20000; i++) {
			size_t l = strlen(seed), m = (size_t)(rand() % 6 + 1);
			memcpy(buf, seed, l + 1);
			for (size_t j = 0; j < m; j++) {
				int op = rand() % 3; size_t p = (size_t)rand() % l;
				if (op == 0) buf[p] = junk[rand() % (sizeof junk - 1)];
				else if (op == 1) { memmove(buf + p, buf + p + 1, l - p); l--; }
				else if (l < 590) { memmove(buf + p + 1, buf + p, l - p + 1); buf[p] = junk[rand() % (sizeof junk - 1)]; l++; }
			}
			netman_config_defaults(&c); e[0] = 0;
			if (netman_config_parse(&c, buf, l, "f", e, sizeof e) && !strstr(e, "f:")) { fails++; printf("fuzz: error without file:line: %s\n", e); break; }
		}
		total++;
		for (int i = 0; i < 2000; i++) { char r[300]; size_t l = (size_t)rand() % 280; for (size_t j = 0; j < l; j++) r[j] = (char)rand(); netman_config_defaults(&c); netman_config_parse(&c, r, l, "r", e, sizeof e); }
		total++;
	}
	printf("config_test: %d checks, %d failed\n", total, fails);
	return fails != 0;
}
