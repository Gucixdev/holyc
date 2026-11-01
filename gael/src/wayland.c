/* wayland.c - a window on Wayland, as a client of the compositor through libwayland-client, which gael loads at run
 * time with dlopen like wgpu: so gael builds and runs without it. A window is a wl_surface with the xdg-shell roles
 * (xdg_surface, xdg_toplevel) on it; wgpu draws into it (gfx.c). Its events come in between frames: the size the
 * compositor gives it, the close button, the keys (by their place on the keyboard, evdev's codes, so WASD is WASD on
 * any layout) and the mouse.
 *
 * The xdg-shell tables below are what wayland-scanner 1.22 writes from xdg-shell.xml (wayland-protocols, stable),
 * which carries this notice:
 *
 *   Copyright (c) 2008-2013 Kristian Hogsberg, (c) 2013 Rafael Antognolli, (c) 2013 Jasper St. Pierre, (c) 2010-2013
 *   Intel Corporation, (c) 2015-2017 Samsung Electronics Co., Ltd, (c) 2015-2017 Red Hat Inc.
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
 *   documentation files (the "Software"), to deal in the Software without restriction, including without limitation
 *   the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and
 *   to permit persons to whom the Software is furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice (including the next paragraph) shall be included in all
 *   copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
 *   THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 *   CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 *   DEALINGS IN THE SOFTWARE. */

#include <poll.h>
#include <unistd.h>

typedef struct WlInterface WlInterface;
typedef struct {
	const char *name, *signature;
	const WlInterface **types;
} WlMessage;
struct WlInterface { /* struct wl_interface */
	const char *name;
	int version, method_count;
	const WlMessage *methods;
	int event_count;
	const WlMessage *events;
};

/* the types of the xdg-shell messages' arguments; the core ones are libwayland's, filled in once it is loaded */
static const WlInterface *g_xdg_types[26];
static const WlMessage g_xdg_wm_base_requests[] = {
	{ "destroy", "", g_xdg_types + 0 }, { "create_positioner", "n", g_xdg_types + 4 },
	{ "get_xdg_surface", "no", g_xdg_types + 5 }, { "pong", "u", g_xdg_types + 0 }
};
static const WlMessage g_xdg_wm_base_events[] = { { "ping", "u", g_xdg_types + 0 } };
static const WlMessage g_xdg_positioner_requests[] = {
	{ "destroy", "", g_xdg_types + 0 }, { "set_size", "ii", g_xdg_types + 0 }, { "set_anchor_rect", "iiii", g_xdg_types + 0 },
	{ "set_anchor", "u", g_xdg_types + 0 }, { "set_gravity", "u", g_xdg_types + 0 },
	{ "set_constraint_adjustment", "u", g_xdg_types + 0 }, { "set_offset", "ii", g_xdg_types + 0 },
	{ "set_reactive", "3", g_xdg_types + 0 }, { "set_parent_size", "3ii", g_xdg_types + 0 },
	{ "set_parent_configure", "3u", g_xdg_types + 0 }
};
static const WlMessage g_xdg_surface_requests[] = {
	{ "destroy", "", g_xdg_types + 0 }, { "get_toplevel", "n", g_xdg_types + 7 }, { "get_popup", "n?oo", g_xdg_types + 8 },
	{ "set_window_geometry", "iiii", g_xdg_types + 0 }, { "ack_configure", "u", g_xdg_types + 0 }
};
static const WlMessage g_xdg_surface_events[] = { { "configure", "u", g_xdg_types + 0 } };
static const WlMessage g_xdg_toplevel_requests[] = {
	{ "destroy", "", g_xdg_types + 0 }, { "set_parent", "?o", g_xdg_types + 11 }, { "set_title", "s", g_xdg_types + 0 },
	{ "set_app_id", "s", g_xdg_types + 0 }, { "show_window_menu", "ouii", g_xdg_types + 12 }, { "move", "ou", g_xdg_types + 16 },
	{ "resize", "ouu", g_xdg_types + 18 }, { "set_max_size", "ii", g_xdg_types + 0 }, { "set_min_size", "ii", g_xdg_types + 0 },
	{ "set_maximized", "", g_xdg_types + 0 }, { "unset_maximized", "", g_xdg_types + 0 },
	{ "set_fullscreen", "?o", g_xdg_types + 21 }, { "unset_fullscreen", "", g_xdg_types + 0 },
	{ "set_minimized", "", g_xdg_types + 0 }
};
static const WlMessage g_xdg_toplevel_events[] = {
	{ "configure", "iia", g_xdg_types + 0 }, { "close", "", g_xdg_types + 0 }, { "configure_bounds", "4ii", g_xdg_types + 0 },
	{ "wm_capabilities", "5a", g_xdg_types + 0 }
};
static const WlMessage g_xdg_popup_requests[] = {
	{ "destroy", "", g_xdg_types + 0 }, { "grab", "ou", g_xdg_types + 22 }, { "reposition", "3ou", g_xdg_types + 24 }
};
static const WlMessage g_xdg_popup_events[] = {
	{ "configure", "iiii", g_xdg_types + 0 }, { "popup_done", "", g_xdg_types + 0 }, { "repositioned", "3u", g_xdg_types + 0 }
};
static const WlInterface g_xdg_wm_base = { "xdg_wm_base", 7, 4, g_xdg_wm_base_requests, 1, g_xdg_wm_base_events };
static const WlInterface g_xdg_positioner = { "xdg_positioner", 7, 10, g_xdg_positioner_requests, 0, 0 };
static const WlInterface g_xdg_surface = { "xdg_surface", 7, 5, g_xdg_surface_requests, 1, g_xdg_surface_events };
static const WlInterface g_xdg_toplevel = { "xdg_toplevel", 7, 14, g_xdg_toplevel_requests, 4, g_xdg_toplevel_events };
static const WlInterface g_xdg_popup = { "xdg_popup", 7, 3, g_xdg_popup_requests, 3, g_xdg_popup_events };

