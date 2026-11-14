/* ssh.h - SSH connections as managed profiles: tunnels (-L -R -D), jump hosts, keepalive, ControlMaster, `ssh -w` layer-3 tunnel devices,
 * auto-reconnect, and a known_hosts you control. Real code in ssh.c. Switch: ssh.enabled (off by default). It runs the system `ssh`; it
 * does not implement the protocol.
 *
 * A profile is NAME.ssh in ssh.dir ("key = value", `forward` may repeat):
 *   host (required), port (22), user, key (identity file, used with IdentitiesOnly), jump (comma list [user@]host[:port], ssh -J),
 *   forward = L:LPORT:HOST:PORT | R:RPORT:HOST:PORT | D:PORT      (L and D listen on 127.0.0.1 only; R on the server's loopback)
 *   keepalive (ServerAliveInterval s, default 30)  keepalive_count (default 3)  connect_timeout (s, default 15)
 *   control = yes|no     ControlMaster socket in ssh.state_dir, so `ssh exec` / `ssh check` reuse the connection
 *   tun = LOCAL:REMOTE   `ssh -w` layer-3 device numbers (tun0 = 0:0); tun_addr = CIDR assigned to the local tunN once it exists.
 *                        Needs root (or a tun device you may open) and PermitTunnel yes on the server, which must configure its end itself.
 *   reconnect = yes|no, reconnect_delay (s, default 5; doubles up to 60 while the link dies within 10 s)   auto = yes|no (ssh apply)
 *   strict = yes | accept-new     host key policy. Host key checking can NOT be turned off here: "no", "off" and "ask" are refused, the
 *                                 known_hosts is ours (ssh.known_hosts, or `known_hosts =` in the profile) and always passed explicitly.
 *   hostkey                       (optional) expected fingerprint SHA256:...; `ssh trust` stores it
 *
 * Host keys: `ssh hostkey NAME` shows the fingerprints the server (and each jump host) presents; check them over another channel, then
 * `ssh trust NAME FINGERPRINT [HOST]` records the key ONLY if the server presents exactly that fingerprint. Nothing is trusted silently.
 * The ssh process is started with BatchMode=yes and ExitOnForwardFailure=yes, as `ssh -N -T`: a failed forward ends (and reconnects) it.
 */
#ifndef NETMAN_SSH_H
#define NETMAN_SSH_H
#include <stdio.h>
#include <stddef.h>

#define NETMAN_SSH_FWD 8
typedef struct {
	char name[32], host[128], user[64], key[200], jump[256], known_hosts[200], strict[12], tun_addr[64];
	int port, keepalive, keepalive_count, connect_timeout, control, tun_local, tun_remote /* -1 = no tun */, reconnect, reconnect_delay, autostart;
	char fwd[NETMAN_SSH_FWD][96];
	int nfwd;
} netman_ssh;

typedef struct {
	char dir[160], state[160], known[200], bin[160], keyscan[160], keygen[160];
} netman_ssh_cfg;

void netman_ssh_cfg_defaults(netman_ssh_cfg *c);
void netman_ssh_init(netman_ssh *p);
int  netman_ssh_parse(const char *text, netman_ssh *p, char *err, size_t en);
int  netman_ssh_format(const netman_ssh *p, char *buf, size_t n);
int  netman_ssh_check(const netman_ssh *p, char *err, size_t en);
/* the exact argv (NULL-terminated, strings in buf) `ssh up` runs; 0 or -1 */
int  netman_ssh_argv(const netman_ssh_cfg *c, const netman_ssh *p, char **argv, int max, char *buf, size_t bn);
int  netman_ssh_status(const netman_ssh_cfg *c, FILE *out);     /* rows  NAME STATE user@host:port detail ; the number of profiles */
int  netman_ssh_main(const netman_ssh_cfg *c, FILE *out, FILE *err, int argc, char **argv);    /* argv after "ssh"; exit status */
#endif
