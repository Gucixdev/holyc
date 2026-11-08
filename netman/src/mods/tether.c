/* tether.c - tethering: detect USB RNDIS/CDC phones and Bluetooth PAN ifaces, use a phone as uplink (client), or share
 * this machine's uplink out to a downstream iface (nftables masquerade + dnsmasq DHCP/DNS). API in enterprise.h.
 *
 * The share-out ruleset lives in its OWN table, inet netman_tether, because firewall.h replaces `inet netman` wholesale.
 * Caveat: an accept here cannot override a drop in another table's forward hook; a strict firewall must allow the pair.
 * Optional helpers (exec'd, no shell): nft, dnsmasq. Bluetooth PAN is bluez D-Bus, built with -DNETMAN_ENT_DBUS.
 */
#define _GNU_SOURCE
#include "enterprise.h"
#include "entcore.h"
#include "net.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TH_NM(s) ent_safe_name((s), 16)

int netman_ent_tether_detect(netman_ent_tether_dev *out, int max)
{
	static const char *const usb[] = {"rndis_host", "cdc_ncm", "cdc_ether", "ipheth", "cdc_eem", "cdc_mbim", "huawei_cdc_ncm", "sierra_net", NULL};
	char dir[300], p[600], tgt[300];
	struct dirent **ls = NULL;
	int n, i, c = 0, k;
	snprintf(dir, sizeof dir, "%s/class/net", netman_ent_sys);
	if ((n = scandir(dir, &ls, NULL, alphasort)) < 0) return errno == ENOENT ? 0 : -errno;
	for (i = 0; i < n; i++) {
		const char *nm = ls[i]->d_name;
		if (nm[0] != '.' && c < max && strlen(nm) < 16) {
			ssize_t l;
			snprintf(p, sizeof p, "%s/%s/device/driver", dir, nm);
			l = readlink(p, tgt, sizeof tgt - 1);
			if (l > 0) {
				const char *drv;
				tgt[l] = 0;
				drv = strrchr(tgt, '/');
				drv = drv ? drv + 1 : tgt;
				for (k = 0; usb[k]; k++)
					if (!strcmp(drv, usb[k])) {
						ent_cpy(out[c].ifn, 16, nm);
						ent_cpy(out[c].driver, 24, drv);
						strcpy(out[c].kind, "usb");
						c++;
						break;
					}
			} else if (!strncmp(nm, "bnep", 4)) {
				ent_cpy(out[c].ifn, 16, nm);
				strcpy(out[c].driver, "bnep");
				strcpy(out[c].kind, "bt");
				c++;
			}
		}
		free(ls[i]);
	}
	free(ls);
	return c;
}

static int th_mask(int plen, char *o, size_t n)
{
	uint32_t m = plen <= 0 ? 0 : htonl(~0u << (32 - plen));
	return inet_ntop(AF_INET, &m, o, n) ? 0 : -EINVAL;
}

int netman_ent_tether_nft(const netman_ent *e, char *out, size_t cap)
{
	struct ent_out o = {out, 0, cap};
	if (cap) out[0] = 0;
	if (!TH_NM(e->name) || (e->upstream[0] && !TH_NM(e->upstream))) return ent_fail(-EINVAL, "tether: bad interface name");
	ent_put(&o, ENT_HDR "table inet netman_tether {}\ndelete table inet netman_tether\ntable inet netman_tether {\n");
	ent_put(&o, "\tchain forward {\n\t\ttype filter hook forward priority 0; policy accept;\n");
	if (e->upstream[0]) ent_put(&o, "\t\tiifname \"%s\" oifname \"%s\" accept\n\t\tiifname \"%s\" oifname \"%s\" ct state established,related accept\n", e->name, e->upstream, e->upstream, e->name);
	else ent_put(&o, "\t\tiifname \"%s\" accept\n\t\toifname \"%s\" ct state established,related accept\n", e->name, e->name);
	ent_put(&o, "\t}\n\tchain postrouting {\n\t\ttype nat hook postrouting priority 100; policy accept;\n");
	if (e->upstream[0]) ent_put(&o, "\t\tiifname \"%s\" oifname \"%s\" masquerade\n", e->name, e->upstream);
	else ent_put(&o, "\t\tiifname \"%s\" oifname != \"%s\" masquerade\n", e->name, e->name);
	ent_put(&o, "\t}\n}\n");
	return o.n >= cap ? -E2BIG : (int)o.n;
}

