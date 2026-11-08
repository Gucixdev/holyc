/* fw_test.c - tests for src/mods/firewall.h.
 *
 *   cc -std=gnu11 -Wall -Wextra -O1 fw_test.c -o /tmp/fw_test && cd tests && /tmp/fw_test       pure: golden renderer text, validation, fuzz, table guard
 *                                                                                             (+ `nft -c` / `iptables-restore --test` syntax vetting when they work unprivileged)
 *   /tmp/fw_test ns                                                    + live apply/diff/confirm/rollback/kill switch inside `unshare -Urn` (never touches the host)
 *   FW_UPDATE=1 /tmp/fw_test                                           rewrite the fw_golden files from the current renderer (review the git diff!)
 *
 * The live part refuses to run unless it is in a non-initial user namespace whose network namespace has only "lo".
 * Not testable here: the legacy iptables binary (iptables-legacy needs the ip_tables module, unavailable in a user namespace), so the
 * iptables fallback is exercised through iptables-nft; a host with legacy xtables is untested. */
#define _GNU_SOURCE
#define NETMAN_FW_IMPLEMENTATION
#include "../src/mods/firewall.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); puts(""); } } while (0)
static char b[1 << 17], b2[1 << 17];

static unsigned long long rs = 88172645463325252ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 11); }

static int have_nft_c, have_ipt_test;
static int vet(const char *plan, const char *cmd) {
    char p[] = "/tmp/fwplanXXXXXX"; int fd = mkstemp(p); if (fd < 0) return -1;
    if (write(fd, plan, strlen(plan)) < 0) { close(fd); return -1; } close(fd);
    char c[400]; snprintf(c, sizeof c, "%s < %s >/dev/null 2>&1", cmd, p);
    int r = system(c); unlink(p); return r;
}
static void vet_nft(const char *what, const char *plan) { if (have_nft_c) CHECK(vet(plan, "nft -c -f -") == 0, "nft -c rejects %s", what); }
static void vet_ipt(const char *what, const char *plan, int v6) {
    if (have_ipt_test) CHECK(vet(plan, v6 ? "ip6tables-restore --noflush --test" : "iptables-restore --noflush --test") == 0, "iptables-restore --test rejects %s", what);
}

/* ---------------------------------------------------------------- golden */
static const char *gdir;
static void golden(const char *name, const char *text) {
    char p[300], *old = NULL; size_t n = 0; snprintf(p, sizeof p, "%s/%s", gdir, name);
    if (getenv("FW_UPDATE")) { FILE *f = fopen(p, "w"); if (f) { fputs(text, f); fclose(f); } return; }
    FILE *f = fopen(p, "r");
    if (!f) { CHECK(0, "golden file %s missing (FW_UPDATE=1 to create)", p); return; }
    fseek(f, 0, SEEK_END); n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    old = malloc(n + 1); n = fread(old, 1, n, f); old[n] = 0; fclose(f);
    CHECK(!strcmp(old, text), "golden %s differs (FW_UPDATE=1 regenerates; review the diff)", name);
    if (strcmp(old, text)) { char o[300]; snprintf(o, sizeof o, "/tmp/%s.got", name); FILE *g = fopen(o, "w"); if (g) { fputs(text, g); fclose(g); } printf("  got written to %s\n", o); }
    free(old);
}

static nmfw_cfg rich(void) {                 /* workstation + every guard + zones + rules */
    nmfw_cfg c; nmfw_rule r;
    nmfw_preset(&c, "workstation");
    nmfw_set_zone(&c, "lo0", "trusted"); nmfw_set_zone(&c, "eth0", "home"); nmfw_set_zone(&c, "wl0", "public"); nmfw_set_zone(&c, "usb*", "untrusted");
    nmfw_parse_rule("allow in tcp port 22,80 from 10.0.0.0/8 on eth0 rate 5 # ssh", &r); nmfw_add_rule(&c, &r);
    nmfw_parse_rule("logdrop in v6 tcp port 23", &r); nmfw_add_rule(&c, &r);
    nmfw_parse_rule("reject out udp port 9999 to 203.0.113.0/24", &r); nmfw_add_rule(&c, &r);
    nmfw_parse_rule("allow fwd on wl0 via eth0 new est", &r); nmfw_add_rule(&c, &r);
    c.ra_guard = 1; snprintf(c.ra_router, sizeof c.ra_router, "fe80::1");
    snprintf(c.dhcp_server, sizeof c.dhcp_server, "192.168.1.1");
    snprintf(c.gw_ip, sizeof c.gw_ip, "192.168.1.1"); snprintf(c.gw_mac, sizeof c.gw_mac, "aa:bb:cc:dd:ee:ff");
    return c;
}

