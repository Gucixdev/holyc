/* bonding.c - see bonding.h. A thin front end over enterprise/enterprise.c; state comes from sysfs (netman_ent_sys). */
#include "modkit.h"
#include "net.h"
#include "bonding.h"
#include "enterprise/enterprise.h"

const char *netman_bond_modes[7] = { "balance-rr", "active-backup", "balance-xor", "broadcast", "802.3ad", "balance-tlb", "balance-alb" };
static const char *const mode_help[7] = {
	"packets round-robin over the slaves (switch needs a static trunk)", "one slave carries, another takes over on failure (no switch support needed)",
	"slave chosen by a hash of the addresses (static trunk)", "every packet on every slave (fault tolerance, wasteful)",
	"IEEE 802.3ad LACP: aggregate with a partner that speaks LACP", "adaptive transmit load balancing (no switch support)", "tlb plus receive balancing by ARP (no switch support)"
};

static char *sysf(char *b, size_t n, const char *bond, const char *file, const char *sub)
{
	char p[300];
	snprintf(p, sizeof p, "%s/class/net/%s/%s/%s", netman_ent_sys, bond, sub, file);
	if (ent_slurp(p, b, n) < 0) b[0] = 0;
	for (size_t i = strlen(b); i && (b[i - 1] == '\n' || b[i - 1] == ' '); ) b[--i] = 0;
	return b;
}
static int slave_up(const char *s)
{
	char p[300], b[32];
	snprintf(p, sizeof p, "%s/class/net/%s/operstate", netman_ent_sys, s);
	if (ent_slurp(p, b, sizeof b) < 0) return 0;
	return !strncmp(b, "up", 2);
}
static int bonds(char out[][32], int max)
{
	char dir[200];
	DIR *d;
	struct dirent *e;
	int n = 0;
	snprintf(dir, sizeof dir, "%s/class/net", netman_ent_sys);
	if (!(d = opendir(dir))) return 0;
	while ((e = readdir(d)) && n < max) {
		char p[600];
		if (e->d_name[0] == '.' || strlen(e->d_name) >= 32) continue;
		snprintf(p, sizeof p, "%s/%s/bonding/mode", dir, e->d_name);
		if (mk_exists(p)) strcpy(out[n++], e->d_name);
	}
	closedir(d);
	for (int i = 1; i < n; i++)
		for (int j = i; j > 0 && strcmp(out[j - 1], out[j]) > 0; j--) { char t[32]; memcpy(t, out[j], 32); memcpy(out[j], out[j - 1], 32); memcpy(out[j - 1], t, 32); }
	return n;
}

int netman_bond_status(FILE *out)
{
	char names[32][32];
	int n = bonds(names, 32);
	for (int i = 0; i < n; i++) {
		char mode[64], sl[256], act[32], mii[16], *t, *sv = NULL;
		int ns = 0, nup = 0;
		sysf(mode, sizeof mode, names[i], "mode", "bonding");
		sysf(sl, sizeof sl, names[i], "slaves", "bonding");
		sysf(act, sizeof act, names[i], "active_slave", "bonding");
		sysf(mii, sizeof mii, names[i], "mii_status", "bonding");
		for (t = strtok_r(sl, " ", &sv); t; t = strtok_r(NULL, " ", &sv)) { ns++; nup += slave_up(t); }
		{
			char *sp = strchr(mode, ' ');   /* sysfs: "active-backup 1" */
			if (sp) *sp = 0;
		}
		fprintf(out, "%s %s %s  slaves %d/%d up%s%s\n", names[i], !ns || !nup ? "down" : nup < ns ? "degraded" : "up", mode, nup, ns, act[0] && strcmp(act, "None") ? "  active " : "", act[0] && strcmp(act, "None") ? act : "");
	}
	return n;
}