int netman_ent_tether_dnsmasq(const netman_ent *e, char *out, size_t cap)
{
	struct ent_out o = {out, 0, cap};
	ent_ip a;
	char ip[48], mask[24], start[46], end[46];
	if (cap) out[0] = 0;
	if (!TH_NM(e->name)) return ent_fail(-EINVAL, "tether: bad interface name");
	if (ent_parse_ip(e->addr, &a, 1) < 0 || a.fam != AF_INET) return ent_fail(-EINVAL, "tether needs address=A.B.C.D/len for the downstream side");
	inet_ntop(AF_INET, a.b, ip, sizeof ip);
	th_mask(a.plen, mask, sizeof mask);
	if (e->dhcp_start[0] && e->dhcp_end[0]) {
		ent_cpy(start, sizeof start, e->dhcp_start);
		ent_cpy(end, sizeof end, e->dhcp_end);
	} else if (a.plen <= 24 && a.plen >= 8) { /* default pool: .10-.100 of the address's own /24 */
		snprintf(start, sizeof start, "%d.%d.%d.10", a.b[0], a.b[1], a.b[2]);
		snprintf(end, sizeof end, "%d.%d.%d.100", a.b[0], a.b[1], a.b[2]);
	} else return ent_fail(-EINVAL, "tether: set dhcp_start= and dhcp_end= for a /%d network", a.plen);
	ent_put(&o, ENT_HDR "interface=%s\nbind-interfaces\nexcept-interface=lo\nlisten-address=%s\nno-resolv\ndhcp-leasefile=%s/tether-%s.leases\n", e->name, ip, netman_run_dir, e->name);
	if (e->dns[0]) ent_put(&o, "server=%s\n", e->dns);
	else ent_put(&o, "resolv-file=/etc/resolv.conf\n");
	ent_put(&o, "dhcp-range=%s,%s,%s,12h\ndhcp-option=3,%s\ndhcp-option=6,%s\n", start, end, mask, ip, ip);
	return o.n >= cap ? -E2BIG : (int)o.n;
}

static int th_forward(int on)
{
	char p[300];
	int fd;
	snprintf(p, sizeof p, "%s/sys/net/ipv4/ip_forward", netman_ent_proc);
	if ((fd = open(p, O_WRONLY | O_CLOEXEC)) < 0) return -errno;
	if (write(fd, on ? "1\n" : "0\n", 2) != 2) {
		int r = -errno;
		close(fd);
		return r;
	}
	close(fd);
	return 0;
}
static void th_path(char *p, size_t n, const char *name, const char *ext) { snprintf(p, n, "%s/tether-%s.%s", netman_run_dir, name, ext); }

static int th_client(const netman_ent *e)
{
	netman_lease l;
	int r = netman_link_set(e->name, 1);
	if (r < 0) return r;
	if ((r = netman_dhcp_acquire(e->name, netman_dhcp_timeout_ms, &l)) < 0) return ent_fail(r, "tether %s: no DHCP answer from the phone", e->name);
	return netman_lease_apply(&l);
}

int netman_ent_tether_add(const netman_ent *e)
{
	char nft[1024], dns[1024], cf[300], pf[300], err[256];
	char *nargv[] = {"nft", "-f", "-", NULL};
	char *dargv[] = {"dnsmasq", "-C", cf, "-x", pf, NULL};
	int r, bt = !strcmp(e->mode, "panu") || !strcmp(e->mode, "nap");
	if ((r = netman_ent_check(e)) < 0) return r;
	if (!strcmp(e->mode, "client") || !strcmp(e->mode, "panu")) {
		if (!strcmp(e->mode, "panu")) {
			char ifn[16];
			if ((r = netman_ent_btpan_connect(e->btmac, "panu", ifn, sizeof ifn)) < 0) return r;
			if (!strcmp(e->mode, "panu") && e->name[0]) { /* name= is the expected bnep iface; use what bluez made */
				netman_ent x = *e;
				ent_cpy(x.name, 16, ifn[0] ? ifn : e->name);
				return th_client(&x);
			}
		}
		return th_client(e);
	}
	if (e->mode[0] && strcmp(e->mode, "share") && strcmp(e->mode, "nap")) return ent_fail(-EINVAL, "tether mode must be share, client, panu or nap");
	if (bt && (r = netman_ent_btpan_server(1, e->name)) < 0) return r;
	if ((r = netman_ent_tether_nft(e, nft, sizeof nft)) < 0) return r;
	if ((r = netman_ent_tether_dnsmasq(e, dns, sizeof dns)) < 0) return r;
	if ((r = ent_mkdirs(netman_run_dir)) < 0) return r;
	if ((r = netman_addr_add(e->name, e->addr)) < 0) return r;
	netman_link_set(e->name, 1);
	if ((r = th_forward(1)) < 0) return ent_fail(r, "cannot enable ip_forward: %s", strerror(-r));
	if ((r = ent_exec(nargv, nft, err, sizeof err)) < 0) return ent_fail(r, "nft (optional helper) failed: %s", r == -ENOENT ? "not installed" : err);
	th_path(cf, sizeof cf, e->name, "conf");
	th_path(pf, sizeof pf, e->name, "pid");
	if ((r = ent_write_atomic(cf, dns, strlen(dns), 0644)) < 0) return r;
	if ((r = ent_exec(dargv, NULL, err, sizeof err)) < 0) return ent_fail(r, "dnsmasq (optional helper) failed: %s", r == -ENOENT ? "not installed" : err);
	return 0;
}