static void test_golden(void) {
    nmfw_cfg c; nmfw_endpoint ep[2] = {{"203.0.113.9", 51820, 1}, {"2001:db8::7", 443, 0}};
    char a[1 << 14];
    nmfw_preset(&c, "lockdown");
    CHECK(nmfw_plan(&c, b, sizeof b) > 0, "lockdown plan"); golden("lockdown.nft", b); vet_nft("lockdown", b);
    CHECK(nmfw_plan_ipt(&c, 0, b, sizeof b) > 0, "lockdown ipt4"); golden("lockdown.ipt4", b); vet_ipt("lockdown v4", b, 0);
    CHECK(nmfw_plan_ipt(&c, 1, b, sizeof b) > 0, "lockdown ipt6"); golden("lockdown.ipt6", b); vet_ipt("lockdown v6", b, 1);

    c = rich();
    CHECK(nmfw_plan(&c, b, sizeof b) > 0, "rich plan"); golden("rich.nft", b); vet_nft("rich", b);
    CHECK(nmfw_plan_arp(&c, a, sizeof a) > 0, "arp plan"); golden("rich.arp", a); vet_nft("arp pin", a);
    CHECK(nmfw_plan_ipt(&c, 0, b, sizeof b) > 0, "rich ipt4"); golden("rich.ipt4", b); vet_ipt("rich v4", b, 0);
    CHECK(nmfw_plan_ipt(&c, 1, b, sizeof b) > 0, "rich ipt6"); golden("rich.ipt6", b); vet_ipt("rich v6", b, 1);

    CHECK(nmfw_share(&c, "ap0", "eth0") == 0, "share"); nmfw_set_zone(&c, "eth0", "public");
    CHECK(nmfw_plan(&c, b, sizeof b) > 0, "share plan"); golden("share.nft", b); vet_nft("share", b);
    CHECK(strstr(b, "masquerade") && strstr(b, "iifname \"ap0\" oifname \"eth0\" accept"), "share masquerades and forwards lan->wan only");
    CHECK(nmfw_plan_ipt(&c, 0, b, sizeof b) > 0, "share ipt4"); golden("share.ipt4", b); vet_ipt("share v4", b, 0);
    CHECK(strstr(b, "MASQUERADE") && strstr(b, "*nat"), "share ipt nat");
    CHECK(nmfw_plan_ipt(&c, 1, b, sizeof b) > 0 && !strstr(b, "MASQUERADE") && !strstr(b, "*nat"), "no ipv6 nat");

    c.ndnat = 1; snprintf(c.dnat[0].iface, 17, "eth0"); c.dnat[0].proto = NMFW_TCP; c.dnat[0].port = 8080; snprintf(c.dnat[0].to, 64, "192.168.50.2"); c.dnat[0].toport = 80;
    CHECK(nmfw_plan(&c, b, sizeof b) > 0 && strstr(b, "dnat ip to 192.168.50.2:80"), "dnat plan"); golden("share_dnat.nft", b); vet_nft("dnat", b);
    CHECK(nmfw_plan_ipt(&c, 0, b, sizeof b) > 0 && strstr(b, "--to-destination 192.168.50.2:80"), "dnat ipt"); vet_ipt("dnat v4", b, 0);

    CHECK(nmfw_preset_killswitch(&c, "wg0", ep, 2) == 0, "killswitch preset");
    CHECK(nmfw_plan(&c, b, sizeof b) > 0, "killswitch preset plan"); golden("killswitch_preset.nft", b); vet_nft("killswitch preset", b);
    CHECK(strstr(b, "policy drop;\n  oifname \"lo\"") != NULL, "killswitch preset: output policy drop");
    CHECK(nmfw_plan_ipt(&c, 0, b, sizeof b) > 0, "killswitch preset ipt4"); golden("killswitch_preset.ipt4", b); vet_ipt("killswitch preset v4", b, 0);
    CHECK(nmfw_plan_ipt(&c, 1, b, sizeof b) > 0, "killswitch preset ipt6"); vet_ipt("killswitch preset v6", b, 1);

    CHECK(nmfw_plan_killswitch("wg0", ep, 2, 0, b, sizeof b) > 0, "killswitch table"); golden("killswitch.nft", b); vet_nft("killswitch", b);
    CHECK(strstr(b, "table inet netman_vpn_ks") != NULL, "killswitch table is vpn.h's table");
    CHECK(nmfw_plan_killswitch("wg0", ep, 2, 1, b, sizeof b) > 0 && strstr(b, "192.168.0.0/16"), "allow_lan"); vet_nft("killswitch lan", b);
}