#define MAX_WINDOWS 4

static struct {
	void *lib, *display, *registry, *compositor, *wm_base, *seat, *keyboard, *pointer;
	const WlInterface *wl_registry, *wl_compositor, *wl_surface, *wl_seat, *wl_keyboard, *wl_pointer, *wl_output;
	void *(*connect)(const char *);
	int (*roundtrip)(void *);
	int (*dispatch_pending)(void *);
	int (*flush)(void *);
	int (*prepare_read)(void *);
	int (*read_events)(void *);
	void (*cancel_read)(void *);
	int (*get_fd)(void *);
	void *(*marshal)(void *, u32, const WlInterface *, u32, u32, ...);
	int (*add_listener)(void *, void (**)(void), void *);
	void (*destroy)(void *);
	char why[300];
	int tried, ok;
} g_wl;

static struct {
	int used, configured, closed, w, h, resized; /* resized: a new size from the compositor, for the GPU's surface */
	void *surface, *xsurface, *toplevel;
	u8 keys[256];
	f64 mx, my;
	u8 buttons[4];
} g_win[MAX_WINDOWS];

static i32 wl_window_of(void *surface) /* the window of a wl_surface, or -1 */
{
	i32 k;
	for (k = 0; k < MAX_WINDOWS; k++) {
		if (g_win[k].used && g_win[k].surface == surface) return k;
	}
	return -1;
}

static i32 g_wl_keys_to = -1, g_wl_mouse_to = -1; /* the windows that have the keyboard and the mouse */

