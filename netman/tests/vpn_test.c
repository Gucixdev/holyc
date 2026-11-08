/* vpn_test.c - tests for vpn.h.
 *
 *   cc -std=gnu11 -O1 -o vpn_test vpn_test.c && ./vpn_test        pure tests: crypto vectors, parser, fuzz, JSON, providers, DNS files
 *   ./vpn_test ns                                                         + live tests inside `unshare -Urn` (never touches the host network)
 *
 * The live part refuses to run unless it is inside a non-initial user namespace with an empty network namespace (only "lo"),
 * so a mistake cannot reach the real routing table. Needs ip(8); nft for the kill switch; openvpn for the OpenVPN check.
 * Provider tests parse saved real API responses in vpn_testdata/; the authenticated calls (Mullvad device registration,
 * NordVPN credentials) need accounts and are NOT tested here.
 */
#define _GNU_SOURCE
#define NETMAN_VPN_IMPLEMENTATION
#include "../src/mods/vpn.h"
#include <sys/wait.h>
#include <sys/stat.h>

static int fails, passes;
#define CHK(c, ...) do { if (c) passes++; else { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define OK(c, what) CHK((c), "%s", what)

static unsigned long long rs = 88172645463325252ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 11); }

static void unhex(const char *h, unsigned char *o, int n) { for (int i = 0; i < n; i++) { unsigned v; sscanf(h + 2 * i, "%2x", &v); o[i] = (unsigned char)v; } }

static char *slurp_file(const char *p, size_t *l) { return netman_vpn__slurp(p, l, 1 << 24, 0); }

static void test_crypto(void) {
    unsigned char k[32], u[32], want[32], got[32];
    unhex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", k, 32);
    unhex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", u, 32);
    unhex("c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552", want, 32);
    netman_vpn__scalarmult(got, k, u);
    OK(memcmp(got, want, 32) == 0, "RFC 7748 X25519 vector 1");
    unhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", k, 32);
    unhex("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a", want, 32);
    netman_vpn_pubkey(k, got);
    OK(memcmp(got, want, 32) == 0, "RFC 7748 6.1 public key");
    char b[45]; unsigned char back[32];
    netman_vpn_b64_encode32(want, b); OK(netman_vpn_b64_decode32(b, back) == 0 && !memcmp(back, want, 32), "base64 roundtrip");
    OK(netman_vpn_b64_decode32("short", back) == -1, "reject short key");
    OK(netman_vpn_b64_decode32("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAB=", back) == -1, "reject non-canonical base64");
    OK(netman_vpn_b64_decode32("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA!=", back) == -1, "reject bad alphabet");
    unsigned char g1[32], g2[32]; OK(netman_vpn_genkey(g1) == 0 && netman_vpn_genkey(g2) == 0 && memcmp(g1, g2, 32) && (g1[0] & 7) == 0 && (g1[31] & 0xc0) == 0x40, "genkey clamped + random");
}

static const char GOOD[] =
    "# comment\n[Interface]\nPrivateKey = AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=\nAddress = 10.0.0.2/24, fd00::2/64\nDNS = 1.1.1.1, 9.9.9.9, corp.example\nMTU = 1380\nListenPort = 51820\nFwMark = 0x1234\nTable = 77\n"
    "PostUp = echo up %i\nPostDown = echo down %i\n\n[Peer]\nPublicKey = AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE=\nPresharedKey = AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE=\n"
    "Endpoint = vpn.example.com:51820   # trailing\nAllowedIPs = 0.0.0.0/0, ::/0\nPersistentKeepalive = 25\n[Peer]\nPublicKey = AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE=\nEndpoint = [2001:db8::1]:443\nAllowedIPs = 10.9.0.0/16\n";