static void test_zone_semantics(void) {
    nmfw_cfg c = rich(); nmfw_rule r;
    nmfw_plan(&c, b, sizeof b);
    /* untrusted: a user allow rule can never open anything on that zone because the zone drop comes first */
    const char *z = strstr(b, "iifname \"usb*\" drop"), *ssh = strstr(b, "tcp dport { 22, 80 }");
    CHECK(z && ssh && z < ssh, "untrusted zone drop precedes user rules");
    const char *pub = strstr(b, "iifname \"wl0\" icmp type echo-request drop");
    CHECK(pub && pub < ssh, "public zone stealth precedes user rules");
    CHECK(strstr(b, "udp sport 67 udp dport 68 ip saddr != 192.168.1.1 drop"), "rogue dhcp server dropped");
    CHECK(strstr(b, "nd-router-advert ip6 saddr != fe80::1 drop"), "ra guard");
    CHECK(strstr(b, "oifname \"wl0\" tcp dport { 135, 139, 445 } drop") && !strstr(b, "oifname \"eth0\" tcp dport { 135"), "name-leak guard only on public/untrusted");
    nmfw_plan_arp(&c, b, sizeof b);
    CHECK(strstr(b, "arp saddr ip 192.168.1.1 arp saddr ether != aa:bb:cc:dd:ee:ff drop"), "gateway pin");
    c.gw_mac[0] = 0; CHECK(nmfw_check(&c) != 0, "half a gateway pin refused");
    c.gw_ip[0] = 0; CHECK(nmfw_check(&c) == 0 && nmfw_plan_arp(&c, b, sizeof b) == 0 && !b[0], "no pin, no arp table");
    c.ra_router[0] = 0; CHECK(nmfw_check(&c) != 0, "ra_guard without router refused");
    c = rich(); snprintf(c.gw_mac, 18, "aa:bb:cc:dd:ee:f"); CHECK(nmfw_check(&c) != 0, "short mac"); snprintf(c.gw_mac, 18, "aa:bb:cc:dd:ee:gg"); CHECK(nmfw_check(&c) != 0, "non-hex mac");
    c = rich(); snprintf(c.dhcp_server, 16, "fe80::1"); CHECK(nmfw_check(&c) != 0, "v6 dhcp server pin refused");
    nmfw_preset(&c, "workstation");
    CHECK(nmfw_preset(&c, "bogus") != 0 && nmfw_preset(&c, NULL) != 0 && nmfw_preset(&c, "share") != 0, "bad presets refused");
    CHECK(nmfw_set_zone(&c, "eth0", "bogus") != 0 && nmfw_set_zone(&c, "e\"th", "home") != 0 && nmfw_set_zone(&c, "", "home") != 0, "bad zone refused");
    for (int i = 0; i < NMFW_MAXZONES; i++) { char n[16]; snprintf(n, sizeof n, "z%d", i); nmfw_set_zone(&c, n, "home"); }
    CHECK(nmfw_set_zone(&c, "overflow", "home") != 0, "zone table full");
    nmfw_set_zone(&c, "z0", "public"); CHECK(c.zones[0].level == NMFW_PUBLIC, "zone level update in place");
    nmfw_preset(&c, "workstation"); memset(&r, 0, sizeof r); r.dir = NMFW_IN; r.act = NMFW_ACCEPT;
    for (int i = 0; i < NMFW_MAXRULES; i++) nmfw_add_rule(&c, &r);
    CHECK(nmfw_add_rule(&c, &r) != 0, "rule table full");
    CHECK(nmfw_share(&c, "a", "a") != 0 && nmfw_share(&c, "a*", "b") != 0 && nmfw_share(&c, NULL, "b") != 0, "bad share refused");
    nmfw_endpoint inj = {"1.2.3.4; flush ruleset", 1, 1}, ok = {"203.0.113.9", 51820, 1}, p0 = {"203.0.113.9", 0, 1};
    CHECK(nmfw_plan_killswitch("wg0", &inj, 1, 0, b, sizeof b) < 0 && nmfw_plan_killswitch("wg0", &ok, 0, 0, b, sizeof b) < 0 && nmfw_plan_killswitch("wg0", &p0, 1, 0, b, sizeof b) < 0
          && nmfw_plan_killswitch("w\"g", &ok, 1, 0, b, sizeof b) < 0 && nmfw_plan_killswitch(NULL, &ok, 1, 0, b, sizeof b) < 0, "killswitch refuses empty/injected/zero-port");
    CHECK(nmfw_preset_killswitch(&c, "wg0", &inj, 1) != 0 && nmfw_preset_killswitch(&c, "wg0", NULL, 0) != 0 && nmfw_preset_killswitch(&c, "wg*", &ok, 1) != 0, "killswitch preset refuses bad input");
}