static int load_prof(const char *name, netman_ent *e)
{
	int r = netman_ent_load(name, e);
	return r < 0 ? r : (strcmp(e->kind, "bond") && strcmp(e->kind, "team") ? -EINVAL : 0);
}
static int forget_prof(const char *name)
{
	char p[300];
	snprintf(p, sizeof p, "%s/%s.ent", netman_conf_dir, name);
	return unlink(p);
}
static int slave_prof(const char *bond, const char *slave, int add)
{
	netman_ent e;
	int i;
	if (load_prof(bond, &e) < 0) return 0;   /* live-only bond: nothing to keep in step */
	for (i = 0; i < e.nmember; i++)
		if (!strcmp(e.member[i], slave)) break;
	if (add && i == e.nmember && e.nmember < NETMAN_ENT_MEMBERS) ent_cpy(e.member[e.nmember++], 16, slave);
	if (!add && i < e.nmember) { memmove(e.member[i], e.member[i + 1], (size_t)(e.nmember - i - 1) * 16); e.nmember--; }
	return netman_ent_save(&e);
}

static int mode_ok(const char *m)
{
	for (int i = 0; i < 7; i++)
		if (!strcmp(m, netman_bond_modes[i]) || (m[0] == '0' + i && !m[1])) return 1;
	return 0;
}

