/* nmapi.h - constants of NetworkManager's D-Bus API (org.freedesktop.NetworkManager) as served by netman's daemon (nmapi.c).
 *
 * The compatibility target is the D-Bus API, not GObject: libnm, nm-applet, plasma-nm (NetworkManagerQt) and nmcli are all clients of it.
 * Values are the ones in NetworkManager's public headers (NMState, NMDeviceType, NMDeviceState, NMActiveConnectionState, NMConnectivityState).
 * Plain C, no libraries. The server itself (nmapi.c) is included into netman.c (it sits on dbus.h's static message code); this header is also
 * used by tests/nmapi_raw_test.c, which is a plain D-Bus client.
 */
#ifndef NETMAN_NMAPI_H
#define NETMAN_NMAPI_H

#define NMAPI_NAME      "org.freedesktop.NetworkManager"
#define NMAPI_ROOT      "/org/freedesktop"
#define NMAPI_PATH      "/org/freedesktop/NetworkManager"
#define NMAPI_SETTINGS  "/org/freedesktop/NetworkManager/Settings"
#define NMAPI_DNS       "/org/freedesktop/NetworkManager/DnsManager"
#define NMAPI_AGENTMGR  "/org/freedesktop/NetworkManager/AgentManager"

enum { NMAPI_STATE_UNKNOWN = 0, NMAPI_STATE_ASLEEP = 10, NMAPI_STATE_DISCONNECTED = 20, NMAPI_STATE_DISCONNECTING = 30, NMAPI_STATE_CONNECTING = 40,
       NMAPI_STATE_CONNECTED_LOCAL = 50, NMAPI_STATE_CONNECTED_SITE = 60, NMAPI_STATE_CONNECTED_GLOBAL = 70 };
enum { NMAPI_CONN_UNKNOWN = 0, NMAPI_CONN_NONE = 1, NMAPI_CONN_PORTAL = 2, NMAPI_CONN_LIMITED = 3, NMAPI_CONN_FULL = 4 };
enum { NMAPI_DEV_UNKNOWN = 0, NMAPI_DEV_ETHERNET = 1, NMAPI_DEV_WIFI = 2, NMAPI_DEV_BOND = 10, NMAPI_DEV_VLAN = 11, NMAPI_DEV_BRIDGE = 13, NMAPI_DEV_GENERIC = 14, NMAPI_DEV_WIREGUARD = 29 };
enum { NMAPI_DS_UNMANAGED = 10, NMAPI_DS_UNAVAILABLE = 20, NMAPI_DS_DISCONNECTED = 30, NMAPI_DS_PREPARE = 40, NMAPI_DS_ACTIVATED = 100, NMAPI_DS_DEACTIVATING = 110, NMAPI_DS_FAILED = 120 };
enum { NMAPI_AC_UNKNOWN = 0, NMAPI_AC_ACTIVATING = 1, NMAPI_AC_ACTIVATED = 2, NMAPI_AC_DEACTIVATING = 3, NMAPI_AC_DEACTIVATED = 4 };

#ifdef NETMAN_DBUS_IMPLEMENTATION_DONE   /* the server half needs dbus.h's types */
int nmapi_start(int fd);                              /* after Hello and RequestName: build the model, no signals yet */
int nmapi_handle(int fd, const netman_dbus_Msg *m);   /* one incoming message; 1 = it was a method call for us (answered), 0 = not ours */
void nmapi_tick(int fd, int scan);                    /* re-read links, wifi (scan = ask the backend for a fresh scan), profiles; emit the differences as signals */
#endif
#endif /* NETMAN_NMAPI_H */