/* ---------------------------------------------------------------- validation, guard, fuzz */
static void test_bad_input(void) {
    nmfw_cfg c; nmfw_rule r; nmfw__tab t[NMFW_MAXTAB];
    nmfw_preset(&c, "lockdown");
    CHECK(nmfw_plan(&c, b, sizeof b) > 0 && strstr(b, "table inet netman {") && strstr(b, "policy drop;"), "lockdown plan");
    CHECK(nmfw__plan_tables(b, t, NMFW_MAXTAB) == 1, "own-table guard accepts own plan");
    const char *foreign[] = {"table inet filter\nflush ruleset\n", "flush ruleset\n", "table inet filter\n", "delete table ip nat\n", "table inet netman\nflush table inet netman\n",
                             "add table inet netman\n", "table ip6 netmanX {\n}\ntable inet filter {\n}\n", "table foo netman\n", "table inet\n", "destroy table inet netman\n", "table inet netman x y\n", NULL};
    for (int i = 0; foreign[i]; i++) CHECK(nmfw__plan_tables(foreign[i], t, NMFW_MAXTAB) < 0, "guard accepted: %.40s", foreign[i]);
    CHECK(nmfw__plan_tables("table inet netman\ntable arp netman_arp\ntable inet netman_vpn_ks\n", t, NMFW_MAXTAB) == 3, "guard accepts our three tables");
    const char *bad[] = {"", "allow", "allow in tcp port 0", "allow in on x\"y", "allow in # a;b", "allow in from 999.1.1.1", "allow in rate 0", "allow sideways", "allow in tcp port 70000",
        "allow in tcp port 22 port 23", "allow in tcp udp", "allow in port 22", "allow in icmp port 1", "allow in v6 from 10.0.0.1", "allow in icmp v6", "allow in icmp6 v4", "allow in from 1.2.3.4/33",
        "allow in from ::1/129", "allow in from 1.2.3.4/", "allow in from 1.2.3.4 from 5.6.7.8", "allow in on a b", "allow in on a*b", "allow in on *", "allow in via eth0", "allow out via eth0",
        "allow in tcp port 1,2,3,4,5,6,7,8,9", "allow in tcp port 1,,2", "allow in tcp port -1", "allow in tcp port 22x", "allow in rate -3", "allow in rate 99999999", "allow in frob",
        "allow in\x01", "allow in # a\"b", "allow in # 012345678901234567890123456789012", "allow in on 0123456789abcdefX", "allow in from $(reboot)", "allow in from 1.2.3.4;drop", "allow in on eth0;flush", NULL};
    for (int i = 0; bad[i]; i++) CHECK(nmfw_parse_rule(bad[i], &r) != 0, "accepted '%s'", bad[i]);
    CHECK(nmfw_parse_rule(NULL, &r) != 0, "NULL rule");
    char longrule[600]; memset(longrule, 'a', sizeof longrule - 1); longrule[sizeof longrule - 1] = 0; CHECK(nmfw_parse_rule(longrule, &r) != 0, "overlong rule");
    CHECK(nmfw_parse_rule("allow in tcp port 22,80 from 10.0.0.0/8 on eth0 rate 5 # ssh", &r) == 0 && r.nports == 2 && r.rate == 5 && !strcmp(r.comment, "ssh"), "good rule: %s", nmfw_error());
    CHECK(nmfw_parse_rule("drop out udp", &r) == 0 && nmfw_parse_rule("accept fwd v4 on wl* via eth0 new est invalid", &r) == 0 && r.ct == 7 && nmfw_parse_rule("allow in icmp6 from fe80::/10", &r) == 0, "more good rules");

    /* hand-built structs with hostile strings never reach the ruleset */
    nmfw_cfg h; const char *evil[] = {"eth0\"", "eth0;", "e th", "eth0\n", "$(x)", "a{b", "", "0123456789abcdef", NULL};
    for (int i = 0; evil[i]; i++) {
        nmfw_preset(&h, "workstation"); memset(&r, 0, sizeof r); r.dir = NMFW_IN; r.act = NMFW_ACCEPT; snprintf(r.iface, 17, "%s", evil[i]);
        if (evil[i][0]) CHECK(nmfw_add_rule(&h, &r) != 0, "iface '%s' accepted by add_rule", evil[i]);
        h.nzones = 1; snprintf(h.zones[0].iface, 17, "%s", evil[i]); h.zones[0].level = NMFW_PUBLIC;
        CHECK(nmfw_plan(&h, b, sizeof b) < 0 && nmfw_plan_ipt(&h, 0, b, sizeof b) < 0, "zone iface '%s' reached a plan", evil[i]);
    }
    nmfw_preset(&h, "workstation"); h.pol_in = 7; CHECK(nmfw_plan(&h, b, sizeof b) < 0, "bad policy");
    nmfw_preset(&h, "workstation"); h.nrules = -1; CHECK(nmfw_check(&h) != 0, "negative count");
    nmfw_preset(&h, "workstation"); h.nrules = 100000; CHECK(nmfw_check(&h) != 0, "huge count");
    nmfw_preset(&h, "workstation"); h.masq = 1; CHECK(nmfw_check(&h) != 0, "masq without lan/wan");
    nmfw_preset(&h, "workstation"); h.ndnat = 1; snprintf(h.dnat[0].iface, 17, "eth0"); h.dnat[0].proto = NMFW_ICMP; CHECK(nmfw_check(&h) != 0, "dnat icmp");
    nmfw_preset(&h, "workstation"); h.limit_new = -1; CHECK(nmfw_check(&h) != 0, "negative limit");
    nmfw_preset(&h, "workstation"); h.pol_out = 5; CHECK(nmfw_plan(&h, b, sizeof b) < 0, "bad out policy");

    /* a plan buffer too small at every size: never overflows, never claims success with truncated text */
    nmfw_cfg g = rich(); int full = nmfw_plan(&g, b, sizeof b), ok = 1;
    char *tiny = malloc((size_t)full + 64);
    for (int cap = 0; cap < full + 2 && ok; cap += (cap < 300 ? 1 : 53)) {
        memset(tiny, 0x5a, (size_t)full + 64);
        int n = nmfw_plan(&g, tiny, (size_t)cap);
        if (cap <= full && n >= 0) ok = 0;
        if (cap > full && n != full) ok = 0;
        for (int i = cap; i < full + 64; i++) if (tiny[i] != 0x5a) { ok = 0; break; }
    }
    CHECK(ok, "plan buffer-size sweep (overflow or false success)");
    free(tiny);

    /* fuzz 1: random token soup into the parser; whatever it accepts must render to a plan that is clean text and passes nft -c / iptables --test */
    static const char *w[] = {"allow", "accept", "drop", "reject", "logdrop", "in", "out", "fwd", "tcp", "udp", "icmp", "icmp6", "v4", "v6", "port", "22", "22,80", "0", "65535", "70000", "from", "to", "on", "via",
        "eth0", "wl*", "10.0.0.0/8", "fe80::/10", "1.2.3.4", "::1", "999.0.0.1", "rate", "5", "new", "est", "invalid", "#", "x", "\"", ";", "{", "}", "$", "\\", "-1", "1e3", "%s", "\t", "//", "/"};
    int acc = 0, vetted = 0;
    for (int it = 0; it < 120000; it++) {
        char line[256]; size_t k = 0; int nw = 1 + (int)(rnd() % 10);
        line[0] = 0;
        for (int i = 0; i < nw; i++) { const char *tk = w[rnd() % (sizeof w / sizeof *w)]; k += (size_t)snprintf(line + k, sizeof line - k, "%s%s", i ? " " : "", tk); }
        if (nmfw_parse_rule(line, &r) == 0) {
            acc++;
            nmfw_preset(&h, rnd() % 2 ? "workstation" : "lockdown");
            if (nmfw_add_rule(&h, &r) != 0) { CHECK(0, "parser accepted but add_rule refused: %s", line); continue; }
            int n = nmfw_plan(&h, b, sizeof b), n4 = nmfw_plan_ipt(&h, 0, b2, sizeof b2);
            if (n < 0 || n4 < 0) { CHECK(0, "accepted rule did not plan: %s", line); continue; }
            for (int i = 0; i < n; i++) if ((unsigned char)b[i] < 0x20 && b[i] != '\n') { CHECK(0, "control char in plan for: %s", line); break; }
            if (acc % 40 == 0) { vetted++; vet_nft(line, b); vet_ipt(line, b2, 0); }
        }
    }
    printf("  fuzz parser: 120000 lines, %d accepted (%d vetted with nft -c / iptables --test)\n", acc, vetted);
    CHECK(acc > 100, "fuzz accepted too little to be meaningful (%d)", acc);

    /* fuzz 2: random bytes in the string fields of a cfg; any plan that renders must pass the table guard and be printable ASCII */
    int rendered = 0;
    for (int it = 0; it < 60000; it++) {
        nmfw_cfg f = rich();
        char *fields[] = {f.wan, f.lan, f.ra_router, f.dhcp_server, f.gw_ip, f.gw_mac, f.zones[0].iface, f.zones[1].iface, f.rules[0].iface, f.rules[0].addr, f.rules[1].comment, f.dnat[0].to, f.dnat[0].iface};
        size_t lens[] = {17, 17, 64, 16, 16, 18, 17, 17, 17, 64, 33, 64, 17};
        int nf = 1 + (int)(rnd() % 3);
        for (int i = 0; i < nf; i++) {
            size_t fi = rnd() % (sizeof fields / sizeof *fields), l = rnd() % lens[fi];
            for (size_t j = 0; j < l; j++) fields[fi][j] = (char)(rnd() % 4 ? " \";{}\n$*:./-_aZ09\\#"[rnd() % 19] : (char)rnd());
            fields[fi][l] = 0;
        }
        if (rnd() % 3 == 0) f.masq = 1;
        if (rnd() % 4 == 0) { f.ndnat = 1; f.dnat[0].proto = NMFW_TCP; f.dnat[0].port = 1; f.dnat[0].toport = 1; }
        int n = nmfw_plan(&f, b, sizeof b);
        if (n >= 0) {
            rendered++;
            nmfw__tab t2[NMFW_MAXTAB]; int unsafe = nmfw__plan_tables(b, t2, NMFW_MAXTAB) < 0;
            for (int i = 0; i < n && !unsafe; i++) if (((unsigned char)b[i] < 0x20 && b[i] != '\n') || (unsigned char)b[i] > 0x7e) unsafe = 1;
            if (unsafe) { CHECK(0, "fuzzed cfg rendered unsafe plan"); break; }
            if (nmfw_plan_ipt(&f, 0, b2, sizeof b2) < 0) { CHECK(0, "nft rendered but ipt refused"); break; }
            if (rendered % 500 == 0) { vet_nft("fuzzed cfg", b); vet_ipt("fuzzed cfg", b2, 0); }
        }
        nmfw_plan_arp(&f, b, sizeof b);
    }
    printf("  fuzz cfg: 60000 corrupted configs, %d still valid and rendered clean\n", rendered);
    CHECK(rendered > 10, "fuzz cfg rendered too few");
}