/* the listeners: one function per event of the version bound, in the protocol's order */
static void wl_on_global(void *data, void *registry, u32 name, const char *iface, u32 version)
{
	(void)data;
	(void)version;
	if (!strcmp(iface, "wl_compositor"))
		g_wl.compositor = g_wl.marshal(registry, 0, g_wl.wl_compositor, 1, 0, name, iface, (u32)1, (void *)0);
	else if (!strcmp(iface, "xdg_wm_base"))
		g_wl.wm_base = g_wl.marshal(registry, 0, &g_xdg_wm_base, 1, 0, name, iface, (u32)1, (void *)0);
	else if (!strcmp(iface, "wl_seat") && !g_wl.seat)
		g_wl.seat = g_wl.marshal(registry, 0, g_wl.wl_seat, 1, 0, name, iface, (u32)1, (void *)0);
}
static void wl_on_global_remove(void *data, void *registry, u32 name) { (void)data; (void)registry; (void)name; }
static void (*const g_wl_registry_listener[])(void) = { (void (*)(void))wl_on_global, (void (*)(void))wl_on_global_remove };

static void wl_on_ping(void *data, void *base, u32 serial) { (void)data; g_wl.marshal(base, 3, 0, 1, 0, serial); }
static void (*const g_wl_wm_base_listener[])(void) = { (void (*)(void))wl_on_ping };

static void wl_on_xconfigure(void *data, void *xs, u32 serial)
{
	i32 k = (i32)(long)data;
	g_wl.marshal(xs, 4, 0, 1, 0, serial); /* ack_configure */
	g_win[k].configured = 1;
}
static void (*const g_wl_xsurface_listener[])(void) = { (void (*)(void))wl_on_xconfigure };

static void wl_on_tconfigure(void *data, void *tl, i32 w, i32 h, void *states)
{
	i32 k = (i32)(long)data;
	(void)tl;
	(void)states;
	if (w > 0 && h > 0 && (w != g_win[k].w || h != g_win[k].h)) { /* 0: the window picks its size */
		g_win[k].w = w;
		g_win[k].h = h;
		g_win[k].resized = 1;
	}
}
static void wl_on_close(void *data, void *tl) { (void)tl; g_win[(i32)(long)data].closed = 1; }
static void (*const g_wl_toplevel_listener[])(void) = { (void (*)(void))wl_on_tconfigure, (void (*)(void))wl_on_close };

static void wl_on_keymap(void *data, void *kb, u32 format, i32 fd, u32 size) { (void)data; (void)kb; (void)format; (void)size; close(fd); }
static void wl_on_kenter(void *data, void *kb, u32 serial, void *surface, void *keys)
{
	(void)data;
	(void)kb;
	(void)serial;
	(void)keys;
	g_wl_keys_to = wl_window_of(surface);
}
static void wl_on_kleave(void *data, void *kb, u32 serial, void *surface)
{
	i32 k = wl_window_of(surface);
	(void)data;
	(void)kb;
	(void)serial;
	if (k >= 0) memset(g_win[k].keys, 0, sizeof(g_win[k].keys)); /* keys let go elsewhere never come back up here */
	g_wl_keys_to = -1;
}
static void wl_on_key(void *data, void *kb, u32 serial, u32 time, u32 key, u32 state)
{
	(void)data;
	(void)kb;
	(void)serial;
	(void)time;
	if (g_wl_keys_to >= 0 && key < 256) g_win[g_wl_keys_to].keys[key] = (u8)(state == 1);
}
static void wl_on_mods(void *data, void *kb, u32 serial, u32 d, u32 la, u32 lo, u32 g)
{
	(void)data;
	(void)kb;
	(void)serial;
	(void)d;
	(void)la;
	(void)lo;
	(void)g;
}
static void (*const g_wl_keyboard_listener[])(void) = {
	(void (*)(void))wl_on_keymap, (void (*)(void))wl_on_kenter, (void (*)(void))wl_on_kleave, (void (*)(void))wl_on_key,
	(void (*)(void))wl_on_mods
};