static void test_parser(void) {
    netman_vpn_wg c; char err[128];
    int r = netman_vpn_wg_parse(GOOD, sizeof GOOD - 1, &c, err, sizeof err);
    CHK(r == 0, "good config: %s", err);
    if (r == 0) {
        OK(c.has_priv && c.naddr == 2 && c.addr[0].prefix == 24 && c.addr[1].family == AF_INET6, "addresses");
        OK(c.dns.n == 2 && c.search.n == 1 && !strcmp(c.search.v[0], "corp.example"), "dns split into servers and search");
        OK(c.mtu == 1380 && c.listen_port == 51820 && c.fwmark == 0x1234 && c.has_fwmark && c.table == 77, "scalars");
        OK(c.post_up.n == 1 && c.post_down.n == 1 && !c.pre_up.n, "hooks");
        OK(c.npeers == 2 && c.peers[0].has_psk && c.peers[0].keepalive == 25 && c.peers[0].nallowed == 2 && c.peers[1].nallowed == 1, "peers");
        OK(c.peers[1].endpoint && !strcmp(c.peers[1].endpoint, "[2001:db8::1]:443"), "v6 endpoint");
        netman_vpn_wg_free(&c);
    }
    const char *bad[] = {
        "", "PrivateKey = x\n", "[Interface]\nPrivateKey = tooshort\n", "[Interface]\nBogus = 1\n", "[Interface]\nAddress = 10.0.0.1/33\n", "[Interface]\nAddress = nope\n",
        "[Interface]\nMTU = 12\n", "[Interface]\nListenPort = 70000\n", "[Interface]\n[Interface]\n", "[Peer]\nPublicKey = AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE=\n",
        "[Interface]\nPrivateKey = AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=\n[Peer]\nEndpoint = 1.2.3.4:5\n",
        "[Interface]\nPrivateKey = AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=\n[Peer]\nPublicKey = AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE=\nEndpoint = nocolon\n",
        "[Interface]\nPrivateKey = AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=\n[Peer]\nPublicKey = AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE=\nAllowedIPs = 1.2.3.4/99\n",
        "[Interface]\nDNS = bad domain!\n", "[Interface]\nTable = 254\n", "[Interface\n", "[Nope]\n", "[Interface]\nFwMark = 0\n", "[Interface]\nmissing equals\n"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) { r = netman_vpn_wg_parse(bad[i], strlen(bad[i]), &c, err, sizeof err); CHK(r == -1, "bad config #%zu accepted", i); }
    char nul[] = "[Interface]\nPrivateKey = AAAA\0AAAA\n"; OK(netman_vpn_wg_parse(nul, sizeof nul - 1, &c, err, sizeof err) == -1, "NUL byte rejected");
    char *big = malloc(300000); memset(big, 'a', 299999); big[299999] = 0; OK(netman_vpn_wg_parse(big, 299999, &c, err, sizeof err) == -1, "huge file rejected"); free(big);
    char longline[5000]; memset(longline, 'x', sizeof longline - 1); longline[sizeof longline - 1] = 0; OK(netman_vpn_wg_parse(longline, strlen(longline), &c, err, sizeof err) == -1, "huge line rejected");
    const char *sec = "[Interface]\nPrivateKey = SECRETSECRETSECRETSECRETSECRETSECRETSEC=\n";
    r = netman_vpn_wg_parse(sec, strlen(sec), &c, err, sizeof err);
    OK(r == -1 && !strstr(err, "SECRET"), "error message does not echo secrets");
    /* fuzz: byte flips, truncations, splices of a valid config, plus random garbage; must not crash (run under ASan/UBSan) */
    size_t gl = sizeof GOOD - 1; int accepted = 0;
    for (int i = 0; i < 60000; i++) {
        char buf[sizeof GOOD + 64]; size_t l = gl; memcpy(buf, GOOD, gl);
        int muts = 1 + (int)(rnd() % 6);
        for (int m = 0; m < muts; m++) {
            switch (rnd() % 5) {
            case 0: buf[rnd() % l] = (char)rnd(); break;
            case 1: l = 1 + rnd() % l; break;
            case 2: { size_t a = rnd() % l; buf[a] = "[]=,/#:\n \t"[rnd() % 10]; } break;
            case 3: { size_t a = rnd() % l, b2 = rnd() % l; char t = buf[a]; buf[a] = buf[b2]; buf[b2] = t; } break;
            default: if (l + 1 < sizeof buf) { size_t a = rnd() % l; memmove(buf + a + 1, buf + a, l - a); buf[a] = (char)rnd(); l++; }
            }
        }
        if (netman_vpn_wg_parse(buf, l, &c, NULL, 0) == 0) { accepted++; netman_vpn_wg_free(&c); }
    }
    for (int i = 0; i < 3000; i++) { char buf[200]; size_t l = rnd() % sizeof buf; for (size_t j = 0; j < l; j++) buf[j] = (char)rnd(); if (netman_vpn_wg_parse(buf, l, &c, NULL, 0) == 0) netman_vpn_wg_free(&c); }
    printf("  fuzz: 63000 inputs, %d still parsed, no crash\n", accepted); passes++;
    OK(netman_vpn_valid_name("wg0", 15) && netman_vpn_valid_name("my-vpn_1", 15) && !netman_vpn_valid_name("", 15) && !netman_vpn_valid_name("a/b", 15) && !netman_vpn_valid_name("../x", 15) && !netman_vpn_valid_name("0123456789abcdef", 15) && !netman_vpn_valid_name(".hid", 15) && !netman_vpn_valid_name("a b", 15), "interface names");
    OK(netman_vpn__detect("[Interface]\nPrivateKey=x\n") == NETMAN_VPN_WG && netman_vpn__detect("client\ndev tun\nremote a 1194\n") == NETMAN_VPN_OVPN && netman_vpn__detect("hello\n") == 0, "type detection");
    netman_vpn_ip ip; netman_vpn__ip_parse("10.1.2.3/20", &ip); netman_vpn__ip_mask(&ip);
    OK(ip.addr[0] == 10 && ip.addr[1] == 1 && ip.addr[2] == 0 && ip.addr[3] == 0, "prefix masking");
    char h[64]; int p;
    OK(netman_vpn__ep_split("[::1]:51820", h, sizeof h, &p) == 0 && p == 51820 && !strcmp(h, "::1") && netman_vpn__ep_split("a.b:0", h, sizeof h, &p) == -1 && netman_vpn__ep_split("::1:5", h, sizeof h, &p) == -1 && netman_vpn__ep_split("a b:5", h, sizeof h, &p) == -1, "endpoint split");
}