/* ---------------------------------------------------------------- live (inside unshare -Urn only) */
static int sh(const char *cmd) { return system(cmd); }
static int shout(const char *cmd, char *o, size_t n) { FILE *f = popen(cmd, "r"); if (!f) return -1; size_t k = fread(o, 1, n - 1, f); o[k] = 0; return pclose(f); }
static int has(const char *cmd, const char *needle) { static char o[1 << 16]; shout(cmd, o, sizeof o); return strstr(o, needle) != NULL; }
static void settle(double s) { struct timespec ts = {(time_t)s, (long)((s - (long)s) * 1e9)}; nanosleep(&ts, NULL); }

static int shout(const char *cmd, char *o, size_t n);
static int live_guard(void) {
    FILE *f = fopen("/proc/self/uid_map", "r"); char l[128] = "";
    if (f) { if (!fgets(l, sizeof l, f)) l[0] = 0; fclose(f); }
    if (strstr(l, "4294967295")) return 0;                        /* initial user namespace */
    char o[2048]; if (shout("ip -o link", o, sizeof o) != 0) return 0;      /* /sys is the host's: ask netlink about OUR namespace */
    int n = 0; for (char *p = o; *p; p++) if (*p == '\n') n++;     /* only "lo" may exist */
    return n <= 1 && (!n || strstr(o, ": lo:"));
}

