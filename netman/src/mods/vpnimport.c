/* vpnimport.c - foreign VPN profile import (see vpnimport.h). Pure text in, text out: nothing here touches the disk or the network. */
#define _GNU_SOURCE
#include "vpnimport.h"
#include "vpnmod.h"
#include "openvpn.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <stdio.h>

typedef struct { char *s; size_t n, cap; } ds;
static void dp(ds *d, const char *f, ...)
{
	va_list a; char b[2048]; va_start(a, f); int k = vsnprintf(b, sizeof b, f, a); va_end(a);
	if (k < 0) return;
	if ((size_t)k >= sizeof b) k = (int)sizeof b - 1;
	if (d->n + (size_t)k + 1 > d->cap) { d->cap = (d->n + (size_t)k + 1) * 2; d->s = realloc(d->s, d->cap); if (!d->s) return; }
	memcpy(d->s + d->n, b, (size_t)k); d->n += (size_t)k; d->s[d->n] = 0;
}
static char *trim(char *s) { while (isspace((unsigned char)*s)) s++; char *e = s + strlen(s); while (e > s && isspace((unsigned char)e[-1])) *--e = 0; return s; }
static void note(netman_vpnimp *o, const char *f, ...) { va_list a; size_t l = strlen(o->note); if (l) { snprintf(o->note + l, sizeof o->note - l, "; "); l = strlen(o->note); } va_start(a, f); vsnprintf(o->note + l, sizeof o->note - l, f, a); va_end(a); }

/* ---- INI keyfile (NetworkManager) --------------------------------------------------------------------------------- */
typedef struct { char sec[160], k[96], v[512]; } kvl;
typedef struct { kvl *a; int n, cap; } kf;
static int kf_parse(const char *t, kf *f)
{
	char sec[160] = ""; f->a = NULL; f->n = f->cap = 0;
	for (const char *p = t; *p; ) {
		const char *e = strchr(p, '\n'); size_t l = e ? (size_t)(e - p) : strlen(p);
		char b[800]; snprintf(b, sizeof b, "%.*s", (int)(l < sizeof b - 1 ? l : sizeof b - 1), p);
		char *s = trim(b);
		if (*s == '[' && s[strlen(s) - 1] == ']') { s[strlen(s) - 1] = 0; snprintf(sec, sizeof sec, "%s", s + 1); }
		else if (*s && *s != '#' && *s != ';' && strchr(s, '=')) {
			char *eq = strchr(s, '='); *eq = 0;
			if (f->n == f->cap) { f->cap = f->cap ? f->cap * 2 : 64; f->a = realloc(f->a, (size_t)f->cap * sizeof *f->a); if (!f->a) return -1; }
			kvl *x = &f->a[f->n++]; snprintf(x->sec, sizeof x->sec, "%s", sec); snprintf(x->k, sizeof x->k, "%s", trim(s)); snprintf(x->v, sizeof x->v, "%s", trim(eq + 1));
		}
		if (!e) break;
		p = e + 1;
	}
	return 0;
}
static const char *kg(const kf *f, const char *sec, const char *k) { for (int i = 0; i < f->n; i++) if (!strcmp(f->a[i].sec, sec) && !strcmp(f->a[i].k, k)) return f->a[i].v; return NULL; }
static int ky(const char *v) { return v && (!strcasecmp(v, "yes") || !strcasecmp(v, "true") || !strcmp(v, "1")); }
static void list_semis(ds *d, const char *v, const char *sep)   /* "a;b;c;" -> "a<sep>b<sep>c" */
{
	char b[512]; int first = 1; snprintf(b, sizeof b, "%s", v);
	for (char *sp = NULL, *t = strtok_r(b, ";", &sp); t; t = strtok_r(NULL, ";", &sp)) { t = trim(t); if (!*t) continue; dp(d, "%s%s", first ? "" : sep, t); first = 0; }
}

