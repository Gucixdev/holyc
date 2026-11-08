/* daemon.c - `netman daemon [--bus ADDRESS]`: the long-running D-Bus service.
 *
 * Claims org.freedesktop.NetworkManager on the system bus (never queues behind or steals it from a running NetworkManager) and serves the
 * NM D-Bus API of mods/nmapi.c until SIGTERM/SIGINT. --bus ADDRESS (or DBUS_SYSTEM_BUS_ADDRESS) selects another bus, e.g. a private
 * dbus-daemon for tests; the Wi-Fi backend then talks to the same bus. daemonless.c is the one-shot CLI path and is not involved.
 * Included into netman.c after mods/nmapi.c.
 */
#include <poll.h>
#include <signal.h>

static volatile sig_atomic_t nd_stop;
static void nd_on_signal(int s) { (void)s; nd_stop = 1; }

static int netman_daemon(int n, char **a)
{
	netman_dbus_Msg m;
	netman_dbus_Buf b = {0};
	const char *bus = getenv("DBUS_SYSTEM_BUS_ADDRESS");
	struct sigaction sa;
	int fd, refresh, tick_ms;
	uint32_t reply = 0;
	time_t next;

	for (int i = 0; i < n; i++) {
		if (!strcmp(a[i], "--bus") && i + 1 < n) bus = a[++i];
		else { fprintf(stderr, "usage: netman daemon [--bus ADDRESS]\n"); return 2; }
	}
	if (!nc_enabled("nmapi")) { fputs("netman: nmapi is disabled in config (netman config toggle nmapi.enabled)\n", stderr); return 3; }
	if (bus && *bus) setenv("DBUS_SYSTEM_BUS_ADDRESS", bus, 1);
	else bus = "unix:path=/run/dbus/system_bus_socket";
	if ((fd = netman_dbus_connect_bus(bus)) < 0 || netman_dbus_bus_hello(fd)) { fprintf(stderr, "netman daemon: cannot reach the bus %s\n", bus); return 1; }

	netman_dbus_put_str(&b, NMAPI_NAME);
	netman_dbus_put_u32(&b, 4);   /* DBUS_NAME_FLAG_DO_NOT_QUEUE: fail instead of waiting for a running NetworkManager to leave */
	if (netman_dbus_call(fd, &m, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName", "su", &b)) {
		fprintf(stderr, "netman daemon: RequestName %s failed (%s)\n", NMAPI_NAME, m.err);
		free(b.b);
		return 1;
	}
	free(b.b);
	if (m.blen >= 4) memcpy(&reply, m.body, 4);
	if (reply != 1) { fprintf(stderr, "netman daemon: %s is already owned (another NetworkManager?); not taking it over\n", NMAPI_NAME); close(fd); return 1; }

	memset(&sa, 0, sizeof sa);
	sa.sa_handler = nd_on_signal;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	refresh = netman_config_int(&nc_cfg, NC_NMAPI_REFRESH);
	nmapi_start(fd);
	fprintf(stderr, "netman daemon: serving %s on %s (refresh %ds)\n", NMAPI_NAME, bus, refresh);
	next = time(NULL) + refresh;
	while (!nd_stop) {
		struct pollfd p = { fd, POLLIN, 0 };
		time_t now = time(NULL);
		tick_ms = next > now ? (int)(next - now) * 1000 : 0;
		if (poll(&p, 1, tick_ms) > 0) {
			if (p.revents & (POLLHUP | POLLERR)) break;
			if (netman_dbus_read_msg(fd, &m, 5000)) break;
			nmapi_handle(fd, &m);
		}
		if (time(NULL) >= next) { nmapi_tick(fd, 0); next = time(NULL) + refresh; }
	}
	close(fd);
	return 0;
}

static int h_daemon(const nc_cmd *c, int n, char **a) { (void)c; return netman_daemon(n, a); }