static void test_json_providers(const char *dir) {
    char path[512]; size_t l; char *j;
    snprintf(path, sizeof path, "%s/vpn_testdata/mullvad_relays.json", dir);
    j = slurp_file(path, &l);
    if (!j) { printf("  skip provider parse tests: %s missing\n", path); return; }
    netman_vpn_relay *r = malloc(sizeof *r * 4096);
    int n = netman_vpn_mullvad_parse(j, l, r, 4096);
    CHK(n > 5, "mullvad parsed %d relays", n);
    if (n > 0) {
        unsigned char k[32];
        OK(!strcmp(r[0].country, "al") && !strcmp(r[0].city, "tia") && !strcmp(r[0].host, "al-tia-wg-001") && !strcmp(r[0].endpoint_ip, "45.142.26.2") && r[0].multihop_port == 3494 && r[0].port == 51820, "mullvad first relay fields");
        int all = 1; for (int i = 0; i < n; i++) if (netman_vpn_b64_decode32(r[i].pubkey, k)) all = 0;
        OK(all, "every parsed key is a valid key");
        int sel = netman_vpn__select(r, n, "SE", NULL, 0); OK(sel >= 0 && !strcmp(r[sel].country, "se"), "select by country (case-insensitive)");
        OK(netman_vpn__select(r, n, "zz", NULL, 0) == -1, "select nothing for unknown country");
        sel = netman_vpn__select(r, n, "al", "tia", 0); OK(sel >= 0, "select by city");
    }
    for (size_t cut = 0; cut < l; cut += l / 97 + 1) { netman_vpn_relay x[64]; (void)netman_vpn_mullvad_parse(j, cut, x, 64); }
    passes++;
    char *evil = strdup("{\"countries\":[{\"code\":\"al\",\"cities\":[{\"code\":\"x\",\"relays\":[{\"hostname\":\"a\\nb\",\"ipv4_addr_in\":\"1.2.3.4\",\"public_key\":\"AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE=\"},"
                        "{\"hostname\":\"ok-host\",\"ipv4_addr_in\":\"1.2.3.4\\nEvil\",\"public_key\":\"AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE=\"},"
                        "{\"hostname\":\"ok-host2\",\"ipv4_addr_in\":\"1.2.3.4\",\"public_key\":\"not a key\"},"
                        "{\"hostname\":\"fine\",\"ipv4_addr_in\":\"1.2.3.4\",\"public_key\":\"AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE=\"}]}]}]}");
    netman_vpn_relay x[8]; n = netman_vpn_mullvad_parse(evil, strlen(evil), x, 8);
    CHK(n == 1 && !strcmp(x[0].host, "fine"), "injection / bad key relays dropped (got %d)", n); free(evil);
    char *deep = malloc(5000); memset(deep, '[', 4999); deep[4999] = 0; OK(netman_vpn_mullvad_parse(deep, 4999, x, 8) == -1, "deep nesting rejected"); free(deep);
    OK(netman_vpn_mullvad_parse("", 0, x, 8) == -1 && netman_vpn_mullvad_parse("{", 1, x, 8) == -1 && netman_vpn_mullvad_parse("{\"countries\":5}", 15, x, 8) == -1, "garbage rejected");
    free(j);
    snprintf(path, sizeof path, "%s/vpn_testdata/nord_recommendations.json", dir);
    j = slurp_file(path, &l);
    if (j) {
        n = netman_vpn_nord_parse(j, l, r, 4096);
        CHK(n >= 1, "nord parsed %d servers", n);
        if (n > 0) {
            OK(strstr(r[0].host, "nordvpn.com") && r[0].load >= 0 && r[0].port == 51820 && r[0].country[0] && r[0].endpoint_ip[0], "nord fields");
            unsigned char k[32]; OK(netman_vpn_b64_decode32(r[0].pubkey, k) == 0, "nord wireguard key found in technologies metadata");
            int best = netman_vpn__select(r, n, NULL, NULL, 0); int lo = 1; for (int i = 0; i < n; i++) if (r[i].load < r[best].load) lo = 0;
            OK(lo, "select picks lowest load when not measuring latency");
        }
        for (size_t cut = 0; cut < l; cut += l / 61 + 1) { netman_vpn_relay x2[16]; (void)netman_vpn_nord_parse(j, cut, x2, 16); }
        passes++; free(j);
    }
    free(r);
}