int netman_bond_main(FILE *out, FILE *err, int argc, char **argv)
{
	const char *c = argc > 0 ? argv[0] : "status";
	int r;
	if (!strcmp(c, "status") || !strcmp(c, "list")) {
		r = netman_bond_status(out);
		if (r == 0) fputs("no bonds\n", out);
		return 0;
	}
	if (!strcmp(c, "modes")) {
		for (int i = 0; i < 7; i++) fprintf(out, "%d  %-14s %s\n", i, netman_bond_modes[i], mode_help[i]);
		return 0;
	}
	if (!strcmp(c, "show") && argc == 2) {
		netman_ent e;
		char a[300], b[64];
		netman_ent_init(&e);
		ent_cpy(e.kind, sizeof e.kind, "bond");
		ent_cpy(e.name, sizeof e.name, argv[1]);
		if ((r = netman_ent_bond_show(&e, out)) < 0) { fprintf(err, "bond %s: %s\n", argv[1], r == -ENODEV ? "no such bond" : netman_ent_error()); return 1; }
		sysf(b, sizeof b, argv[1], "ad_select", "bonding");
		if (b[0]) { fprintf(out, "  ad_select %s", b); sysf(b, sizeof b, argv[1], "ad_num_ports", "bonding"); (void)a; if (b[0]) fprintf(out, "  ports %s", b); fputc('\n', out); }
		fprintf(out, "  profile: %s\n", load_prof(argv[1], &e) == 0 ? "saved (netman bond apply recreates it)" : "none (live only)");
		return 0;
	}
	if (!strcmp(c, "add") && argc >= 3) {
		netman_ent e;
		netman_ent_init(&e);
		ent_cpy(e.kind, sizeof e.kind, "bond");
		if (!mk_name_ok(argv[1]) || strlen(argv[1]) > 15) { fprintf(err, "bond: bad name '%s'\n", argv[1]); return 2; }
		ent_cpy(e.name, sizeof e.name, argv[1]);
		for (int i = 2; i < argc; i++) {
			char *eq = strchr(argv[i], '='), k[24];
			if (!eq) {
				if (e.nmember >= NETMAN_ENT_MEMBERS || !mk_name_ok(argv[i]) || strlen(argv[i]) > 15) { fprintf(err, "bond: bad or too many slaves ('%s', max %d)\n", argv[i], NETMAN_ENT_MEMBERS); return 2; }
				ent_cpy(e.member[e.nmember++], 16, argv[i]);
				continue;
			}
			snprintf(k, sizeof k, "%.*s", (int)(eq - argv[i]), argv[i]);
			eq++;
			if (!strcmp(k, "mode")) { if (!mode_ok(eq)) { fprintf(err, "bond: unknown mode '%s' (netman bond modes)\n", eq); return 2; } ent_cpy(e.mode, sizeof e.mode, eq); }
			else if (!strcmp(k, "miimon")) { if (mk_int(eq, 0, 10000, &e.miimon)) { fputs("bond: miimon is 0..10000 ms\n", err); return 2; } }
			else if (!strcmp(k, "mtu")) { if (mk_int(eq, 68, 65535, &e.mtu)) { fputs("bond: bad mtu\n", err); return 2; } }
			else if (!strcmp(k, "primary")) ent_cpy(e.primary, sizeof e.primary, eq);
			else if (!strcmp(k, "lacp_rate")) ent_cpy(e.lacp_rate, sizeof e.lacp_rate, eq);
			else if (!strcmp(k, "hash")) ent_cpy(e.hash, sizeof e.hash, eq);
			else if (!strcmp(k, "addr")) ent_cpy(e.addr, sizeof e.addr, eq);
			else { fprintf(err, "bond: unknown key '%s'\n", k); return 2; }
		}
		if ((r = netman_ent_check(&e)) < 0) { fprintf(err, "bond: %s\n", netman_ent_error()); return 2; }
		if ((r = netman_ent_bond_add(&e)) < 0) { fprintf(err, "bond %s: %s\n", e.name, r == -EEXIST ? "already exists (netman bond del first)" : netman_ent_error()); return 1; }
		if ((r = netman_ent_save(&e)) < 0) { fprintf(err, "bond %s created, but the profile was not saved: %s\n", e.name, netman_ent_error()); return 1; }
		fprintf(out, "bond %s up with %d slave%s (profile saved)\n", e.name, e.nmember, e.nmember == 1 ? "" : "s");
		return 0;
	}
	if (!strcmp(c, "del") && argc == 2) {
		netman_ent e;
		netman_ent_init(&e);
		ent_cpy(e.kind, sizeof e.kind, "bond");
		ent_cpy(e.name, sizeof e.name, argv[1]);
		if (!mk_name_ok(argv[1])) return 2;
		r = netman_ent_bond_del(&e);
		if (r < 0 && r != -ENODEV) { fprintf(err, "bond %s: %s\n", argv[1], strerror(-r)); return 1; }
		if (forget_prof(argv[1]) < 0 && r == -ENODEV) { fprintf(err, "bond %s: no such bond or profile\n", argv[1]); return 1; }
		fprintf(out, "bond %s removed\n", argv[1]);
		return 0;
	}
	if (!strcmp(c, "slave") && argc == 4 && (!strcmp(argv[1], "add") || !strcmp(argv[1], "del"))) {
		int add = !strcmp(argv[1], "add");
		if (!mk_name_ok(argv[2]) || !mk_name_ok(argv[3])) return 2;
		if ((r = netman_ent_bond_slave(argv[2], argv[3], add)) < 0) { fprintf(err, "bond: %s\n", netman_ent_error()); return 1; }
		slave_prof(argv[2], argv[3], add);
		fprintf(out, "%s %s %s\n", argv[3], add ? "enslaved to" : "released from", argv[2]);
		return 0;
	}
	if (!strcmp(c, "apply") && argc == 1) {
		char names[64][64];
		int n = netman_ent_list(names, 64), bad = 0, done = 0;
		for (int i = 0; i < n; i++) {
			netman_ent e;
			if (netman_ent_load(names[i], &e) < 0 || (strcmp(e.kind, "bond") && strcmp(e.kind, "team"))) continue;
			r = netman_ent_add(&e);
			if (r < 0 && r != -EEXIST) { fprintf(err, "bond %s: %s\n", names[i], netman_ent_error()); bad++; } else { done++; fprintf(out, "%s %s\n", names[i], r == -EEXIST ? "already there" : "created"); }
		}
		if (!done && !bad) fputs("no saved bond profiles\n", out);
		return bad ? 1 : 0;
	}
	fputs("usage: netman bond status|list|show NAME|modes|add NAME [k=v] SLAVE...|del NAME|slave add|del NAME IF|apply\n", err);
	return 2;
}
