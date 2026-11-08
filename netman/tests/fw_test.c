/* fw_test.c - minimal fw.h tests: golden render, validation, own-table guard, nft -c / iptables-restore --test.
 * gcc -std=gnu11 -Wall -Wextra -O2 -I.. fw_test.c -o fw_test ; unshare -Urn ./fw_test   (check mode needs no network changes) */
#define NETMAN_FW_IMPLEMENTATION
#include "fw.h"
static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); puts(""); } } while (0)
static char b[1 << 16];
static int vet(const char *plan, const char *cmd) {
    char p[] = "/tmp/fwplanXXXXXX"; int fd = mkstemp(p); if (fd < 0) return -1;
    if (write(fd, plan, strlen(plan)) < 0) { close(fd); return -1; } close(fd);
    char c[400]; snprintf(c, sizeof c, "%s < %s >/dev/null 2>&1", cmd, p);
    int r = system(c); unlink(p); return r;
}
int main(void) {
    nmfw_cfg c; nmfw_rule r; nmfw__tab t[NMFW_MAXTAB];
    nmfw_preset(&c, "lockdown");
    CHECK(nmfw_plan(&c, b, sizeof b) > 0 && strstr(b, "table inet netman {") && strstr(b, "policy drop;"), "lockdown plan");
    CHECK(nmfw__plan_tables(b, t, NMFW_MAXTAB) == 1, "own-table guard accepts own plan");
    CHECK(nmfw__plan_tables("table inet filter\nflush ruleset\n", t, NMFW_MAXTAB) < 0, "guard refuses foreign");
    const char *bad[] = {"", "allow", "allow in tcp port 0", "allow in on x\"y", "allow in # a;b", "allow in from 999.1.1.1", "allow in rate 0", "allow sideways", NULL};
    for (int i = 0; bad[i]; i++) CHECK(nmfw_parse_rule(bad[i], &r) != 0, "accepted '%s'", bad[i]);
    CHECK(nmfw_parse_rule("allow in tcp port 22,80 from 10.0.0.0/8 on eth0 rate 5 # ssh", &r) == 0, "good rule: %s", nmfw_error());
    nmfw_preset(&c, "workstation"); nmfw_add_rule(&c, &r); nmfw_set_zone(&c, "wl0", "public"); nmfw_share(&c, "wl0", "eth0");
    int n = nmfw_plan(&c, b, sizeof b);
    CHECK(n > 0 && strstr(b, "masquerade"), "share plan");
    if (system("nft list tables >/dev/null 2>&1") == 0) CHECK(vet(b, "nft -c -f -") == 0, "nft -c rejects plan");
    else puts("SKIP nft -c (not available/permitted here)");
    n = nmfw_plan_ipt(&c, 0, b, sizeof b);
    CHECK(n > 0 && !strstr(b, "-F") && !strstr(b, ":INPUT"), "ipt plan touches only own chains");
    if (system("iptables-restore --version >/dev/null 2>&1") == 0) CHECK(vet(b, "iptables-restore --noflush --test") == 0, "iptables-restore --test rejects plan");
    nmfw_endpoint ep = {"203.0.113.9", 51820, 1};
    CHECK(nmfw_plan_killswitch("wg0", &ep, 1, 0, b, sizeof b) > 0 && strstr(b, "nmbv_ks"), "killswitch");
    CHECK(nmfw_plan_killswitch("wg0", &ep, 0, 0, b, sizeof b) < 0, "no endpoints refused");
    nmfw_endpoint inj = {"1.2.3.4; flush ruleset", 1, 1};
    CHECK(nmfw_plan_killswitch("wg0", &inj, 1, 0, b, sizeof b) < 0, "injection refused");
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