static void test_dns_files(void) {
    char dir[] = "/tmp/netman_vpn_dnsXXXXXX"; if (!mkdtemp(dir)) { fails++; return; }
    char rundir[600], rc[600], other[600];
    snprintf(rundir, sizeof rundir, "%s/run", dir); snprintf(rc, sizeof rc, "%s/resolv.conf", dir); snprintf(other, sizeof other, "%s/real.conf", dir);
    netman_vpn_run_dir = rundir; netman_vpn_resolv_path = rc; netman_vpn__rundir();
    const char *cf = "[Interface]\nPrivateKey = AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=\nDNS = 10.64.0.1, lab.example\n";
    netman_vpn_wg c; netman_vpn_wg_parse(cf, strlen(cf), &c, NULL, 0);
    OK(c.dns.n == 1, "dns parsed");
    netman_vpn__spit(rc, "nameserver 9.9.9.9\n", 19, 0644);
    OK(netman_vpn__dns_set("t1", &c) == 0, "dns set");
    size_t l; char *t = slurp_file(rc, &l);
    OK(t && strstr(t, "# netman_vpn:t1") && strstr(t, "nameserver 10.64.0.1") && strstr(t, "search lab.example") && !strstr(t, "9.9.9.9"), "resolv.conf replaced with tunnel dns"); free(t);
    OK(netman_vpn__dns_restore("t1") == 0, "dns restore");
    t = slurp_file(rc, &l); OK(t && !strcmp(t, "nameserver 9.9.9.9\n"), "original resolv.conf back byte for byte"); free(t);
    unlink(rc); netman_vpn__spit(other, "nameserver 127.0.0.53\n", 22, 0644); symlink("real.conf", rc);
    OK(netman_vpn__dns_set("t2", &c) == 0, "dns set over symlink");
    struct stat st; OK(lstat(rc, &st) == 0 && S_ISREG(st.st_mode), "symlink replaced by file");
    t = slurp_file(other, &l); OK(t && !strcmp(t, "nameserver 127.0.0.53\n"), "symlink target left untouched"); free(t);
    OK(netman_vpn__dns_restore("t2") == 0, "restore symlink");
    char tg[100]; ssize_t n = readlink(rc, tg, sizeof tg - 1); if (n > 0) tg[n] = 0; OK(n > 0 && !strcmp(tg, "real.conf"), "symlink put back");
    unlink(rc); netman_vpn__spit(rc, "nameserver 8.8.8.8\n", 19, 0644);
    netman_vpn__dns_set("t3", &c); netman_vpn__spit(rc, "nameserver 1.1.1.1\n", 19, 0644);
    FILE *save = netman_vpn_err; netman_vpn_err = fopen("/dev/null", "w");
    OK(netman_vpn__dns_restore("t3") != 0, "restore refuses when resolv.conf was changed by someone else"); fclose(netman_vpn_err); netman_vpn_err = save;
    t = slurp_file(rc, &l); OK(t && !strcmp(t, "nameserver 1.1.1.1\n"), "foreign change preserved"); free(t);
    netman_vpn_wg_free(&c);
    char cmd[700]; snprintf(cmd, sizeof cmd, "rm -rf %s", dir); if (system(cmd)) {}
    netman_vpn_run_dir = "/run/netman/vpn"; netman_vpn_resolv_path = "/etc/resolv.conf";
}

/* ================================================================ live tests, inside unshare -Urn only */
static char TD[256], RUND[300], RESOLV[300];

static void sh(const char *cmd, char *out, size_t n) {
    FILE *p = popen(cmd, "r"); size_t o = 0; if (out) out[0] = 0;
    if (!p) return;
    char b[512]; while (fgets(b, sizeof b, p)) { if (out && o + strlen(b) + 1 < n) { strcpy(out + o, b); o += strlen(b); } }
    pclose(p);
}
static int udp_send(const char *ip, int port, const char *msg) {
    int s = socket(AF_INET, SOCK_DGRAM, 0); struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((unsigned short)port)};
    inet_pton(AF_INET, ip, &a.sin_addr);
    int r = sendto(s, msg, strlen(msg), 0, (struct sockaddr *)&a, sizeof a) < 0 ? errno : 0; close(s); return r;
}
static int wait_byte(int fd, int ms) { struct pollfd p = {fd, POLLIN, 0}; if (poll(&p, 1, ms) <= 0) return 0; char c; return read(fd, &c, 1) == 1; }
static void wfile(const char *name, const char *text, mode_t m) { char p[600]; snprintf(p, sizeof p, "%s/%s", TD, name); netman_vpn__spit(p, text, strlen(text), m); }