static void test_live_nft(void) {
    char sd[] = "/tmp/fwstateXXXXXX"; if (!mkdtemp(sd)) { CHECK(0, "mkdtemp"); return; }
    nmfw_state_dir = sd; nmfw_force_backend = NMFW_B_NFT;
    static char o[1 << 15];
    CHECK(nmfw_backend() == NMFW_B_NFT, "nft backend");
    sh("nft add table inet foreign; nft add chain inet foreign keep '{ type filter hook input priority 0; policy accept; }'; nft add rule inet foreign keep counter comment foreignrule");
    sh("nft add table ip nat; nft add chain ip nat pre '{ type nat hook prerouting priority -100; }'");
    nmfw_cfg c = rich();
    CHECK(nmfw_apply(&c, 0) == 0, "apply nft: %s", nmfw_error());
    CHECK(has("nft list table inet netman", "chain c_in") && has("nft list table arp netman_arp", "pinned gateway"), "own tables present");
    CHECK(has("nft list table inet foreign", "foreignrule") && has("nft list table ip nat", "chain pre"), "foreign tables untouched");
    shout("nft list tables", o, sizeof o);
    CHECK(strstr(o, "inet netman") && strstr(o, "inet foreign") && strstr(o, "ip nat"), "table list");
    CHECK(nmfw_diff(&c, o, sizeof o) > 0 && strstr(o, "(no change)"), "diff after apply is empty");
    nmfw_rule r; nmfw_parse_rule("allow in tcp port 8443", &r); nmfw_add_rule(&c, &r);
    nmfw_diff(&c, o, sizeof o); CHECK(strstr(o, "+  ") && strstr(o, "8443") && !strstr(o, "\n- "), "diff shows only the new rule: %s", o);
    CHECK(nmfw_apply(&c, 0) == 0 && has("nft list table inet netman", "8443"), "re-apply replaces atomically");

    /* an invalid cfg is refused before nft is touched */
    nmfw_cfg bad = rich(); snprintf(bad.dhcp_server, 16, "999.1.1.1");
    CHECK(nmfw_apply(&bad, 0) != 0, "invalid cfg refused");
    CHECK(has("nft list table inet netman", "8443") && has("nft list table inet foreign", "foreignrule"), "refused apply leaves rules in place");

    /* confirm-or-rollback */
    nmfw_cfg c2; nmfw_preset(&c2, "lockdown");
    CHECK(nmfw_apply(&c2, 2) == 0, "apply with 2s timer: %s", nmfw_error());
    CHECK(!has("nft list table inet netman", "8443") && has("nft list table inet netman", "policy drop"), "new ruleset active while pending");
    CHECK(nmfw_confirm() == 0 && nmfw_confirm() != 0, "confirm once ok, second refused");
    settle(2.8);
    CHECK(!has("nft list table inet netman", "8443") && !has("nft list ruleset", "netman_arp") && has("nft list table inet netman", "policy drop"), "confirmed ruleset survives the timer");
    CHECK(nmfw_apply(&c, 2) == 0 && has("nft list table inet netman", "8443"), "apply the rich cfg again, with timer");
    settle(3.6);
    CHECK(!has("nft list table inet netman", "8443") && !has("nft list ruleset", "netman_arp") && has("nft list table inet netman", "policy drop"), "no confirm: timer restored the previous (lockdown) ruleset");
    CHECK(has("nft list table inet foreign", "foreignrule"), "foreign table survived rollback");
    CHECK(nmfw_confirm() != 0, "nothing pending after rollback");
    /* race: a confirm landing exactly between the watchdog's last poll and its rollback (forced interleaving). It used to be accepted AND rolled back. */
    nmfw__test_window = (void (*)(void))nmfw__claim;
    CHECK(nmfw_apply(&c, 1) == 0 && has("nft list table inet netman", "8443"), "apply for the race test");
    settle(2.5);
    nmfw__test_window = NULL;
    CHECK(has("nft list table inet netman", "8443"), "confirm won the race: the watchdog must NOT roll back");
    CHECK(nmfw_confirm() != 0, "pidfile already claimed: a second confirm is refused");
    nmfw__clear_pending();
    CHECK(nmfw_apply(&c2, 0) == 0 && !has("nft list table inet netman", "8443"), "back to lockdown after the race test");
    CHECK(nmfw_apply(&c, 60) == 0 && has("nft list table inet netman", "8443"), "apply with long timer");
    CHECK(nmfw_rollback() == 0 && !has("nft list table inet netman", "8443"), "manual rollback: %s", nmfw_error());
    CHECK(nmfw_remove() == 0 && !has("nft list tables", "inet netman") && has("nft list tables", "inet foreign"), "remove deletes only ours");
    CHECK(nmfw_apply(&c, 60) == 0 && nmfw_rollback() == 0 && !has("nft list tables", "inet netman") && !has("nft list tables", "netman_arp") && has("nft list tables", "inet foreign"),
          "rollback of a first-ever apply removes our tables: %s", nmfw_error());

    /* kill switch: same table name as vpn.h, foreign tables untouched */
    nmfw_endpoint ep = {"203.0.113.9", 51820, 1};
    CHECK(nmfw_killswitch("wg0", &ep, 1, 0, 1) == 0 && has("nft list table inet netman_vpn_ks", "oifname \"wg0\" accept"), "killswitch on: %s", nmfw_error());
    CHECK(has("nft list table inet foreign", "foreignrule"), "foreign table untouched by killswitch");
    CHECK(nmfw_killswitch(NULL, NULL, 0, 0, 0) == 0 && !has("nft list tables", "netman_vpn_ks") && nmfw_killswitch(NULL, NULL, 0, 0, 0) != 0, "killswitch off; a second off reports it");
    CHECK(nmfw_killswitch("wg0", NULL, 0, 0, 1) != 0 && !has("nft list tables", "netman_vpn_ks"), "killswitch without endpoints refused, nothing armed");
    CHECK(nmfw_preset_killswitch(&c2, "wg0", &ep, 1) == 0 && nmfw_apply(&c2, 0) == 0 && has("nft list table inet netman", "oifname \"wg0\" accept"), "killswitch preset via the main table: %s", nmfw_error());
    nmfw_remove();

    /* share turns forwarding on and rollback restores it */
    sh("echo 0 > /proc/sys/net/ipv4/ip_forward");
    nmfw_cfg s; nmfw_share(&s, "ap0", "eth0");
    CHECK(nmfw_apply(&s, 60) == 0 && has("cat /proc/sys/net/ipv4/ip_forward", "1") && has("nft list table inet netman", "masquerade"), "share apply turns forwarding on: %s", nmfw_error());
    CHECK(nmfw_rollback() == 0 && has("cat /proc/sys/net/ipv4/ip_forward", "0"), "rollback restores ip_forward");
    nmfw_remove();
    sh("nft delete table inet foreign; nft delete table ip nat");
    char rm[300]; snprintf(rm, sizeof rm, "rm -rf %s", sd); sh(rm);
}