static void wl_on_penter(void *data, void *p, u32 serial, void *surface, i32 x, i32 y)
{
	(void)data;
	(void)p;
	(void)serial;
	if ((g_wl_mouse_to = wl_window_of(surface)) >= 0) {
		g_win[g_wl_mouse_to].mx = x / 256.0; /* wl_fixed_t: 24.8 */
		g_win[g_wl_mouse_to].my = y / 256.0;
	}
}
static void wl_on_pleave(void *data, void *p, u32 serial, void *surface)
{
	i32 k = wl_window_of(surface);
	(void)data;
	(void)p;
	(void)serial;
	if (k >= 0) memset(g_win[k].buttons, 0, sizeof(g_win[k].buttons));
	g_wl_mouse_to = -1;
}
static void wl_on_motion(void *data, void *p, u32 time, i32 x, i32 y)
{
	(void)data;
	(void)p;
	(void)time;
	if (g_wl_mouse_to >= 0) {
		g_win[g_wl_mouse_to].mx = x / 256.0;
		g_win[g_wl_mouse_to].my = y / 256.0;
	}
}
static void wl_on_button(void *data, void *p, u32 serial, u32 time, u32 button, u32 state)
{
	(void)data;
	(void)p;
	(void)serial;
	(void)time;
	if (g_wl_mouse_to >= 0 && button >= 0x110 && button <= 0x112) /* BTN_LEFT, BTN_RIGHT, BTN_MIDDLE: 1, 2, 3 */
		g_win[g_wl_mouse_to].buttons[button - 0x110 + 1] = (u8)(state == 1);
}
static void wl_on_axis(void *data, void *p, u32 time, u32 axis, i32 value) { (void)data; (void)p; (void)time; (void)axis; (void)value; }
static void (*const g_wl_pointer_listener[])(void) = {
	(void (*)(void))wl_on_penter, (void (*)(void))wl_on_pleave, (void (*)(void))wl_on_motion, (void (*)(void))wl_on_button,
	(void (*)(void))wl_on_axis
};

static void wl_on_caps(void *data, void *seat, u32 caps)
{
	(void)data;
	if ((caps & 1) && !g_wl.pointer) { /* get_pointer */
		g_wl.pointer = g_wl.marshal(seat, 0, g_wl.wl_pointer, 1, 0, (void *)0);
		g_wl.add_listener(g_wl.pointer, (void (**)(void))g_wl_pointer_listener, 0);
	}
	if ((caps & 2) && !g_wl.keyboard) { /* get_keyboard */
		g_wl.keyboard = g_wl.marshal(seat, 1, g_wl.wl_keyboard, 1, 0, (void *)0);
		g_wl.add_listener(g_wl.keyboard, (void (**)(void))g_wl_keyboard_listener, 0);
	}
}
static void (*const g_wl_seat_listener[])(void) = { (void (*)(void))wl_on_caps };

/* wl_open: libwayland-client, the compositor and what gael needs of it; 0 with the reason in g_wl.why */
static int wl_open(void)
{
	if (g_wl.tried) return g_wl.ok;
	g_wl.tried = 1;
	if (!(g_wl.lib = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_LOCAL))) {
		sprintf(g_wl.why, "no libwayland-client: %.200s", dlerror());
		return 0;
	}
#define WLSYM(field, name) \
	if (!(*(void **)&g_wl.field = dlsym(g_wl.lib, name))) { sprintf(g_wl.why, "libwayland-client has no " name); return 0; }
	WLSYM(connect, "wl_display_connect")
	WLSYM(roundtrip, "wl_display_roundtrip")
	WLSYM(dispatch_pending, "wl_display_dispatch_pending")
	WLSYM(flush, "wl_display_flush")
	WLSYM(prepare_read, "wl_display_prepare_read")
	WLSYM(read_events, "wl_display_read_events")
	WLSYM(cancel_read, "wl_display_cancel_read")
	WLSYM(get_fd, "wl_display_get_fd")
	WLSYM(marshal, "wl_proxy_marshal_flags")
	WLSYM(add_listener, "wl_proxy_add_listener")
	WLSYM(destroy, "wl_proxy_destroy")
	WLSYM(wl_registry, "wl_registry_interface")
	WLSYM(wl_compositor, "wl_compositor_interface")
	WLSYM(wl_surface, "wl_surface_interface")
	WLSYM(wl_seat, "wl_seat_interface")
	WLSYM(wl_keyboard, "wl_keyboard_interface")
	WLSYM(wl_pointer, "wl_pointer_interface")
	WLSYM(wl_output, "wl_output_interface")