static int nm_wireguard(const kf *f, netman_vpnimp *o, char *err, size_t en)
{
	ds d = {0}; const char *pk = kg(f, "wireguard", "private-key");
	if (!pk) { snprintf(err, en, "NetworkManager wireguard connection without a private-key (it lives in the secret agent: add PrivateKey yourself)"); return -1; }
	dp(&d, "[Interface]\nPrivateKey = %s\n", pk);
	const char *v;
	if ((v = kg(f, "wireguard", "listen-port")) && atoi(v)) dp(&d, "ListenPort = %s\n", v);
	if ((v = kg(f, "wireguard", "fwmark")) && atoi(v)) dp(&d, "FwMark = %s\n", v);
	if ((v = kg(f, "wireguard", "mtu")) && atoi(v)) dp(&d, "MTU = %s\n", v);
	const char *fam[] = { "ipv4", "ipv6" }; ds addr = {0}, dns = {0};
	for (int q = 0; q < 2; q++) {
		for (int i = 1; i < 20; i++) {
			char k[16]; snprintf(k, sizeof k, "address%d", i); if (!(v = kg(f, fam[q], k))) break;
			char a[128]; snprintf(a, sizeof a, "%.127s", v); a[strcspn(a, ",")] = 0;
			dp(&addr, "%s%s", addr.n ? ", " : "", trim(a));
		}
		if ((v = kg(f, fam[q], "dns"))) { ds t = {0}; list_semis(&t, v, ", "); if (t.s && *t.s) dp(&dns, "%s%s", dns.n ? ", " : "", t.s); free(t.s); }
	}
	if (addr.n) dp(&d, "Address = %s\n", addr.s);
	if (dns.n) dp(&d, "DNS = %s\n", dns.s);
	free(addr.s); free(dns.s);
	int peers = 0;
	for (int i = 0; i < f->n; i++) {
		const char *sn = f->a[i].sec;
		if (strncmp(sn, "wireguard-peer.", 15) || (i && !strcmp(f->a[i - 1].sec, sn))) continue;
		peers++;
		dp(&d, "\n[Peer]\nPublicKey = %s\n", sn + 15);
		if ((v = kg(f, sn, "preshared-key"))) dp(&d, "PresharedKey = %s\n", v);
		if ((v = kg(f, sn, "endpoint"))) dp(&d, "Endpoint = %s\n", v);
		if ((v = kg(f, sn, "allowed-ips"))) { dp(&d, "AllowedIPs = "); list_semis(&d, v, ", "); dp(&d, "\n"); }
		if ((v = kg(f, sn, "persistent-keepalive")) && atoi(v)) dp(&d, "PersistentKeepalive = %s\n", v);
	}
	if (!peers) { free(d.s); snprintf(err, en, "no [wireguard-peer.KEY] section"); return -1; }
	o->text = d.s; o->len = d.n; strcpy(o->ext, ".conf"); strcpy(o->type, "wireguard");
	return 0;
}