static int guard_ns(void) {
    FILE *f = fopen("/proc/self/uid_map", "r"); char b[128] = "";
    if (f) { if (!fgets(b, sizeof b, f)) b[0] = 0; fclose(f); }
    unsigned a, bb, c;
    if (sscanf(b, "%u %u %u", &a, &bb, &c) != 3 || (a == 0 && bb == 0 && c > 65536)) { puts("REFUSING live tests: not inside a user namespace (initial uid_map)"); return 0; }
    int others = 0;   /* if_nameindex() asks the kernel for THIS netns; /sys/class/net can still show the host's */
    struct if_nameindex *ni = if_nameindex();
    for (struct if_nameindex *i = ni; ni && i->if_index; i++) if (strcmp(i->if_name, "lo")) others++;
    if (ni) if_freenameindex(ni);
    if (others) { puts("REFUSING live tests: network namespace is not empty (would touch real interfaces)"); return 0; }
    return geteuid() == 0;
}

static void live_wireguard(void) {
    unsigned char ka[32], kb[32], pa[32], pb[32]; char sa[45], sb[45], ua[45], ub[45];
    netman_vpn_genkey(ka); netman_vpn_genkey(kb); netman_vpn_pubkey(ka, pa); netman_vpn_pubkey(kb, pb);
    netman_vpn_b64_encode32(ka, sa); netman_vpn_b64_encode32(kb, sb); netman_vpn_b64_encode32(pa, ua); netman_vpn_b64_encode32(pb, ub);
    char cfg[2048], hookf[300]; snprintf(hookf, sizeof hookf, "%s/hook.out", TD);
    snprintf(cfg, sizeof cfg, "[Interface]\nPrivateKey = %s\nAddress = 10.77.0.2/24\nListenPort = 51821\n\n[Peer]\nPublicKey = %s\nAllowedIPs = 10.77.0.1/32\n", sb, ua);
    wfile("wgB.conf", cfg, 0600);
    snprintf(cfg, sizeof cfg, "[Interface]\nPrivateKey = %s\nAddress = 10.77.0.1/24\nPostUp = echo %%i > %s\nPostDown = echo down-%%i >> %s\n\n[Peer]\nPublicKey = %s\nEndpoint = 192.168.55.2:51821\nAllowedIPs = 10.77.0.2/32\nPersistentKeepalive = 1\n", sa, hookf, hookf, ub);
    wfile("wgA.conf", cfg, 0600);
    wfile("wgW.conf", cfg, 0666);   /* world-writable profile must be refused */

    int toB[2], fromB[2]; if (pipe(toB) || pipe(fromB)) { fails++; return; }
    pid_t kid = fork();
    if (kid == 0) {   /* ---- namespace B ---- */
        close(toB[1]); close(fromB[0]);
        if (unshare(CLONE_NEWNET)) _exit(90);
        char c = 1; if (write(fromB[1], &c, 1) != 1) _exit(95);
        if (read(toB[0], &c, 1) != 1) _exit(91);
        if (system("ip link set lo up; ip addr add 192.168.55.2/24 dev vB; ip link set vB mtu 1400 up")) _exit(92);
        FILE *dn = fopen("/dev/null", "w"); netman_vpn_out = dn;
        if (netman_vpn_up("wgB", 0)) _exit(93);
        int s = socket(AF_INET, SOCK_DGRAM, 0); struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(9999)};
        if (bind(s, (struct sockaddr *)&a, sizeof a)) _exit(94);
        c = 2; if (write(fromB[1], &c, 1) != 1) _exit(95);
        alarm(150);
        struct pollfd p[2] = {{s, POLLIN, 0}, {toB[0], POLLIN, 0}};
        for (;;) {
            if (poll(p, 2, -1) < 0) continue;
            if (p[1].revents) break;
            if (p[0].revents) { char b[64]; if (recv(s, b, sizeof b, 0) > 0) { c = 3; if (write(fromB[1], &c, 1) != 1) _exit(95); } }
        }
        netman_vpn_down("wgB");
        _exit(0);
    }
    close(toB[0]); close(fromB[1]);
    OK(wait_byte(fromB[0], 5000), "namespace B created");
    char cmd[300]; snprintf(cmd, sizeof cmd, "ip link add vA type veth peer name vB netns %d && ip addr add 192.168.55.1/24 dev vA && ip link set vA mtu 1400 up && ip link set lo up", (int)kid);
    OK(system(cmd) == 0, "veth pair between namespaces");
    char c = 1; if (write(toB[1], &c, 1) != 1) fails++;
    OK(wait_byte(fromB[0], 8000), "wgB is up and serving in its namespace");

    char base_rules[2048], buf[2048]; sh("ip rule", base_rules, sizeof base_rules);
    FILE *save = netman_vpn_err; FILE *dn = fopen("/dev/null", "w");
    netman_vpn_err = dn; OK(netman_vpn_up("wgW", 0) != 0, "world-writable profile refused"); netman_vpn_err = save;
    netman_vpn_err = dn; OK(netman_vpn_up("bad name", 0) != 0, "invalid name refused"); netman_vpn_err = save;
    OK(netman_vpn_up("wgA", 0) == 0, "wgA up (native netlink)");
    OK(netman_vpn__is_wg_link("wgA") && if_nametoindex("wgA"), "link exists and is DEVTYPE wireguard");
    char *hk = slurp_file(hookf, NULL); OK(hk && !strncmp(hk, "wgA", 3), "PostUp hook ran with %i substituted"); free(hk);
    sh("ip -o addr show dev wgA", buf, sizeof buf); OK(strstr(buf, "10.77.0.1/24"), "address configured");
    sh("ip -o link show wgA", buf, sizeof buf); OK(strstr(buf, "mtu 1420") && strstr(buf, "UP"), "mtu 1420 and up by default");
    sh("ip route get 10.77.0.2", buf, sizeof buf); OK(strstr(buf, "dev wgA"), "route to peer via tunnel");
    netman_vpn_err = dn; OK(netman_vpn_up("wgA", 0) != 0, "second up refused"); netman_vpn_err = save;
    OK(udp_send("10.77.0.2", 9999, "hello") == 0, "send through tunnel");
    OK(wait_byte(fromB[0], 8000), "REAL handshake + data: B received the datagram through WireGuard");
    netman_vpn_wgstat st; OK(netman_vpn_wg_stats("wgA", &st) == 0 && st.present && st.npeers == 1 && st.handshaked == 1 && st.tx > 0 && st.rx > 0, "stats: handshake done, rx/tx counted");
    netman_vpn_out = save; netman_vpn_status("wgA"); netman_vpn_out = NULL;
    int rtt = netman_vpn_rtt_ms("192.168.55.2", 1000); CHK(rtt >= 0, "icmp rtt over veth (ms=%d)", rtt);
    int mtu = netman_vpn_mtu_probe("192.168.55.2"); CHK(mtu == 1400, "path mtu probe over a 1400 veth found %d", mtu);
    OK(netman_vpn_down("wgA") == 0 && !if_nametoindex("wgA"), "wgA down removes the link");
    hk = slurp_file(hookf, NULL); OK(hk && strstr(hk, "down-wgA"), "PostDown hook ran"); free(hk);

    char dnsorig[] = "nameserver 9.9.9.9\n"; netman_vpn__spit(RESOLV, dnsorig, strlen(dnsorig), 0644);
    snprintf(cfg, sizeof cfg, "[Interface]\nPrivateKey = %s\nAddress = 10.77.0.1/24\nDNS = 10.77.0.53\nFwMark = 0x5555\n\n[Peer]\nPublicKey = %s\nEndpoint = 192.168.55.2:51821\nAllowedIPs = 0.0.0.0/0\nPersistentKeepalive = 1\n", sa, ub);
    wfile("wgA.conf", cfg, 0600);
    OK(netman_vpn_up("wgA", 0) == 0, "wgA up with default route");
    sh("ip rule", buf, sizeof buf); OK(strstr(buf, "not from all fwmark 0x5555 lookup 21845") && strstr(buf, "suppress_prefixlength 0"), "policy rules like wg-quick");
    sh("ip route show table 21845", buf, sizeof buf); OK(strstr(buf, "default dev wgA"), "default route in its own table");
    sh("ip route get 8.8.8.8", buf, sizeof buf); OK(strstr(buf, "dev wgA"), "internet-bound traffic goes into the tunnel");
    sh("ip route get 192.168.55.2 mark 0x5555", buf, sizeof buf); OK(strstr(buf, "dev vA") && !strstr(buf, "wgA"), "marked tunnel transport bypasses the tunnel (no routing loop)");
    sh("ip route show table main", buf, sizeof buf); OK(!strstr(buf, "default"), "main table untouched by the default route");
    size_t rl; char *rtxt = slurp_file(RESOLV, &rl); OK(rtxt && strstr(rtxt, "nameserver 10.77.0.53") && !strstr(rtxt, "9.9.9.9"), "dns now goes to the tunnel resolver"); free(rtxt);
    OK(udp_send("10.77.0.2", 9999, "again") == 0 && wait_byte(fromB[0], 8000), "data flows with a full-tunnel config (handshake survived the default route)");

    setenv("NETMAN_VPN_KS_LAN", "0", 1);
    OK(netman_vpn_killswitch("wgA", 1) == 0, "kill switch armed");
    sh("nft list table inet netman_vpn_ks", buf, sizeof buf); OK(strstr(buf, "policy drop") && strstr(buf, "192.168.55.2") && strstr(buf, "51821") && strstr(buf, "\"wgA\""), "ruleset: drop by default, tunnel + endpoint allowed");
    OK(udp_send("10.77.0.2", 9999, "ks") == 0 && wait_byte(fromB[0], 8000), "traffic through the tunnel still passes with the kill switch on");
    OK(udp_send("192.168.55.2", 4444, "leak") == EPERM, "direct (non-tunnel) traffic is dropped");
    OK(udp_send("192.168.55.2", 53, "dns") == EPERM, "dns outside the tunnel is dropped (leak guard)");
    OK(netman_vpn_killswitch("wgA", 1) == 0, "re-arming is atomic and idempotent");
    OK(system("ip link del wgA") == 0, "simulate a tunnel crash");
    OK(udp_send("192.168.55.2", 4444, "leak2") == EPERM, "still no leak while the tunnel is down (survives flaps)");
    int w = netman_vpn_watch_once("wgA", 180); CHK(w == 1, "watchdog reconnects a dead tunnel (got %d)", w);
    OK(if_nametoindex("wgA") != 0, "tunnel back after watchdog");
    OK(udp_send("10.77.0.2", 9999, "after") == 0 && wait_byte(fromB[0], 8000), "data flows again after reconnect, kill switch still armed");
    OK(netman_vpn_watch_once("wgA", 180) == 0, "watchdog leaves a healthy tunnel alone");
    OK(netman_vpn_killswitch("wgA", 0) == 0, "kill switch disarmed");
    netman_vpn_err = dn; OK(netman_vpn_killswitch("wgA", 0) != 0, "disarming twice reports it"); netman_vpn_err = save;
    OK(udp_send("192.168.55.2", 4444, "free") == 0, "traffic free again");
    OK(netman_vpn_down("wgA") == 0, "wgA down");
    sh("ip rule", buf, sizeof buf); OK(strcmp(buf, base_rules) == 0, "ip rules exactly as before: nothing left behind");
    rtxt = slurp_file(RESOLV, &rl); OK(rtxt && !strcmp(rtxt, dnsorig), "resolv.conf restored byte for byte"); free(rtxt);
    OK(!if_nametoindex("wgA"), "link gone");
    char sp[600]; snprintf(sp, sizeof sp, "%s/wgA.state", RUND); OK(access(sp, F_OK) != 0, "state file removed");

    char outf[600]; snprintf(outf, sizeof outf, "%s/run.out", TD);
    char script[3000]; snprintf(script, sizeof script, "ip -o link | awk '{print $2}' > %s; ip route >> %s; cat %s >> %s; echo ping > /dev/udp/10.77.0.2/9999", outf, outf, RESOLV, outf);
    char *argvr[] = {"bash", "-c", script, NULL};
    snprintf(cfg, sizeof cfg, "[Interface]\nPrivateKey = %s\nAddress = 10.77.0.1/24\nDNS = 10.77.0.53\n\n[Peer]\nPublicKey = %s\nEndpoint = 192.168.55.2:51821\nAllowedIPs = 0.0.0.0/0\nPersistentKeepalive = 1\n", sa, ub);
    wfile("wgA.conf", cfg, 0600);
    int rr = netman_vpn_run("wgA", argvr); CHK(rr == 0, "netman_vpn_run exit status %d", rr);
    char *ro = slurp_file(outf, NULL);
    OK(ro && strstr(ro, "lo:") && strstr(ro, "wgA:") && !strstr(ro, "vA"), "command sees only lo + the tunnel (host nics invisible)");
    OK(ro && strstr(ro, "default dev wgA"), "its only route is the tunnel");
    OK(ro && strstr(ro, "nameserver 10.77.0.53"), "its resolver is the tunnel's");
    free(ro);
    OK(wait_byte(fromB[0], 8000), "REAL data: the command's datagram reached B through the tunnel");
    OK(!if_nametoindex("wgA"), "host namespace has no tunnel interface left; other commands stay direct");
    rtxt = slurp_file(RESOLV, &rl); OK(rtxt && !strcmp(rtxt, dnsorig), "host resolv.conf unchanged by run"); free(rtxt);
    char *argvf[] = {"false", NULL}; OK(netman_vpn_run("wgA", argvf) == 1, "exit status of the command is returned");

    close(toB[1]);
    int stt; waitpid(kid, &stt, 0); OK(WIFEXITED(stt) && WEXITSTATUS(stt) == 0, "namespace B exited cleanly");
    fclose(dn);
}

