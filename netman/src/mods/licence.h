/* licence.h - enterprise licence/key store: licence files with expiry under /etc/netman/licences, `netman licence
 * status|renew`, renewal through a pluggable hook command, and WireGuard/VPN key rotation. Real .c: licence.c.
 *
 * Licence file NAME.lic (key = value):  name, holder, kind, expires = YYYY-MM-DD | never, key (opaque token).
 * It is valid through the whole of its expiry day (UTC). Renewal NEVER talks to a network itself: the configured hook
 * (a command, split on spaces, exec'd without a shell, given NAME CURRENT_PATH NEW_PATH) does that and must write the
 * new licence file at NEW_PATH. We then check it parses, names the same licence and expires later, and swap atomically,
 * keeping the old one as NAME.lic.prev.
 *
 * Key rotation: NAME.key holds a base64 X25519 private key (WireGuard format, 0600). rotate = generate a new key, write
 * NAME.key.new, hardlink the old one to NAME.key.prev, rename the new over NAME.key, then run the optional apply hook
 * (NAME PUBKEY) - on hook failure the old key is restored. The curve arithmetic is in licence.c: no `wg` binary needed.
 * Return 0/count or a negative errno; text in netman_ent_error() (enterprise.h).
 */
#ifndef NETMAN_LICENCE_H
#define NETMAN_LICENCE_H
#include <stdio.h>
#include <time.h>

typedef struct {
	char name[64], holder[64], kind[24];
	time_t expires; /* first second NOT valid (end of the expiry day); 0 = never */
	int days;	/* whole days left (negative once expired); 99999 = never */
	int state;	/* 0 valid, 1 expiring (inside warn window), 2 expired */
} netman_lic;

extern const char *netman_lic_dir;	/* "/etc/netman/licences" */
extern int netman_lic_warn_days;	/* 30 */
extern const char *netman_lic_renew_hook; /* "" = none */
extern const char *netman_lic_apply_hook; /* run after a key rotation; "" = none */
extern time_t (*netman_lic_now)(void);	/* tests override the clock */

int netman_lic_parse(const char *text, netman_lic *l);	/* 0 or -EINVAL */
int netman_lic_load(const char *name, netman_lic *l);
int netman_lic_list(netman_lic *out, int max);		/* count; sorted by name */
int netman_lic_status(FILE *f);				/* prints a table; returns the number expiring or expired */
int netman_lic_renew(const char *name);

int netman_key_generate(char priv_b64[45]);
int netman_key_pub(const char *priv_b64, char pub_b64[45]);
int netman_key_rotate(const char *name, char pub_b64[45]);	/* new key, old kept as .prev, returns the new public key */
int netman_key_show(const char *name, char pub_b64[45]);	/* public key of the current NAME.key */
#endif