static const char *vd(const kf *f, const char *k) { return kg(f, "vpn", k); }
static const char *vs(const kf *f, const char *k) { return kg(f, "vpn-secrets", k); }
static void unconverted(netman_vpnimp *o, const kf *f, const char *const *known)
{
	char b[300] = "";
	for (int i = 0; i < f->n; i++) {
		if (strcmp(f->a[i].sec, "vpn") || !strcmp(f->a[i].k, "service-type")) continue;
		int ok = 0; for (int j = 0; known[j]; j++) if (!strcmp(f->a[i].k, known[j])) ok = 1;
		if (!ok && strlen(b) + strlen(f->a[i].k) + 3 < sizeof b && !strstr(f->a[i].k, "-flags")) snprintf(b + strlen(b), sizeof b - strlen(b), "%s%s", b[0] ? ", " : "", f->a[i].k);
	}
	if (b[0]) note(o, "not converted: %s", b);
}
static int nm_openvpn(const kf *f, netman_vpnimp *o, char *err, size_t en)
{
	static const char *const known[] = { "remote", "port", "proto-tcp", "dev", "connection-type", "ca", "cert", "key", "username", "remote-cert-tls", "cipher", "auth", NULL };
	ds d = {0}; const char *v, *rem = vd(f, "remote");
	if (!rem) { snprintf(err, en, "no remote in the [vpn] section"); return -1; }
	char host[200], port[16] = ""; snprintf(host, sizeof host, "%.199s", rem);
	char *col = strrchr(host, ':'); if (col && !strchr(col + 1, ':')) { *col = 0; snprintf(port, sizeof port, "%s", col + 1); }
	if (!port[0] && (v = vd(f, "port"))) snprintf(port, sizeof port, "%.15s", v);
	dp(&d, "client\ndev %s\nproto %s\nremote %s %s\nnobind\npersist-key\npersist-tun\n", (v = vd(f, "dev")) && !strncmp(v, "tap", 3) ? "tap" : "tun", ky(vd(f, "proto-tcp")) ? "tcp" : "udp", host, port[0] ? port : "1194");
	if ((v = vd(f, "remote-cert-tls")) && *v) dp(&d, "remote-cert-tls %s\n", v); else dp(&d, "remote-cert-tls server\n");
	if ((v = vd(f, "cipher")) && *v) dp(&d, "cipher %s\n", v);
	if ((v = vd(f, "auth")) && *v) dp(&d, "auth %s\n", v);
	if ((v = vd(f, "ca"))) dp(&d, "ca %s\n", v);
	if ((v = vd(f, "cert"))) dp(&d, "cert %s\n", v);
	if ((v = vd(f, "key"))) dp(&d, "key %s\n", v);
	if ((v = vd(f, "connection-type")) && strstr(v, "password")) dp(&d, "auth-user-pass\n");
	if (vs(f, "password")) note(o, "the password is in NetworkManager's secret store: not imported (put it in NAME.auth, two lines: user, password)");
	unconverted(o, f, known);
	o->text = d.s; o->len = d.n; strcpy(o->ext, ".ovpn"); strcpy(o->type, "openvpn");
	return 0;
}
static int nm_native_head(ds *d, const char *type, const char *server) { dp(d, "# imported from a NetworkManager connection\nType = %s\nServer = %s\n", type, server); return 0; }
static int nm_vpn(const kf *f, netman_vpnimp *o, char *err, size_t en)
{
	const char *svc = vd(f, "service-type"), *v; ds d = {0};
	if (!svc) { snprintf(err, en, "no service-type in the [vpn] section"); return -1; }
	const char *s = strrchr(svc, '.'); s = s ? s + 1 : svc;
	if (!strcmp(s, "pptp")) { snprintf(err, en, "PPTP is not supported and will not be: its MS-CHAPv2/MPPE encryption is broken (use WireGuard, OpenVPN, IPsec/IKEv2 or OpenConnect)"); return -1; }
	if (!strcmp(s, "openvpn")) return nm_openvpn(f, o, err, en);
	if (!strcmp(s, "strongswan")) {
		static const char *const known[] = { "address", "user", "method", "certificate", "usercert", "userkey", "virtual", "encap", "ipcomp", "proposal", "ike", "esp", "type", NULL };
		const char *addr = vd(f, "address"), *m = vd(f, "method");
		if (!addr) { snprintf(err, en, "no address in the [vpn] section"); return -1; }
		nm_native_head(&d, "ipsec", addr);
		dp(&d, "Auth = %s\n", !m || !strcmp(m, "eap") ? "eap" : !strcmp(m, "key") ? "cert" : !strcmp(m, "psk") ? "psk" : "eap");
		if ((v = vd(f, "user"))) dp(&d, "Username = %s\n", v);
		if ((v = vs(f, "password"))) dp(&d, "Password = %s\n", v); else if (!m || !strcmp(m, "eap")) note(o, "the password is in NetworkManager's secret store: not imported (add Password = ...)");
		if ((v = vd(f, "certificate"))) dp(&d, "CA = %s\n", v);
		if ((v = vd(f, "usercert"))) dp(&d, "Cert = %s\n", v);
		if ((v = vd(f, "userkey"))) dp(&d, "Key = %s\n", v);
		if (vd(f, "virtual")) dp(&d, "VirtualIP = %s\n", ky(vd(f, "virtual")) ? "yes" : "no");
		if (m && strcmp(m, "eap") && strcmp(m, "key") && strcmp(m, "psk")) note(o, "method %s is not supported, set to eap", m);
		if (ky(vd(f, "proposal"))) { if ((v = vd(f, "ike"))) dp(&d, "Proposals = %s\n", v); if ((v = vd(f, "esp"))) dp(&d, "EspProposals = %s\n", v); }
		unconverted(o, f, known);
		o->text = d.s; o->len = d.n; strcpy(o->ext, ".vpn"); strcpy(o->type, "ipsec"); return 0;
	}
	if (!strcmp(s, "l2tp")) {
		static const char *const known[] = { "gateway", "user", "ipsec-enabled", "ipsec-psk", "ipsec-gateway-id", "mtu", "mru", "refuse-pap", "refuse-chap", "refuse-eap", "refuse-mschap", "require-mppe", "user-auth-type", NULL };
		const char *g = vd(f, "gateway"); if (!g) { snprintf(err, en, "no gateway in the [vpn] section"); return -1; }
		nm_native_head(&d, "l2tp", g);
		if ((v = vd(f, "user"))) dp(&d, "Username = %s\n", v);
		if ((v = vs(f, "password"))) dp(&d, "Password = %s\n", v); else note(o, "the password is in NetworkManager's secret store: not imported (add Password = ...)");
		if (ky(vd(f, "ipsec-enabled"))) {
			if ((v = vs(f, "ipsec-psk")) || (v = vd(f, "ipsec-psk"))) dp(&d, "PSK = %s\n", v); else note(o, "IPsec is on but the PSK is in the secret store: add PSK = ... (without it this profile is plain, unencrypted L2TP)");
			if ((v = vd(f, "ipsec-gateway-id"))) dp(&d, "RemoteId = %s\n", v);
		} else note(o, "IPsec is off in the original: plain L2TP carries no encryption");
		if ((v = vd(f, "mtu")) && atoi(v) >= 576) dp(&d, "MTU = %s\n", v);
		unconverted(o, f, known);
		o->text = d.s; o->len = d.n; strcpy(o->ext, ".vpn"); strcpy(o->type, "l2tp"); return 0;
	}
	if (!strcmp(s, "openconnect")) {
		static const char *const known[] = { "gateway", "protocol", "username", "usergroup", "cacert", "usercert", "userkey", "authtype", "pem_passphrase_fsid", NULL };
		const char *g = vd(f, "gateway"); if (!g) { snprintf(err, en, "no gateway in the [vpn] section"); return -1; }
		nm_native_head(&d, "openconnect", g);
		dp(&d, "Protocol = %s\n", (v = vd(f, "protocol")) && *v ? v : "anyconnect");
		if ((v = vd(f, "username"))) dp(&d, "Username = %s\n", v);
		if ((v = vs(f, "password")) ) dp(&d, "Password = %s\n", v); else if (vd(f, "username")) note(o, "the password is in NetworkManager's secret store: not imported (add Password = ...)");
		if ((v = vd(f, "usergroup"))) dp(&d, "AuthGroup = %s\n", v);
		if ((v = vd(f, "cacert"))) dp(&d, "CA = %s\n", v);
		if ((v = vd(f, "usercert"))) dp(&d, "Cert = %s\n", v);
		if ((v = vd(f, "userkey"))) dp(&d, "Key = %s\n", v);
		unconverted(o, f, known);
		o->text = d.s; o->len = d.n; strcpy(o->ext, ".vpn"); strcpy(o->type, "openconnect"); return 0;
	}
	if (!strcmp(s, "sstp")) {
		const char *g = vd(f, "gateway"); if (!g) { snprintf(err, en, "no gateway in the [vpn] section"); return -1; }
		nm_native_head(&d, "sstp", g); if ((v = vd(f, "user"))) dp(&d, "Username = %s\n", v);
		note(o, "SSTP is recognised but not supported by netman (no maintained client)");
		o->text = d.s; o->len = d.n; strcpy(o->ext, ".vpn"); strcpy(o->type, "sstp"); return 0;
	}
	snprintf(err, en, "NetworkManager VPN plugin '%s' is not supported (wanted: openvpn, strongswan, l2tp, openconnect)", s);
	return -1;
}
static int nm_convert(const char *t, netman_vpnimp *o, char *err, size_t en)
{
	kf f; int rc; const char *ty;
	if (kf_parse(t, &f)) { snprintf(err, en, "out of memory"); return -1; }
	ty = kg(&f, "connection", "type");
	if (!ty) { snprintf(err, en, "keyfile has no type= in [connection]"); rc = -1; }
	else if (!strcmp(ty, "wireguard")) rc = nm_wireguard(&f, o, err, en);
	else if (!strcmp(ty, "vpn")) rc = nm_vpn(&f, o, err, en);
	else { snprintf(err, en, "NetworkManager connection type '%s' is not a VPN", ty); rc = -1; }
	free(f.a); return rc;
}