static void live_openvpn(void) {
    if (system("openvpn --version >/dev/null 2>&1") || access("/dev/net/tun", W_OK)) { puts("  SKIP openvpn live test: openvpn or /dev/net/tun unusable"); return; }
    char cmd[1500], keyf[600]; snprintf(keyf, sizeof keyf, "%s/static.key", TD);
    snprintf(cmd, sizeof cmd, "openvpn --genkey secret %s >/dev/null 2>&1 || openvpn --genkey --secret %s >/dev/null 2>&1", keyf, keyf);
    if (system(cmd) || chmod(keyf, 0600)) { puts("  SKIP openvpn live test: cannot generate a static key"); return; }
    char cfg[1500];
    for (int i = 0; i < 2; i++) {
        snprintf(cfg, sizeof cfg, "dev tun%d\nproto udp\nlocal 127.0.0.1\nlport %d\nremote 127.0.0.1 %d\nrport %d\nifconfig 10.9.0.%d 10.9.0.%d\nsecret %s\ncipher AES-256-CBC\nauth SHA256\nallow-deprecated-insecure-static-crypto\nkeepalive 2 10\nverb 3\n",
                 i, i ? 1195 : 1194, i ? 1194 : 1195, i ? 1194 : 1195, i ? 2 : 1, i ? 1 : 2, keyf);
        wfile(i ? "ovB.conf" : "ovA.conf", cfg, 0600);
    }
    FILE *dn = fopen("/dev/null", "w"), *save = netman_vpn_out; netman_vpn_out = dn;
    int a = netman_vpn_up("ovA", 0), b = netman_vpn_up("ovB", 25);
    CHK(a == 0 && b == 0, "openvpn pair started (a=%d b=%d)", a, b);
    if (a || b) { netman_vpn_out = NULL; netman_vpn_logs(a ? "ovA" : "ovB", 12); netman_vpn_out = dn; }
    int con = 0; for (int i = 0; i < 100 && !con; i++) { con = !strcmp(netman_vpn__ovpn_state("ovA"), "connected") && !strcmp(netman_vpn__ovpn_state("ovB"), "connected"); if (!con) usleep(200000); }
    CHK(con, "openvpn static-key pair reaches 'Initialization Sequence Completed' (A=%s B=%s)", netman_vpn__ovpn_state("ovA"), netman_vpn__ovpn_state("ovB"));
    char buf[4096]; sh("ip -o addr show", buf, sizeof buf);
    OK(strstr(buf, "10.9.0.1") || strstr(buf, "10.9.0.2"), "openvpn created its tun device");
    netman_vpn_out = NULL; netman_vpn_status("ovA"); netman_vpn_list(); netman_vpn_out = dn;
    OK(netman_vpn_down("ovA") == 0 && netman_vpn_down("ovB") == 0, "openvpn stopped via pidfile (cmdline verified)");
    OK(!strcmp(netman_vpn__ovpn_state("ovA"), "down"), "state is down afterwards");
    char pf[600]; snprintf(pf, sizeof pf, "%s/ovC.pid", RUND); char me[32]; snprintf(me, sizeof me, "%d\n", (int)getpid()); netman_vpn__spit(pf, me, strlen(me), 0600);
    OK(!netman_vpn__ovpn_pid("ovC", NULL), "pidfile of an unrelated live process is not trusted");
    wfile("ovP.conf", "client\ndev tun\nremote 127.0.0.1 1194\nauth-user-pass ovP.auth\n", 0600);
    wfile("ovP.auth", "user\nhunter2\n", 0644);
    FILE *se = netman_vpn_err; netman_vpn_err = dn; OK(netman_vpn_up("ovP", 0) != 0, "auth file readable by others is refused (and no openvpn started)"); netman_vpn_err = se;
    OK(!netman_vpn__ovpn_pid("ovP", NULL), "nothing was started");
    wfile("ovQ.conf", "client\ndev tun\nremote 127.0.0.1 1194\nauth-user-pass\n", 0600);
    netman_vpn_err = dn; OK(netman_vpn_up("ovQ", 0) != 0, "interactive auth-user-pass without NAME.auth is refused"); netman_vpn_err = se;
    netman_vpn_out = save; fclose(dn);
}

