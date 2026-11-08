/* fakecomp - tiny fake compositor: wl_seat + ext_idle_notifier_v1 only.
 * stdin: "activity" (user input: resumes idle clients, restarts timers),
 * "quit". Prints READY when listening on $WAYLAND_DISPLAY. Not a display server. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server.h>
#include "ext-idle-notify-v1-server-protocol.h"

typedef struct N { struct wl_resource *res; struct wl_event_source *t; uint32_t ms; int idle; struct N *next; } N;
static N *list;
static struct wl_display *disp;
static struct wl_event_loop *loop;

static int fire(void *d)
{
	N *n = d; n->idle = 1; ext_idle_notification_v1_send_idled(n->res); return 0;
}
static void destroy_req(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static const struct ext_idle_notification_v1_interface n_impl = { destroy_req };
static void n_gone(struct wl_resource *r)
{
	N *n = wl_resource_get_user_data(r);
	for (N **p = &list; *p; p = &(*p)->next) if (*p == n) { *p = n->next; break; }
	wl_event_source_remove(n->t); free(n);
}
static void get_n(struct wl_client *c, struct wl_resource *r, uint32_t id, uint32_t ms, struct wl_resource *seat)
{
	(void)seat;
	N *n = calloc(1, sizeof *n); n->ms = ms;
	n->res = wl_resource_create(c, &ext_idle_notification_v1_interface, wl_resource_get_version(r), id);
	wl_resource_set_implementation(n->res, &n_impl, n, n_gone);
	n->t = wl_event_loop_add_timer(loop, fire, n);
	wl_event_source_timer_update(n->t, ms);
	n->next = list; list = n;
}
static const struct ext_idle_notifier_v1_interface nf_impl = { destroy_req, get_n };
static void bind_nf(struct wl_client *c, void *d, uint32_t v, uint32_t id)
{
	(void)d; struct wl_resource *r = wl_resource_create(c, &ext_idle_notifier_v1_interface, v, id);
	wl_resource_set_implementation(r, &nf_impl, NULL, NULL);
}
static void bind_seat(struct wl_client *c, void *d, uint32_t v, uint32_t id)
{
	(void)d; struct wl_resource *r = wl_resource_create(c, &wl_seat_interface, v < 1 ? v : 1, id);
	wl_resource_set_implementation(r, NULL, NULL, NULL);
}
static int on_stdin(int fd, uint32_t mask, void *d)
{
	char b[128]; (void)mask; (void)d;
	ssize_t n = read(fd, b, sizeof b - 1);
	if (n <= 0) { wl_display_terminate(disp); return 0; }
	b[n] = 0;
	if (strstr(b, "quit")) wl_display_terminate(disp);
	if (strstr(b, "activity"))
		for (N *p = list; p; p = p->next) {
			if (p->idle) { p->idle = 0; ext_idle_notification_v1_send_resumed(p->res); }
			wl_event_source_timer_update(p->t, p->ms);
		}
	return 0;
}
int main(void)
{
	disp = wl_display_create(); loop = wl_display_get_event_loop(disp);
	const char *s = wl_display_add_socket_auto(disp);
	if (!s) return 1;
	wl_global_create(disp, &wl_seat_interface, 1, NULL, bind_seat);
	wl_global_create(disp, &ext_idle_notifier_v1_interface, 1, NULL, bind_nf);
	wl_event_loop_add_fd(loop, 0, WL_EVENT_READABLE, on_stdin, NULL);
	printf("READY %s\n", s); fflush(stdout);
	wl_display_run(disp);
	return 0;
}