/* ---- strongSwan: swanctl.conf (nested blocks) and legacy ipsec.conf ------------------------------------------------ */
typedef struct { char path[200], v[400]; } sv;
typedef struct { sv *a; int n, cap; } svl;
static void sv_add(svl *l, const char *path, const char *v)
{
	if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 64; l->a = realloc(l->a, (size_t)l->cap * sizeof *l->a); if (!l->a) { l->n = l->cap = 0; return; } }
	snprintf(l->a[l->n].path, sizeof l->a[l->n].path, "%s", path); snprintf(l->a[l->n].v, sizeof l->a[l->n].v, "%s", v); l->n++;
}
static int swan_parse(const char *t, svl *l)
{
	char stack[16][64]; int depth = 0;
	for (const char *p = t; *p; ) {
		const char *e = strchr(p, '\n'); size_t n = e ? (size_t)(e - p) : strlen(p);
		char b[600]; snprintf(b, sizeof b, "%.*s", (int)(n < sizeof b - 1 ? n : sizeof b - 1), p);
		char *h = strchr(b, '#'); if (h) *h = 0;
		char *s = trim(b);
		if (*s) {
			size_t sl = strlen(s);
			if (s[sl - 1] == '{') { s[sl - 1] = 0; s = trim(s); char *c = strchr(s, '='); if (c) *c = 0; if (depth >= 15) return -1; snprintf(stack[depth++], 64, "%s", trim(s)); }
			else if (!strcmp(s, "}")) { if (depth) depth--; }
			else if (strchr(s, '=')) {
				char *eq = strchr(s, '='); *eq = 0; char path[200] = "";
				for (int i = 0; i < depth; i++) snprintf(path + strlen(path), sizeof path - strlen(path), "%.63s.", stack[i]);
				snprintf(path + strlen(path), sizeof path - strlen(path), "%s", trim(s));
				char *v = trim(eq + 1); size_t vl = strlen(v); if (vl >= 2 && v[0] == '"' && v[vl - 1] == '"') { v[vl - 1] = 0; v++; }
				sv_add(l, path, v);
			}
		}
		if (!e) break;
		p = e + 1;
	}
	return 0;
}
static const char *sg(const svl *l, const char *prefix, const char *suffix)   /* first path starting with prefix and ending with suffix */
{
	size_t pl = strlen(prefix), sl = strlen(suffix);
	for (int i = 0; i < l->n; i++) { size_t k = strlen(l->a[i].path); if (k >= pl + sl && !strncmp(l->a[i].path, prefix, pl) && !strcmp(l->a[i].path + k - sl, suffix)) return l->a[i].v; }
	return NULL;
}
static void unhex(const char *v, char *out, size_t n)
{
	if (v[0] == '0' && (v[1] == 'x' || v[1] == 'X') && strlen(v) > 2 && strlen(v) % 2 == 0) {
		size_t o = 0; for (const char *q = v + 2; q[0] && q[1] && o + 1 < n; q += 2) { unsigned x; if (sscanf(q, "%2x", &x) != 1) { snprintf(out, n, "%s", v); return; } out[o++] = (char)x; } out[o] = 0;
	} else snprintf(out, n, "%s", v);
}
static const char *absp(const char *dir, const char *f, char *buf, size_t n) { if (f[0] == '/') snprintf(buf, n, "%s", f); else snprintf(buf, n, "/etc/swanctl/%s/%s", dir, f); return buf; }
static int swanctl_convert(const char *t, netman_vpnimp *o, char *err, size_t en)
{
	svl l = {0}; ds d = {0}; char conn[64] = "", pre[100], b[600], sec[400], idv[160];
	if (swan_parse(t, &l)) { snprintf(err, en, "swanctl.conf nesting too deep"); free(l.a); return -1; }
	for (int i = 0; i < l.n; i++) if (!strncmp(l.a[i].path, "connections.", 12)) { snprintf(conn, sizeof conn, "%s", l.a[i].path + 12); conn[strcspn(conn, ".")] = 0; break; }
	if (!conn[0]) { snprintf(err, en, "no connection in the strongSwan configuration"); free(l.a); return -1; }
	snprintf(pre, sizeof pre, "connections.%s.", conn);
	char q[260]; const char *v, *auth, *rid, *eapid, *lid;
	snprintf(q, sizeof q, "%sremote_addrs", pre); v = sg(&l, q, "");
	if (!v) { snprintf(err, en, "connection %s has no remote_addrs", conn); free(l.a); return -1; }
	snprintf(b, sizeof b, "%s", v); b[strcspn(b, ", ")] = 0;
	dp(&d, "# imported from a strongSwan connection '%s'\nType = ipsec\nServer = %s\n", conn, b);
	snprintf(q, sizeof q, "%slocal", pre); auth = sg(&l, q, ".auth"); if (!auth) { snprintf(q, sizeof q, "%slocal", pre); auth = sg(&l, q, "auth"); }
	snprintf(q, sizeof q, "%sremote.id", pre); rid = sg(&l, q, "");
	snprintf(q, sizeof q, "%slocal", pre); eapid = sg(&l, q, ".eap_id"); lid = sg(&l, q, ".id");
	const char *mode = !auth || !strncmp(auth, "eap", 3) ? "eap" : !strcmp(auth, "psk") ? "psk" : "cert";
	dp(&d, "Auth = %s\n", mode);
	if (rid) dp(&d, "RemoteId = %s\n", rid);
	if (!strcmp(mode, "eap")) {
		const char *u = eapid ? eapid : lid; if (u) dp(&d, "Username = %s\n", u);
		for (int i = 0; i < l.n; i++) if (!strncmp(l.a[i].path, "secrets.eap", 11) && strlen(l.a[i].path) > 7 && !strcmp(l.a[i].path + strlen(l.a[i].path) - 7, ".secret")) { unhex(l.a[i].v, sec, sizeof sec); dp(&d, "Password = %s\n", sec); explicit_bzero(sec, sizeof sec); break; }
		if (!strstr(d.s, "Password =")) note(o, "no eap secret in the file: add Password = ...");
	} else if (!strcmp(mode, "psk")) {
		if (lid) dp(&d, "LocalId = %s\n", lid);
		for (int i = 0; i < l.n; i++) if (!strncmp(l.a[i].path, "secrets.ike", 11) && !strcmp(l.a[i].path + strlen(l.a[i].path) - 7, ".secret")) { unhex(l.a[i].v, sec, sizeof sec); dp(&d, "PSK = %s\n", sec); explicit_bzero(sec, sizeof sec); break; }
		if (!strstr(d.s, "PSK =")) note(o, "no ike secret in the file: add PSK = ...");
	} else {
		snprintf(q, sizeof q, "%slocal", pre); if ((v = sg(&l, q, ".certs"))) dp(&d, "Cert = %s\n", absp("x509", v, b, sizeof b));
		if (lid) dp(&d, "LocalId = %s\n", lid);
		for (int i = 0; i < l.n; i++) if (!strncmp(l.a[i].path, "secrets.private", 15) && !strcmp(l.a[i].path + strlen(l.a[i].path) - 5, ".file")) { dp(&d, "Key = %s\n", absp("private", l.a[i].v, b, sizeof b)); break; }
		snprintf(q, sizeof q, "%sremote", pre); if ((v = sg(&l, q, ".cacerts"))) { char c1[200]; snprintf(c1, sizeof c1, "%s", v); c1[strcspn(c1, ", ")] = 0; dp(&d, "CA = %s\n", absp("x509ca", c1, b, sizeof b)); }
	}
	snprintf(q, sizeof q, "%svips", pre); dp(&d, "VirtualIP = %s\n", sg(&l, q, "") ? "yes" : "no"); (void)idv;
	snprintf(q, sizeof q, "%sversion", pre); if ((v = sg(&l, q, "")) && (atoi(v) == 1 || atoi(v) == 2)) dp(&d, "Version = %d\n", atoi(v));
	snprintf(q, sizeof q, "%sproposals", pre); if ((v = sg(&l, q, ""))) dp(&d, "Proposals = %s\n", v);
	snprintf(q, sizeof q, "%schildren.", pre); if ((v = sg(&l, q, ".remote_ts")) && strcmp(v, "dynamic")) { if (!strstr(v, "0.0.0.0/0")) dp(&d, "Routes = %s\n", v); }
	snprintf(q, sizeof q, "%schildren.", pre); if ((v = sg(&l, q, ".esp_proposals"))) dp(&d, "EspProposals = %s\n", v);
	free(l.a);
	o->text = d.s; o->len = d.n; strcpy(o->ext, ".vpn"); strcpy(o->type, "ipsec");
	return 0;
}
static int ipsecconf_convert(const char *t, netman_vpnimp *o, char *err, size_t en)
{
	char conn[64] = "", right[200] = "", rightid[200] = "", leftid[200] = "", leftauth[40] = "", authby[40] = "", rsub[200] = "", lcert[200] = "", eapid[200] = "", ike[200] = "", esp[200] = "", kex[20] = "";
	int in = 0, have = 0;
	for (const char *p = t; *p; ) {
		const char *e = strchr(p, '\n'); size_t n = e ? (size_t)(e - p) : strlen(p);
		char b[500]; snprintf(b, sizeof b, "%.*s", (int)(n < sizeof b - 1 ? n : sizeof b - 1), p);
		char *h = strchr(b, '#'); if (h) *h = 0;
		char *s = trim(b);
		if (!strncmp(s, "conn ", 5) || !strncmp(s, "config ", 7)) {
			in = 0;
			if (!strncmp(s, "conn ", 5) && !have && s[5] != '%') { in = 1; have = 1; snprintf(conn, sizeof conn, "%s", trim(s + 5)); }
		} else if (in && strchr(s, '=')) {
			char *eq = strchr(s, '='); *eq = 0; char *k = trim(s), *v = trim(eq + 1);
#define K(name, var) if (!strcmp(k, name)) snprintf(var, sizeof var, "%s", v)
			K("right", right); K("rightid", rightid); K("leftid", leftid); K("leftauth", leftauth); K("authby", authby); K("rightsubnet", rsub);
			K("leftcert", lcert); K("eap_identity", eapid); K("ike", ike); K("esp", esp); K("keyexchange", kex);
#undef K
		}
		if (!e) break;
		p = e + 1;
	}
	if (!have) { snprintf(err, en, "no 'conn NAME' section in the ipsec.conf"); return -1; }
	if (!right[0] || !strcmp(right, "%any")) { snprintf(err, en, "conn %s has no right= (the server address)", conn); return -1; }
	ds d = {0}; const char *mode = !strncmp(leftauth, "eap", 3) || eapid[0] ? "eap" : !strcmp(authby, "secret") || !strcmp(authby, "psk") ? "psk" : "cert";
	dp(&d, "# imported from an ipsec.conf connection '%s'\nType = ipsec\nServer = %s\nAuth = %s\n", conn, right, mode);
	if (rightid[0]) dp(&d, "RemoteId = %s\n", rightid[0] == '@' ? rightid + 1 : rightid);
	if (!strcmp(mode, "eap")) { const char *u = eapid[0] ? eapid : leftid; if (u[0]) dp(&d, "Username = %s\n", u); note(o, "passwords live in ipsec.secrets: not imported (add Password = ...)"); }
	else if (!strcmp(mode, "psk")) { if (leftid[0]) dp(&d, "LocalId = %s\n", leftid[0] == '@' ? leftid + 1 : leftid); note(o, "the PSK lives in ipsec.secrets: not imported (add PSK = ...)"); }
	else { if (lcert[0]) { char a[300]; dp(&d, "Cert = %s\n", absp("x509", lcert, a, sizeof a)); } note(o, "Key = (private key path) is not in ipsec.conf: add it"); }
	if (rsub[0] && !strstr(rsub, "0.0.0.0/0")) { dp(&d, "Routes = "); list_semis(&d, rsub, ", "); for (char *c = d.s; *c; c++) if (*c == ',' && c[1] != ' ') {} dp(&d, "\n"); }
	if (!strcmp(kex, "ikev1")) dp(&d, "Version = 1\n");
	if (ike[0]) dp(&d, "Proposals = %.*s\n", (int)strcspn(ike, "!"), ike);
	if (esp[0]) dp(&d, "EspProposals = %.*s\n", (int)strcspn(esp, "!"), esp);
	o->text = d.s; o->len = d.n; strcpy(o->ext, ".vpn"); strcpy(o->type, "ipsec");
	return 0;
}

