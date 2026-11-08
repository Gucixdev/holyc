/* qlogind - org.freedesktop.login1 service (see docs/login1-surface.md for the exact surface).
 * Build: cc -std=gnu11 -O2 -Isrc/mods src/qlogind.c -o bin/qlogind        (see ./run)
 * Env: DBUS_SYSTEM_BUS_ADDRESS (REQUIRED: qlogind refuses to touch the default system bus), QLOGIND_RUN, QLOGIND_CONF, QLOGIND_VTFILE,
 *      QLOGIND_DEVDIR, QLOGIND_NAME. Power actions are a dry run unless real_power=1 is set in the config.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define QDBUS_IMPLEMENTATION
#include "mods/qdbus.h"
#define LOGIN1_IMPLEMENTATION
#include "mods/login1.h"

static volatile int stop;
static void on_sig(int s) { (void)s; stop = 1; }

int main(int argc, char **argv)
{
	qd_conn c;
	qd_msg r;
	qd_buf b = {0};
	const char *addr = getenv("DBUS_SYSTEM_BUS_ADDRESS");
	struct sigaction sa = { .sa_handler = on_sig };
	(void)argv;
	if (argc > 1) { fputs("usage: qlogind   (no arguments; configuration via qlogind.conf and the environment, see qlogind.8)\n", stderr); return 2; }
	if (!addr || !*addr) {
		fputs("qlogind: DBUS_SYSTEM_BUS_ADDRESS is not set; this build refuses to attach to the default system bus (a login service is already there)\n", stderr);
		return 2;
	}
	sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
	if (l1_init()) return 1;
	if (qd_open(&c, addr)) { fprintf(stderr, "qlogind: cannot connect to %s\n", addr); return 1; }
	qd_put_str(&b, l1_bus_name()); qd_put_u32(&b, 4);   /* DBUS_NAME_FLAG_DO_NOT_QUEUE: never replaces an owner */
	if (qd_call(&c, &r, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName", "su", &b, NULL, 0, 3000) != 0) {
		fprintf(stderr, "qlogind: RequestName(%s) failed\n", l1_bus_name());
		return 1;
	}
	{
		qd_rd rd = { r.body, 0, r.blen, 0 };
		if (qd_r_u32(&rd) != 1) { fprintf(stderr, "qlogind: %s is already owned by someone else; not replacing it\n", l1_bus_name()); return 1; }
	}
	qd_msg_free(&r); free(b.b);
	fprintf(stderr, "qlogind: serving %s as %s\n", l1_bus_name(), c.name);
	int rc = l1_run(&c, &stop) < 0 ? 1 : 0;
	qd_close(&c);
	return rc;
}