#undef WLSYM
	/* the argument types of xdg-shell's messages, as wayland-scanner lays them out */
	g_xdg_types[4] = &g_xdg_positioner;
	g_xdg_types[5] = &g_xdg_surface;
	g_xdg_types[6] = g_wl.wl_surface;
	g_xdg_types[7] = &g_xdg_toplevel;
	g_xdg_types[8] = &g_xdg_popup;
	g_xdg_types[9] = &g_xdg_surface;
	g_xdg_types[10] = &g_xdg_positioner;
	g_xdg_types[11] = &g_xdg_toplevel;
	g_xdg_types[12] = g_wl.wl_seat;
	g_xdg_types[16] = g_wl.wl_seat;
	g_xdg_types[18] = g_wl.wl_seat;
	g_xdg_types[21] = g_wl.wl_output;
	g_xdg_types[22] = g_wl.wl_seat;
	g_xdg_types[24] = &g_xdg_positioner;
	if (!(g_wl.display = g_wl.connect(0))) {
		strcpy(g_wl.why, "no Wayland compositor to connect to (WAYLAND_DISPLAY)");
		return 0;
	}
	g_wl.registry = g_wl.marshal(g_wl.display, 1, g_wl.wl_registry, 1, 0, (void *)0); /* get_registry */
	g_wl.add_listener(g_wl.registry, (void (**)(void))g_wl_registry_listener, 0);
	g_wl.roundtrip(g_wl.display);
	if (!g_wl.compositor || !g_wl.wm_base) {
		strcpy(g_wl.why, "the Wayland compositor has no wl_compositor or no xdg_wm_base");
		return 0;
	}
	g_wl.add_listener(g_wl.wm_base, (void (**)(void))g_wl_wm_base_listener, 0);
	if (g_wl.seat) g_wl.add_listener(g_wl.seat, (void (**)(void))g_wl_seat_listener, 0);
	g_wl.roundtrip(g_wl.display);
	g_wl.ok = 1;
	return 1;
}

/* wl_window TITLE W H: a window, configured by the compositor; its number, or -1 with the reason in g_wl.why */
static i32 wl_window(const char *title, int w, int h)
{
	i32 k;
	int n;
	if (!wl_open()) return -1;
	for (k = 0; k < MAX_WINDOWS && g_win[k].used; k++) {}
	if (k == MAX_WINDOWS) {
		sprintf(g_wl.why, "there are %d windows already, all there can be", MAX_WINDOWS);
		return -1;
	}
	memset(&g_win[k], 0, sizeof(g_win[k]));
	g_win[k].used = 1;
	g_win[k].w = w;
	g_win[k].h = h;
	g_win[k].surface = g_wl.marshal(g_wl.compositor, 0, g_wl.wl_surface, 1, 0, (void *)0); /* create_surface */
	g_win[k].xsurface = g_wl.marshal(g_wl.wm_base, 2, &g_xdg_surface, 1, 0, (void *)0, g_win[k].surface); /* get_xdg_surface */
	g_wl.add_listener(g_win[k].xsurface, (void (**)(void))g_wl_xsurface_listener, (void *)(long)k);
	g_win[k].toplevel = g_wl.marshal(g_win[k].xsurface, 1, &g_xdg_toplevel, 1, 0, (void *)0); /* get_toplevel */
	g_wl.add_listener(g_win[k].toplevel, (void (**)(void))g_wl_toplevel_listener, (void *)(long)k);
	g_wl.marshal(g_win[k].toplevel, 2, 0, 1, 0, title); /* set_title */
	g_wl.marshal(g_win[k].toplevel, 3, 0, 1, 0, "gael"); /* set_app_id */
	g_wl.marshal(g_win[k].surface, 6, 0, 1, 0); /* commit: the compositor answers with a configure */
	for (n = 0; n < 100 && !g_win[k].configured; n++) g_wl.roundtrip(g_wl.display);
	if (!g_win[k].configured) {
		strcpy(g_wl.why, "the Wayland compositor never configured the window");
		g_win[k].used = 0;
		return -1;
	}
	g_win[k].resized = 0; /* the size it starts with is the one the GPU's surface gets */
	return k;
}