/* ---- detection ----------------------------------------------------------------------------------------------------- */
static int line_has(const char *t, const char *start)   /* a line that begins (after blanks) with START */
{
	size_t l = strlen(start);
	for (const char *p = t; *p; ) { const char *q = p; while (*q == ' ' || *q == '\t') q++; if (!strncasecmp(q, start, l)) return 1; p = strchr(p, '\n'); if (!p) break; p++; }
	return 0;
}
int netman_vpnimp_convert(const char *text, size_t len, netman_vpnimp *o, char *err, size_t en)
{
	char ty[24]; memset(o, 0, sizeof *o); (void)len;
	if (line_has(text, "[interface]")) { o->text = strdup(text); o->len = strlen(text); strcpy(o->ext, ".conf"); strcpy(o->type, "wireguard"); return o->text ? 0 : -1; }
	if (line_has(text, "[connection]") && (line_has(text, "type=") || line_has(text, "type ="))) return nm_convert(text, o, err, en);
	if (line_has(text, "connections") && strchr(text, '{')) return swanctl_convert(text, o, err, en);
	if (line_has(text, "conn ") && (line_has(text, "right") || line_has(text, "left"))) return ipsecconf_convert(text, o, err, en);
	if (netman_vm_native(text, ty, sizeof ty)) {
		if (!strcmp(ty, "pptp")) { snprintf(err, en, "PPTP is not supported and will not be: its encryption is broken"); return -1; }
		const netman_vpnmod *m = netman_vpnmod_by_type(ty);
		if (!m || !m->detect(text)) { snprintf(err, en, "unknown Type = %s (ipsec, l2tp, openconnect, sstp)", ty); return -1; }
		o->text = strdup(text); o->len = strlen(text); strcpy(o->ext, ".vpn"); snprintf(o->type, sizeof o->type, "%.15s", ty); return o->text ? 0 : -1;
	}
	if (netman_openvpn_detect(text)) { o->text = strdup(text); o->len = strlen(text); strcpy(o->ext, ".ovpn"); strcpy(o->type, "openvpn"); return o->text ? 0 : -1; }
	snprintf(err, en, "cannot tell what this is (WireGuard [Interface], OpenVPN remote/client lines, NetworkManager .nmconnection, strongSwan swanctl.conf / ipsec.conf, or a netman Type = ... profile)");
	return -1;
}
void netman_vpnimp_free(netman_vpnimp *o) { if (o->text) { explicit_bzero(o->text, o->len); free(o->text); } o->text = NULL; o->len = 0; }
