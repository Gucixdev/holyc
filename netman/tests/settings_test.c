/* settings_test.c - the settings schema: every config key has an entry, kinds/ranges/choices are sane, hidden/gate/source logic.
 * cc -std=gnu11 -Isrc tests/settings_test.c src/settings.c src/config.c -o /tmp/settings_test && /tmp/settings_test */
#define _GNU_SOURCE
#include "settings.h"
#include <stdlib.h>
#include <string.h>
static int fails, total;
#define CHECK(c, ...) do { total++; if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); puts(""); } } while (0)
int main(void)
{
	netman_config c; char ch[32][24], key[96]; int k, i, n;
	netman_config_defaults(&c);
	for (k = 0; k < NC_N; k++) {
		snprintf(key, sizeof key, "%s.%s", netman_config_section(k), netman_config_name(k));
		CHECK(nset_find(key) == k, "%s: nset_find", key);
		CHECK(nset_tab(k) && *nset_tab(k), "%s has no tab (add its section to the table in settings.c)", key);
		CHECK(nset_tab_index(k) >= 0 && nset_tab_index(k) < nset_ntabs(), "%s: tab index", key);
		char sh[128]; nset_short(k, sh, sizeof sh);
		CHECK(*sh && strlen(sh) <= 90, "%s: short description '%s'", key, sh);
		CHECK(!strcmp(nset_apply(k), "live") || !strcmp(nset_apply(k), "restart"), "%s: apply", key);
		switch (nset_kind(k)) {
		case NSET_INT: { long lo, hi; CHECK(nset_range(k, &lo, &hi) && lo <= hi, "%s: int range", key); break; }
		case NSET_ENUM: n = nset_choices(k, ch, 32); CHECK(n >= 2, "%s: enum choices", key);
			for (i = 0, total++; i < n; i++) if (!strcmp(ch[i], netman_config_default(k))) break;
			if (i == n) { fails++; printf("FAIL: %s: default '%s' is not a choice\n", key, netman_config_default(k)); }
			break;
		case NSET_BOOL: CHECK(nset_choices(k, ch, 32) == 2, "%s: bool choices", key); break;
		default: break;
		}
		CHECK(nset_is_default(&c, k), "%s: default config differs from default", key);
		CHECK(!strcmp(nset_source(&c, k), "default"), "%s: source of default is %s", key, nset_source(&c, k));
		if (nset_is_gate(k)) CHECK(nset_gate_key(k) == k && !nset_hidden(&c, k), "%s: gate never hides itself", key);
	}
	/* every section in the table owns keys; every key's section is in the table */
	for (i = 0; i < nset_nsecs(); i++) {
		int have = 0; for (k = 0; k < NC_N; k++) if (!strcmp(netman_config_section(k), nset_secname(i))) have = 1;
		CHECK(have, "schema section [%s] has no config keys", nset_secname(i));
	}
	CHECK(nset_ntabs() >= 5, "tabs");
	/* hidden: a key of a switched-off module is hidden, the switch is not */
	{ int sw = nset_find("ssh.enabled"), d = nset_find("ssh.dir"); netman_config_defaults(&c);
	  CHECK(sw >= 0 && d >= 0 && nset_is_gate(sw), "ssh gate");
	  CHECK(nset_hidden(&c, d) == !netman_config_bool(&c, sw), "ssh.dir hidden follows ssh.enabled"); }
	CHECK(nset_find("nope.nothing") == -1 && nset_find("wifi") == -1, "find unknown");
	printf("settings_test: %d checks, %d failed\n", total, fails);
	return fails != 0;
}