/* wl_close K: window K off the screen, once what drew into it has let go of it (gfx.c); its keys and buttons up */
static o wl_close(i32 k)
{
	g_wl.marshal(g_win[k].toplevel, 0, 0, 1, 1); /* xdg_toplevel.destroy; flag 1 destroys the proxy too */
	g_wl.marshal(g_win[k].xsurface, 0, 0, 1, 1); /* xdg_surface.destroy */
	g_wl.marshal(g_win[k].surface, 0, 0, 1, 1);  /* wl_surface.destroy */
	g_win[k].toplevel = g_win[k].xsurface = g_win[k].surface = 0;
	memset(g_win[k].keys, 0, sizeof(g_win[k].keys));
	memset(g_win[k].buttons, 0, sizeof(g_win[k].buttons));
	g_wl.flush(g_wl.display);
}

/* wl_pump: the events that came in since the last frame, without waiting for any */
static o wl_pump(void)
{
	struct pollfd pfd;
	if (!g_wl.ok) return;
	while (g_wl.prepare_read(g_wl.display) != 0) g_wl.dispatch_pending(g_wl.display);
	g_wl.flush(g_wl.display);
	pfd.fd = g_wl.get_fd(g_wl.display);
	pfd.events = POLLIN;
	pfd.revents = 0;
	if (poll(&pfd, 1, 0) > 0) g_wl.read_events(g_wl.display);
	else g_wl.cancel_read(g_wl.display);
	g_wl.dispatch_pending(g_wl.display);
}

/* the keys by name, as evdev numbers them (linux/input-event-codes.h): by their place, whatever the layout */
static const struct {
	const char *name;
	u8 code;
} g_wl_keynames[] = {
	{ "escape", 1 }, { "1", 2 }, { "2", 3 }, { "3", 4 }, { "4", 5 }, { "5", 6 }, { "6", 7 }, { "7", 8 }, { "8", 9 },
	{ "9", 10 }, { "0", 11 }, { "minus", 12 }, { "equal", 13 }, { "backspace", 14 }, { "tab", 15 }, { "q", 16 },
	{ "w", 17 }, { "e", 18 }, { "r", 19 }, { "t", 20 }, { "y", 21 }, { "u", 22 }, { "i", 23 }, { "o", 24 }, { "p", 25 },
	{ "enter", 28 }, { "ctrl", 29 }, { "a", 30 }, { "s", 31 }, { "d", 32 }, { "f", 33 }, { "g", 34 }, { "h", 35 },
	{ "j", 36 }, { "k", 37 }, { "l", 38 }, { "shift", 42 }, { "z", 44 }, { "x", 45 }, { "c", 46 }, { "v", 47 },
	{ "b", 48 }, { "n", 49 }, { "m", 50 }, { "alt", 56 }, { "space", 57 }, { "f1", 59 }, { "f2", 60 }, { "f3", 61 },
	{ "f4", 62 }, { "f5", 63 }, { "f6", 64 }, { "f7", 65 }, { "f8", 66 }, { "f9", 67 }, { "f10", 68 }, { "up", 103 },
	{ "left", 105 }, { "right", 106 }, { "down", 108 }
};

/* wl_key WINDOW NAME: 1 while the key is down, 0 when not, -1 for a name gael does not know */
static int wl_key(i32 k, const char *name)
{
	size_t i;
	for (i = 0; i < GAEL_COUNT(g_wl_keynames); i++) {
		if (strcmp(name, g_wl_keynames[i].name)) continue;
		if (g_win[k].keys[g_wl_keynames[i].code]) return 1;
		if (!strcmp(name, "shift")) return g_win[k].keys[54]; /* the right one too */
		if (!strcmp(name, "ctrl")) return g_win[k].keys[97];
		if (!strcmp(name, "alt")) return g_win[k].keys[100];
		return 0;
	}
	return -1;
}
