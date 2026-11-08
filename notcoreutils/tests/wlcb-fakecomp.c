/* wlcb-fakecomp - a tiny fake compositor for tests/wlcb: just wl_seat plus
 * the data-control manager (ext-data-control-v1, or zwlr with -DWLR), enough
 * to drive wlcb's daemon without a real desktop. Not a display server.
 *
 * Commands on stdin, one per line (it plays the "other application"):
 *   copy TEXT      an app takes the clipboard with TEXT
 *   primary TEXT   an app takes the PRIMARY selection with TEXT
 *   clear          the app that owned the clipboard went away
 *   read           print "GOT:<text>" for whatever the clipboard holds now
 *   quit
 * It prints "READY" once the socket is up. Protocol header/glue names come
 * from the Makefile-style generated files (see tests/wlcb). */
#define _GNU_SOURCE
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server.h>
#ifdef WLR
#include "wlr-data-control-unstable-v1-server-protocol.h"
#define N(x) zwlr_data_control_##x
#define PROTO_V 2
#else
#include "ext-data-control-v1-server-protocol.h"
#define N(x) ext_data_control_##x
#define PROTO_V 1
#endif

typedef struct { struct wl_resource *res; char *text; char *mimes[8]; int nmimes; } Src; /* res NULL = internal "other app" */
typedef struct { struct wl_resource *res; struct wl_client *client; } Dev;

static Dev g_devs[8];
static int g_ndevs;
static Src *g_cur[2]; /* [0] clipboard, [1] primary */
static struct wl_display *g_disp;
static int g_quit;

static int src_has(Src *s, const char *m)
{
    int i;
    if (!s->res) return strcmp(m, "text/plain;charset=utf-8") == 0 || strcmp(m, "text/plain") == 0;
    for (i = 0; i < s->nmimes; i++) if (strcmp(s->mimes[i], m) == 0) return 1;
    return 0;
}

static void offer_receive(struct wl_client *c, struct wl_resource *r, const char *mime, int fd)
{
    Src *s = wl_resource_get_user_data(r);
    (void)c;
    if (s && src_has(s, mime)) {
        if (s->res) N(source_v1_send_send)(s->res, mime, fd);
        else if (write(fd, s->text, strlen(s->text)) < 0) { }
    }
    close(fd);
}
static void res_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static const struct N(offer_v1_interface) offer_impl = { .receive = offer_receive, .destroy = res_destroy };

static void send_sel(Dev *d, int kind)
{
    Src *s = g_cur[kind];
    struct wl_resource *o = NULL;
    int i;
    /* like wlroots: the owner itself is told "no offer" for its own source */
    if (s && s->res && wl_resource_get_client(s->res) == d->client) s = NULL;
    if (s) {
        o = wl_resource_create(d->client, &N(offer_v1_interface), wl_resource_get_version(d->res), 0);
        wl_resource_set_implementation(o, &offer_impl, s, NULL);
        N(device_v1_send_data_offer)(d->res, o);
        if (!s->res) { N(offer_v1_send_offer)(o, "text/plain;charset=utf-8"); N(offer_v1_send_offer)(o, "text/plain"); }
        else for (i = 0; i < s->nmimes; i++) N(offer_v1_send_offer)(o, s->mimes[i]);
    }
    if (kind == 0) N(device_v1_send_selection)(d->res, o);
    else N(device_v1_send_primary_selection)(d->res, o);
}

static void set_cur(Src *s, int kind)
{
    int i;
    Src *old = g_cur[kind];
    g_cur[kind] = s;
    if (old && old != s && old->res) N(source_v1_send_cancelled)(old->res);
    for (i = 0; i < g_ndevs; i++) send_sel(&g_devs[i], kind);
}

static void dev_set_selection(struct wl_client *c, struct wl_resource *r, struct wl_resource *src) { (void)c; (void)r; set_cur(src ? wl_resource_get_user_data(src) : NULL, 0); }
static void dev_set_primary(struct wl_client *c, struct wl_resource *r, struct wl_resource *src) { (void)c; (void)r; set_cur(src ? wl_resource_get_user_data(src) : NULL, 1); }
static const struct N(device_v1_interface) dev_impl = { .set_selection = dev_set_selection, .set_primary_selection = dev_set_primary, .destroy = res_destroy };