int netman_ent_tether_del(const netman_ent *e)
{
	char cf[300], pf[300], b[32];
	char *nargv[] = {"nft", "delete", "table", "inet", "netman_tether", NULL};
	char err[128];
	if (!strcmp(e->mode, "client") || !strcmp(e->mode, "panu")) {
		netman_lease l;
		memset(&l, 0, sizeof l);
		ent_cpy(l.ifn, 16, e->name);
		netman_addr_flush(e->name);
		return 0;
	}
	th_path(cf, sizeof cf, e->name, "conf");
	th_path(pf, sizeof pf, e->name, "pid");
	if (ent_slurp(pf, b, sizeof b) > 0 && atoi(b) > 1) kill(atoi(b), SIGTERM);
	unlink(pf);
	unlink(cf);
	ent_exec(nargv, NULL, err, sizeof err); /* best effort: absent table or no nft is fine */
	netman_addr_flush(e->name);
	if (!strcmp(e->mode, "nap")) netman_ent_btpan_server(0, e->name);
	return 0;
}

int netman_ent_tether_show(const netman_ent *e, FILE *f)
{
	netman_ent_tether_dev d[16];
	char pf[300], b[32];
	int n = netman_ent_tether_detect(d, 16), i;
	fprintf(f, "tether %s mode %s\n", e->name, e->mode[0] ? e->mode : "share");
	for (i = 0; i < n; i++) fprintf(f, "  detected %s (%s, driver %s)\n", d[i].ifn, d[i].kind, d[i].driver);
	if (!n) fprintf(f, "  no tethering device detected\n");
	th_path(pf, sizeof pf, e->name, "pid");
	if (ent_slurp(pf, b, sizeof b) > 0) fprintf(f, "  dnsmasq pid %d %s\n", atoi(b), kill(atoi(b), 0) == 0 ? "running" : "dead");
	return 0;
}

/* ---------------------------------------------------------------- Bluetooth PAN (bluez D-Bus) */
#ifdef NETMAN_ENT_DBUS
#include "dbus.h"
static int th_btpath(const char *mac, char *p, size_t n)
{
	unsigned m[6];
	if (!mac || sscanf(mac, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6 || strlen(mac) != 17) return ent_fail(-EINVAL, "bad Bluetooth address '%s'", mac ? mac : "");
	snprintf(p, n, "/org/bluez/hci0/dev_%02X_%02X_%02X_%02X_%02X_%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
	return 0;
}
int netman_ent_btpan_connect(const char *mac, const char *role, char *ifn, size_t n)
{
	char p[80];
	netman_dbus_Buf b = {0};
	netman_ent_tether_dev d[16];
	int fd, r, c, i;
	if (!role || (strcmp(role, "panu") && strcmp(role, "nap") && strcmp(role, "gn"))) return ent_fail(-EINVAL, "role must be panu, nap or gn");
	if ((r = th_btpath(mac, p, sizeof p)) < 0) return r;
	if ((fd = netman_dbus_system_bus()) < 0) return ent_fail(-ECONNREFUSED, "cannot reach the system bus");
	netman_dbus_put_str(&b, role);
	r = netman_dbus_simple_call(fd, "org.bluez", p, "org.bluez.Network1", "Connect", "s", &b, "PAN connect");
	free(b.b);
	close(fd);
	if (r) return ent_fail(-EIO, "bluez refused PAN connect to %s", mac);
	c = netman_ent_tether_detect(d, 16);
	if (ifn && n) ifn[0] = 0;
	for (i = 0; i < c; i++)
		if (!strcmp(d[i].kind, "bt") && ifn && n) ent_cpy(ifn, n, d[i].ifn);
	return 0;
}
int netman_ent_btpan_server(int on, const char *bridge)
{
	netman_dbus_Buf b = {0};
	int fd, r;
	if (!TH_NM(bridge)) return ent_fail(-EINVAL, "bad bridge name");
	if ((fd = netman_dbus_system_bus()) < 0) return ent_fail(-ECONNREFUSED, "cannot reach the system bus");
	if (on) {
		netman_dbus_put_str(&b, "nap");
		netman_dbus_put_str(&b, bridge);
		r = netman_dbus_simple_call(fd, "org.bluez", "/org/bluez/hci0", "org.bluez.NetworkServer1", "Register", "ss", &b, "NAP register");
	} else {
		netman_dbus_put_str(&b, "nap");
		r = netman_dbus_simple_call(fd, "org.bluez", "/org/bluez/hci0", "org.bluez.NetworkServer1", "Unregister", "s", &b, "NAP unregister");
	}
	free(b.b);
	close(fd);
	return r ? ent_fail(-EIO, "bluez NAP %s failed", on ? "register" : "unregister") : 0;
}
#else
int netman_ent_btpan_connect(const char *mac, const char *role, char *ifn, size_t n)
{
	(void)mac, (void)role;
	if (ifn && n) ifn[0] = 0;
	return ent_fail(-ENOSYS, "Bluetooth PAN needs a build with -DNETMAN_ENT_DBUS");
}
int netman_ent_btpan_server(int on, const char *bridge)
{
	(void)on, (void)bridge;
	return ent_fail(-ENOSYS, "Bluetooth PAN needs a build with -DNETMAN_ENT_DBUS");
}
#endif