static void test_live_ipt(void) {
    char sd[] = "/tmp/fwstateXXXXXX"; if (!mkdtemp(sd)) { CHECK(0, "mkdtemp"); return; }
    nmfw_state_dir = sd; nmfw_force_backend = NMFW_B_IPT;
    if (sh("iptables -w -L -n >/dev/null 2>&1") != 0) { puts("  SKIP iptables live tests: iptables not usable in this namespace"); return; }
    sh("iptables -w -N FOREIGN; iptables -w -A FOREIGN -p tcp --dport 4242 -j ACCEPT; iptables -w -A INPUT -j FOREIGN");
    nmfw_cfg c = rich(); nmfw_rule r;
    CHECK(nmfw_apply(&c, 0) == 0, "iptables apply: %s", nmfw_error());
    CHECK(has("iptables -w -S", "-N NETMAN") && has("iptables -w -S INPUT", "-j NETMAN") && has("iptables -w -S NETMAN", "22,80"), "own chain + jump present");
    CHECK(has("iptables -w -S FOREIGN", "4242") && has("iptables -w -S INPUT", "-j FOREIGN"), "foreign chain untouched");
    shout("iptables -w -S INPUT", b, sizeof b);
    CHECK(strstr(b, "-j NETMAN\n") && strstr(b, "-j FOREIGN") && strstr(b, "-j NETMAN\n") < strstr(b, "-j FOREIGN"), "jump inserted first, foreign jump kept");
    nmfw_parse_rule("allow in tcp port 8443", &r); nmfw_add_rule(&c, &r);
    CHECK(nmfw_apply(&c, 2) == 0 && has("iptables -w -S NETMAN", "8443"), "iptables apply with timer");
    settle(3.6);
    CHECK(!has("iptables -w -S NETMAN", "8443") && has("iptables -w -S NETMAN", "22,80"), "timer rolled back to the previous chain content");
    CHECK(has("iptables -w -S FOREIGN", "4242"), "foreign chain survived rollback");
    CHECK(nmfw_remove() == 0 && !has("iptables -w -S", "NETMAN") && has("iptables -w -S", "-N FOREIGN"), "iptables remove: ours only");
    nmfw_cfg s; nmfw_share(&s, "ap0", "eth0");
    CHECK(nmfw_apply(&s, 60) == 0 && has("iptables -w -t nat -S", "MASQUERADE") && has("iptables -w -t nat -S POSTROUTING", "NETMAN_POST"), "iptables share nat: %s", nmfw_error());
    CHECK(nmfw_rollback() == 0 && !has("iptables -w -t nat -S", "NETMAN"), "iptables first-apply rollback removes chains: %s", nmfw_error());
    nmfw_remove();
    sh("iptables -w -D INPUT -j FOREIGN; iptables -w -F FOREIGN; iptables -w -X FOREIGN");
    char rm[300]; snprintf(rm, sizeof rm, "rm -rf %s", sd); sh(rm);
}

