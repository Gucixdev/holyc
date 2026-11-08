/* tests/globals.c - print every wl_registry global of the running compositor, one "name vN" per line (no wayland-info needed) */
#include <stdio.h>
#include <wayland-client.h>
static void g(void *d, struct wl_registry *r, uint32_t id, const char *i, uint32_t v) { printf("%s v%u\n", i, v); }
static void gr(void *d, struct wl_registry *r, uint32_t id) {}
static const struct wl_registry_listener l = { g, gr };
int main(void)
{
	struct wl_display *d = wl_display_connect(NULL);
	if (!d) return 1;
	wl_registry_add_listener(wl_display_get_registry(d), &l, NULL);
	wl_display_roundtrip(d);
	return 0;
}