static void src_offer(struct wl_client *c, struct wl_resource *r, const char *m)
{
    Src *s = wl_resource_get_user_data(r);
    (void)c;
    if (s->nmimes < 8) s->mimes[s->nmimes++] = strdup(m);
}
static const struct N(source_v1_interface) src_impl = { .offer = src_offer, .destroy = res_destroy };

static void mgr_create_source(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    Src *s = calloc(1, sizeof(*s));
    s->res = wl_resource_create(c, &N(source_v1_interface), wl_resource_get_version(r), id);
    wl_resource_set_implementation(s->res, &src_impl, s, NULL);
}
static void dev_gone(struct wl_resource *r)
{
    int i;
    for (i = 0; i < g_ndevs; i++) if (g_devs[i].res == r) { g_devs[i] = g_devs[--g_ndevs]; break; }
}
static void mgr_get_device(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *seat)
{
    struct wl_resource *d = wl_resource_create(c, &N(device_v1_interface), wl_resource_get_version(r), id);
    (void)seat;
    wl_resource_set_implementation(d, &dev_impl, NULL, dev_gone);
    if (g_ndevs < 8) { g_devs[g_ndevs].res = d; g_devs[g_ndevs].client = c; g_ndevs++; }
    send_sel(&g_devs[g_ndevs - 1], 0);
}
static const struct N(manager_v1_interface) mgr_impl = { .create_data_source = mgr_create_source, .get_data_device = mgr_get_device, .destroy = res_destroy };

static void bind_mgr(struct wl_client *c, void *d, uint32_t v, uint32_t id)
{
    struct wl_resource *r = wl_resource_create(c, &N(manager_v1_interface), (int)v, id);
    (void)d;
    wl_resource_set_implementation(r, &mgr_impl, NULL, NULL);
}
static void bind_seat(struct wl_client *c, void *d, uint32_t v, uint32_t id)
{
    struct wl_resource *r = wl_resource_create(c, &wl_seat_interface, 1, id);
    (void)d; (void)v;
    wl_resource_set_implementation(r, NULL, NULL, NULL);
}

static int on_stdin(int fd, uint32_t mask, void *data)
{
    char line[70000];
    (void)fd; (void)mask; (void)data;
    if (!fgets(line, sizeof(line), stdin)) { g_quit = 1; wl_display_terminate(g_disp); return 0; }
    line[strcspn(line, "\n")] = 0;
    if (!strncmp(line, "copy ", 5) || !strncmp(line, "primary ", 8)) {
        int k = line[0] == 'p';
        Src *s = calloc(1, sizeof(*s));
        s->text = strdup(line + (k ? 8 : 5));
        set_cur(s, k);
    } else if (!strcmp(line, "clear")) {
        set_cur(NULL, 0);
    } else if (!strcmp(line, "read")) {
        Src *s = g_cur[0];
        char buf[70000];
        int p[2];
        size_t n = 0;
        if (!s) { printf("GOT:\n"); fflush(stdout); return 0; }
        if (!s->res) { printf("GOT:%s\n", s->text); fflush(stdout); return 0; }
        if (pipe(p) < 0) return 0;
        N(source_v1_send_send)(s->res, "text/plain;charset=utf-8", p[1]);
        close(p[1]);
        wl_display_flush_clients(g_disp);
        for (;;) {
            struct pollfd pf = { p[0], POLLIN, 0 };
            ssize_t r;
            if (poll(&pf, 1, 2000) <= 0) break;
            r = read(p[0], buf + n, sizeof(buf) - 1 - n);
            if (r <= 0) break;
            n += (size_t)r;
        }
        close(p[0]);
        buf[n] = 0;
        printf("GOT:%s\n", buf);
        fflush(stdout);
    } else if (!strcmp(line, "quit")) {
        wl_display_terminate(g_disp);
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *sock = argc > 1 ? argv[1] : "wlcb-fake";
    g_disp = wl_display_create();
    if (wl_display_add_socket(g_disp, sock) < 0) { fprintf(stderr, "fakecomp: cannot add socket\n"); return 1; }
    wl_global_create(g_disp, &wl_seat_interface, 1, NULL, bind_seat);
    wl_global_create(g_disp, &N(manager_v1_interface), PROTO_V, NULL, bind_mgr);
    wl_event_loop_add_fd(wl_display_get_event_loop(g_disp), 0, WL_EVENT_READABLE, on_stdin, NULL);
    printf("READY\n");
    fflush(stdout);
    wl_display_run(g_disp);
    wl_display_destroy(g_disp);
    return 0;
}