int main(int argc, char **argv) {
    gdir = "fw_golden";
    if (access(gdir, R_OK)) gdir = "tests/fw_golden";
    if (argc > 1 && !strcmp(argv[1], "ns")) {
        execlp("unshare", "unshare", "-Urn", argv[0], "inner", (char *)NULL); perror("unshare"); return 2;
    }
    setenv("XTABLES_LOCKFILE", "/tmp/fw_test_xt.lock", 1);
    have_nft_c = system("echo \"table inet fwprobe\" | nft -c -f - >/dev/null 2>&1") == 0 && system("nft list tables >/dev/null 2>&1") == 0;
    have_ipt_test = system("echo '*filter\nCOMMIT' | iptables-restore --noflush --test >/dev/null 2>&1") == 0;
    if (!have_nft_c) puts("SKIP nft -c vetting (nft not permitted here; use `ns` mode)");
    if (!have_ipt_test) puts("SKIP iptables-restore --test vetting (not permitted here; use `ns` mode)");
    test_golden(); test_zone_semantics(); test_bad_input();
    if (argc > 1 && !strcmp(argv[1], "inner")) {
        if (!live_guard()) { puts("refusing live tests: not in a throwaway user+net namespace"); return 2; }
        sh("ip link set lo up 2>/dev/null");
        test_live_nft(); test_live_ipt();
    }
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