static void live(void) {
    snprintf(TD, sizeof TD, "/tmp/netman_vpn_liveXXXXXX");
    if (!mkdtemp(TD)) { fails++; return; }
    snprintf(RUND, sizeof RUND, "%s/run", TD); snprintf(RESOLV, sizeof RESOLV, "%s/etc/resolv.conf", TD); { char d2[400]; snprintf(d2, sizeof d2, "%s/etc", TD); mkdir(d2, 0755); }
    netman_vpn_conf_dir = TD; netman_vpn_run_dir = RUND; netman_vpn_resolv_path = RESOLV;
    puts("live tests (user+net namespace):");
    FILE *dn = fopen("/dev/null", "w"); netman_vpn_out = dn;
    live_wireguard();
    live_openvpn();
    fclose(dn); netman_vpn_out = NULL;
    char cmd[400]; snprintf(cmd, sizeof cmd, "rm -rf %s", TD); if (system(cmd)) {}
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "ns")) { execlp("unshare", "unshare", "-Urn", argv[0], "inner", (char *)NULL); perror("unshare"); return 2; }
    int inner = argc > 1 && !strcmp(argv[1], "inner");
    test_crypto(); test_parser(); test_json_providers("."); test_dns_files();
    if (inner) { if (guard_ns()) live(); else fails++; }
    printf("%d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
