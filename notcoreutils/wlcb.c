/* wlcb - a clipboard manager for Wayland: the same tool as xcb, same CLI,
 * but it speaks the data-control protocol (ext-data-control-v1 if the
 * compositor has it, else zwlr_data_control_manager_v1), which unlike
 * plain wl_data_device needs no focused window - so it works under dwl
 * and any wlroots compositor. It watches the clipboard (text/plain;
 * charset=utf-8, text/plain), keeps a RAM ring buffer of history (not
 * just the most recent entry), and re-activates old entries by owning
 * the selection itself. If the app that owned the clipboard exits, the
 * last entry is re-owned, so copied text survives it.
 *
 * Daemon + client, one binary: the first `copy`/`paste`/TUI
 * invocation auto-starts a background daemon (Wayland connection + a
 * Unix socket at $XDG_RUNTIME_DIR/wlcb.sock, or /tmp/.wlcb-
 * <uid>.sock if that's unset) if one isn't already running; every
 * later invocation is just a client talking to it over the socket.
 * The daemon holds ALL clipboard state in RAM (a fixed-size static
 * ring buffer - no dynamic allocation, no disk-backed history file).
 *
 * usage: wlcb [-p] copy [TEXT...]   (stdin if no TEXT given)
 *        wlcb paste            (prints the current entry)
 *        wlcb list             (prints every history entry, numbered)
 *        wlcb select N         (re-activates history entry N)
 *        wlcb [-p]             (interactive TUI, no args, real tty only)
 *   -p  when this call starts the daemon, it also records the PRIMARY
 *       (middle-click) selection into history. Ignored if one is running.
 */

#define BASE_TYPE_IMPLEMENTATION
#include "base.h"
#define ARENA_ALLOC_IMPLEMENTATION
#include "arena_alloc.h"
#define FUZZY_MATCH_IMPLEMENTATION
#include "fuzzy_match.h"
#include "patches.h"
#include "ext-data-control-v1-client-protocol.h"
#include "wlr-data-control-unstable-v1-client-protocol.h"
#include <poll.h>
#include <wayland-client.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define CLIP_MAX_ENTRIES 50
#define CLIP_MAX_LEN 65536

typedef struct {
    char text[CLIP_MAX_LEN];
    size_t len;
} Clip_Entry;

/* ---- daemon-only state (RAM history) ---- */
static Clip_Entry g_history[CLIP_MAX_ENTRIES];
static i32 g_history_count = 0; /* entries in use, 0..CLIP_MAX_ENTRIES, index 0 = current/most recent */

static void clip_socket_path(char *out, size_t cap)
{
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (xdg && xdg[0]) snprintf(out, cap, "%s/wlcb.sock", xdg);
    else snprintf(out, cap, "/tmp/.wlcb-%d.sock", (i32)getuid());
}

/* moves history[idx] to the front (index 0 becomes it) - re-copying an
 * older entry makes it "the current one" again, same behavior every
 * real clipboard-history manager has. */
static void clip_bump_to_front(i32 idx)
{
    Clip_Entry tmp;
    if (idx <= 0 || idx >= g_history_count) return;
    tmp = g_history[idx];
    memmove(&g_history[1], &g_history[0], (size_t)idx * sizeof(Clip_Entry));
    g_history[0] = tmp;
}

/* inserts a brand new entry at the front, dropping the oldest if the
 * ring is full - real bounded RAM usage, never grows without limit. */
static void clip_push_front(const char *text, size_t len)
{
    i32 i;
    if (len >= CLIP_MAX_LEN) len = CLIP_MAX_LEN - 1;

    /* de-dup: if this exact text is already in history, just bump it
     * instead of storing a second copy - avoids the common "copy the
     * same thing twice" case silently filling the ring with dupes. */
    for (i = 0; i < g_history_count; i++) {
        if (g_history[i].len == len && memcmp(g_history[i].text, text, len) == 0) {
            clip_bump_to_front(i);
            return;
        }
    }

    if (g_history_count < CLIP_MAX_ENTRIES) g_history_count++;
    memmove(&g_history[1], &g_history[0], (size_t)(g_history_count - 1) * sizeof(Clip_Entry));
    memcpy(g_history[0].text, text, len);
    g_history[0].len = len;
}

/* ============================================================
 * daemon: wlr-data-control / ext-data-control (works without a
 * focused window, which plain wl_data_device cannot) + Unix socket
 * server. The two protocols have the same shape, so one set of
 * wrappers (g_ext picks) drives either.
 * ============================================================ */

typedef struct {
    void *proxy;
    i32 has_utf8, has_plain, has_utf8s, has_string;
} Offer;

static struct wl_display *g_dpy;
static struct wl_seat *g_seat;
static struct ext_data_control_manager_v1 *g_ext_mgr;
static struct zwlr_data_control_manager_v1 *g_wlr_mgr;
static i32 g_ext;       /* 1 = ext-data-control-v1, 0 = zwlr_data_control_manager_v1 */
static i32 g_primary;   /* -p: also record the PRIMARY selection into history */
static void *g_dev;
static void *g_src;     /* our live source, NULL if somebody else owns the clipboard */
static char g_cur[CLIP_MAX_LEN]; /* snapshot served by g_src (history[0] may change under it) */
static size_t g_cur_len;

static const char *const g_mimes[] = { "text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "STRING", "TEXT" };

static void on_global(void *d, struct wl_registry *reg, u32 name, const char *iface, u32 ver)
{
    (void)d;
    if (strcmp(iface, "wl_seat") == 0 && !g_seat)
        g_seat = wl_registry_bind(reg, name, &wl_seat_interface, 1);
    else if (strcmp(iface, ext_data_control_manager_v1_interface.name) == 0)
        g_ext_mgr = wl_registry_bind(reg, name, &ext_data_control_manager_v1_interface, 1);
    else if (strcmp(iface, zwlr_data_control_manager_v1_interface.name) == 0)
        g_wlr_mgr = wl_registry_bind(reg, name, &zwlr_data_control_manager_v1_interface, ver < 2 ? ver : 2);
}
static void on_global_remove(void *d, struct wl_registry *r, u32 n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener g_reg_l = { on_global, on_global_remove };

/* reads fd until EOF into out; gives up after 1s of silence so a stuck
 * source can never freeze the daemon */
static size_t clip_read_fd(i32 fd, char *out, size_t cap)
{
    size_t got = 0;
    for (;;) {
        struct pollfd p;
        ssize_t n;
        p.fd = fd; p.events = POLLIN;
        if (poll(&p, 1, 1000) <= 0) break;
        if (got + 1 >= cap) break;
        n = read(fd, out + got, cap - 1 - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    return got;
}

static void clip_receive(Offer *o, i32 primary)
{
    const char *mime = NULL;
    i32 p[2];
    static char buf[CLIP_MAX_LEN]; /* static: keeps the 64K off the stack, daemon is single-threaded */
    size_t n;

    (void)primary;
    if (o->has_utf8) mime = g_mimes[0];
    else if (o->has_plain) mime = g_mimes[1];
    else if (o->has_utf8s) mime = g_mimes[2];
    else if (o->has_string) mime = g_mimes[3];
    if (!mime || pipe(p) < 0) return;
    if (g_ext) ext_data_control_offer_v1_receive(o->proxy, mime, p[1]);
    else zwlr_data_control_offer_v1_receive(o->proxy, mime, p[1]);
    close(p[1]);
    wl_display_flush(g_dpy);
    n = clip_read_fd(p[0], buf, sizeof(buf));
    close(p[0]);
    if (n > 0) clip_push_front(buf, n);
}

static void offer_mime(Offer *o, const char *m)
{
    if (strcmp(m, "text/plain;charset=utf-8") == 0) o->has_utf8 = 1;
    else if (strcmp(m, "text/plain") == 0) o->has_plain = 1;
    else if (strcmp(m, "UTF8_STRING") == 0) o->has_utf8s = 1;
    else if (strcmp(m, "STRING") == 0) o->has_string = 1;
}

static void offer_free(Offer *o)
{
    if (g_ext) ext_data_control_offer_v1_destroy(o->proxy);
    else zwlr_data_control_offer_v1_destroy(o->proxy);
    free(o);
}

static void clip_own_selection(void);

static void dev_selection(void *offer, i32 primary)
{
    Offer *o = offer ? wl_proxy_get_user_data(offer) : NULL;
    if (primary) {
        if (o && g_primary) clip_receive(o, 1);
    } else if (o) {
        clip_receive(o, 0);
    } else if (!g_src && g_history_count > 0) {
        clip_own_selection(); /* the app that owned it went away: keep the clipboard alive */
    }
    if (o) offer_free(o);
}

/* ---- per-protocol shims (identical bodies, different types) ---- */
#define SHIMS(P, T) \
static void P##_offer_cb(void *d, struct T##_offer_v1 *of, const char *m) { (void)of; offer_mime(d, m); } \
static const struct T##_offer_v1_listener P##_offer_l = { P##_offer_cb }; \
static void P##_data_offer(void *d, struct T##_device_v1 *dev, struct T##_offer_v1 *of) \
{ Offer *o = calloc(1, sizeof(*o)); (void)d; (void)dev; if (!o) return; o->proxy = of; T##_offer_v1_add_listener(of, &P##_offer_l, o); } \
static void P##_selection(void *d, struct T##_device_v1 *dev, struct T##_offer_v1 *of) { (void)d; (void)dev; dev_selection(of, 0); } \
static void P##_finished(void *d, struct T##_device_v1 *dev) { (void)d; (void)dev; } \
static void P##_primary(void *d, struct T##_device_v1 *dev, struct T##_offer_v1 *of) { (void)d; (void)dev; dev_selection(of, 1); } \
static const struct T##_device_v1_listener P##_dev_l = { P##_data_offer, P##_selection, P##_finished, P##_primary }; \
static void P##_send(void *d, struct T##_source_v1 *s, const char *m, i32 fd) { (void)d; (void)s; (void)m; clip_send_fd(fd); } \
static void P##_cancelled(void *d, struct T##_source_v1 *s) { (void)d; T##_source_v1_destroy(s); if (g_src == (void *)s) g_src = NULL; } \
static const struct T##_source_v1_listener P##_src_l = { P##_send, P##_cancelled };

/* writes the served snapshot to a requester's pipe, bounded wait */
static void clip_send_fd(i32 fd)
{
    size_t off = 0;
    i32 fl = fcntl(fd, F_GETFL);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    while (off < g_cur_len) {
        struct pollfd p;
        ssize_t n = write(fd, g_cur + off, g_cur_len - off);
        if (n > 0) { off += (size_t)n; continue; }
        if (n < 0 && errno != EAGAIN && errno != EINTR) break;
        p.fd = fd; p.events = POLLOUT;
        if (poll(&p, 1, 2000) <= 0) break;
    }
    close(fd);
}

SHIMS(ext, ext_data_control)
SHIMS(wlr, zwlr_data_control)

static void clip_own_selection(void)
{
    size_t i;
    if (!g_dev || g_history_count == 0) return;
    memcpy(g_cur, g_history[0].text, g_history[0].len);
    g_cur_len = g_history[0].len;
    if (g_src) { /* replacing our own source: cancelled() will not fire for it, drop it here */
        if (g_ext) ext_data_control_source_v1_destroy(g_src);
        else zwlr_data_control_source_v1_destroy(g_src);
        g_src = NULL;
    }
    if (g_ext) {
        struct ext_data_control_source_v1 *s = ext_data_control_manager_v1_create_data_source(g_ext_mgr);
        for (i = 0; i < sizeof(g_mimes) / sizeof(g_mimes[0]); i++) ext_data_control_source_v1_offer(s, g_mimes[i]);
        ext_data_control_source_v1_add_listener(s, &ext_src_l, NULL);
        ext_data_control_device_v1_set_selection(g_dev, s);
        g_src = s;
    } else {
        struct zwlr_data_control_source_v1 *s = zwlr_data_control_manager_v1_create_data_source(g_wlr_mgr);
        for (i = 0; i < sizeof(g_mimes) / sizeof(g_mimes[0]); i++) zwlr_data_control_source_v1_offer(s, g_mimes[i]);
        zwlr_data_control_source_v1_add_listener(s, &wlr_src_l, NULL);
        zwlr_data_control_device_v1_set_selection(g_dev, s);
        g_src = s;
    }
    wl_display_flush(g_dpy);
}

/* reads a length-prefixed frame: one line "N\n" then exactly N raw
 * bytes. Used for both directions of the protocol (client->daemon
 * ADD payloads, daemon->client PASTE/LIST replies) - simple and
 * unambiguous for arbitrary binary-safe clipboard text (embedded
 * newlines included). */
static i32 clip_read_frame(i32 fd, char *out, size_t cap, size_t *out_len)
{
    char lenbuf[32];
    size_t li = 0;
    size_t n;
    ssize_t got;

    while (li + 1 < sizeof(lenbuf)) {
        got = read(fd, lenbuf + li, 1);
        if (got <= 0) return 0;
        if (lenbuf[li] == '\n') break;
        li++;
    }
    lenbuf[li] = '\0';
    n = (size_t)strtoul(lenbuf, NULL, 10);
    if (n >= cap) n = cap - 1;

    {
        size_t read_total = 0;
        while (read_total < n) {
            got = read(fd, out + read_total, n - read_total);
            if (got <= 0) return 0;
            read_total += (size_t)got;
        }
    }
    out[n] = '\0';
    *out_len = n;
    return 1;
}

static void clip_write_frame(i32 fd, const char *data, size_t len)
{
    char lenbuf[32];
    i32 n = snprintf(lenbuf, sizeof(lenbuf), "%lu\n", (unsigned long)len); /* %zu is real UB under -std=c89, see cald.c */
    write(fd, lenbuf, (size_t)n);
    if (len > 0) write(fd, data, len);
}

static void clip_handle_client(i32 fd)
{
    char cmd[16];
    char buf[CLIP_MAX_LEN];
    size_t len;
    ssize_t n;
    i32 ci = 0;

    /* command word up to the first space or newline */
    for (;;) {
        char c;
        n = read(fd, &c, 1);
        if (n <= 0) { close(fd); return; }
        if (c == ' ' || c == '\n' || ci + 1 >= (i32)sizeof(cmd)) break;
        cmd[ci++] = c;
    }
    cmd[ci] = '\0';

    if (strcmp(cmd, "ADD") == 0) {
        if (clip_read_frame(fd, buf, sizeof(buf), &len)) {
            clip_push_front(buf, len);
            clip_own_selection();
            write(fd, "OK\n", 3);
        }
    } else if (strcmp(cmd, "PASTE") == 0) {
        clip_write_frame(fd, g_history_count > 0 ? g_history[0].text : "", g_history_count > 0 ? g_history[0].len : 0);
    } else if (strcmp(cmd, "LIST") == 0) {
        char hdr[32];
        i32 i;
        n = snprintf(hdr, sizeof(hdr), "%d\n", g_history_count);
        write(fd, hdr, (size_t)n);
        for (i = 0; i < g_history_count; i++) clip_write_frame(fd, g_history[i].text, g_history[i].len);
    } else if (strcmp(cmd, "SELECT") == 0) {
        char idxbuf[16];
        i32 ii = 0;
        for (;;) {
            char c;
            n = read(fd, &c, 1);
            if (n <= 0 || c == '\n' || ii + 1 >= (i32)sizeof(idxbuf)) break;
            idxbuf[ii++] = c;
        }
        idxbuf[ii] = '\0';
        clip_bump_to_front(atoi(idxbuf));
        clip_own_selection();
        write(fd, "OK\n", 3);
    }
    close(fd);
}

static i32 clip_listen_socket(const char *path)
{
    i32 fd;
    struct sockaddr_un addr;

    unlink(path); /* stale socket from a crashed/killed daemon - safe to remove, we're about to bind our own */
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { close(fd); return -1; }
    listen(fd, 8);
    return fd;
}

static void clip_run_daemon(void)
{
    char sockpath[512];
    i32 listen_fd;
    struct wl_registry *reg;

    g_dpy = wl_display_connect(NULL);
    if (!g_dpy) { fprintf(stderr, "wlcb: cannot connect to the Wayland display - is WAYLAND_DISPLAY set?\n"); exit(1); }
    reg = wl_display_get_registry(g_dpy);
    wl_registry_add_listener(reg, &g_reg_l, NULL);
    wl_display_roundtrip(g_dpy);
    if (g_seat && g_ext_mgr) {
        g_ext = 1;
        g_dev = ext_data_control_manager_v1_get_data_device(g_ext_mgr, g_seat);
        ext_data_control_device_v1_add_listener(g_dev, &ext_dev_l, NULL);
    } else if (g_seat && g_wlr_mgr) {
        g_ext = 0;
        g_dev = zwlr_data_control_manager_v1_get_data_device(g_wlr_mgr, g_seat);
        zwlr_data_control_device_v1_add_listener(g_dev, &wlr_dev_l, NULL);
    } else {
        fprintf(stderr, "wlcb: compositor offers neither ext_data_control_manager_v1 nor zwlr_data_control_manager_v1\n");
        exit(1);
    }
    wl_display_roundtrip(g_dpy);

    clip_socket_path(sockpath, sizeof(sockpath));
    listen_fd = clip_listen_socket(sockpath);
    if (listen_fd < 0) { fprintf(stderr, "wlcb: cannot listen on %s: %s\n", sockpath, strerror(errno)); exit(1); }

    signal(SIGPIPE, SIG_IGN); /* a client that disconnects mid-write must not kill the whole daemon */

    for (;;) {
        struct pollfd pf[2];
        while (wl_display_prepare_read(g_dpy) != 0) wl_display_dispatch_pending(g_dpy);
        if (wl_display_flush(g_dpy) < 0 && errno != EAGAIN) { wl_display_cancel_read(g_dpy); break; }
        pf[0].fd = wl_display_get_fd(g_dpy); pf[0].events = POLLIN;
        pf[1].fd = listen_fd; pf[1].events = POLLIN;
        if (poll(pf, 2, -1) < 0) { wl_display_cancel_read(g_dpy); if (errno == EINTR) continue; break; }

        if (pf[0].revents & POLLIN) {
            if (wl_display_read_events(g_dpy) < 0) break;
            if (wl_display_dispatch_pending(g_dpy) < 0) break;
        } else {
            wl_display_cancel_read(g_dpy);
        }
        if (pf[0].revents & (POLLERR | POLLHUP)) break; /* compositor went away */
        if (pf[1].revents & POLLIN) {
            i32 cfd = accept(listen_fd, NULL, NULL);
            if (cfd >= 0) clip_handle_client(cfd);
        }
    }
    unlink(sockpath);
}

/* ============================================================
 * client side
 * ============================================================ */

static i32 clip_connect(void)
{
    char sockpath[512];
    struct sockaddr_un addr;
    i32 fd;

    clip_socket_path(sockpath, sizeof(sockpath));
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sockpath, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { close(fd); return -1; }
    return fd;
}

/* connects to the daemon, starting it (fork+detach) if nothing
 * answers yet - short bounded retry while the fresh daemon opens its
 * X connection and binds its socket. */
static i32 clip_connect_or_start_daemon(void)
{
    i32 fd = clip_connect();
    pid_t pid;
    i32 tries;

    if (fd >= 0) return fd;

    pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        setsid();
        if (fork() > 0) _exit(0); /* double-fork so the daemon isn't a child of the CLI invocation at all - it must outlive it */
        {
            i32 devnull = open("/dev/null", O_RDWR);
            if (devnull >= 0) { dup2(devnull, 0); dup2(devnull, 1); dup2(devnull, 2); close(devnull); }
        }
        clip_run_daemon();
        _exit(0);
    }
    waitpid(pid, NULL, 0); /* reap the immediate child (which already _exit(0)'d after its own fork) */

    for (tries = 0; tries < 50; tries++) {
        struct timeval tv;
        fd = clip_connect();
        if (fd >= 0) return fd;
        tv.tv_sec = 0; tv.tv_usec = 50000;
        select(0, NULL, NULL, NULL, &tv);
    }
    return -1;
}

static i32 clip_cmd_copy(i32 argc, char **argv)
{
    char buf[CLIP_MAX_LEN];
    size_t len;
    i32 fd;

    if (argc > 0) {
        i32 i;
        len = 0;
        for (i = 0; i < argc && len < sizeof(buf) - 1; i++) {
            i32 n = snprintf(buf + len, sizeof(buf) - len, "%s%s", argv[i], i + 1 < argc ? " " : "");
            len += (size_t)n;
        }
    } else {
        len = fread(buf, 1, sizeof(buf) - 1, stdin);
    }
    /* trim exactly one trailing newline, same convention st's own
     * clipboard patch already uses - a pasted-back-in shell command
     * getting an extra blank line is a real, common annoyance. */
    if (len > 0 && buf[len - 1] == '\n') len--;

    fd = clip_connect_or_start_daemon();
    if (fd < 0) { fprintf(stderr, "wlcb: cannot reach/start the daemon\n"); return 1; }
    {
        char hdr[32];
        i32 n = snprintf(hdr, sizeof(hdr), "ADD %lu\n", (unsigned long)len); /* %zu is real UB under -std=c89, see cald.c */
        write(fd, hdr, (size_t)n);
        write(fd, buf, len);
    }
    {
        char reply[8];
        read(fd, reply, sizeof(reply));
    }
    close(fd);
    return 0;
}

static i32 clip_cmd_paste(void)
{
    i32 fd = clip_connect_or_start_daemon();
    char buf[CLIP_MAX_LEN];
    size_t len;
    if (fd < 0) { fprintf(stderr, "wlcb: cannot reach/start the daemon\n"); return 1; }
    write(fd, "PASTE\n", 6);
    if (clip_read_frame(fd, buf, sizeof(buf), &len)) fwrite(buf, 1, len, stdout);
    printf("\n");
    close(fd);
    return 0;
}

/* fills g_history from the daemon's own LIST reply - used by both
 * `clipboard list` and the TUI below, so both see the exact same
 * data the daemon is really holding. */
static i32 clip_fetch_history(i32 fd)
{
    char hdr[32];
    i32 hi = 0;
    i32 count, i;
    for (;;) {
        char c;
        if (read(fd, &c, 1) <= 0 || c == '\n' || hi + 1 >= (i32)sizeof(hdr)) break;
        hdr[hi++] = c;
    }
    hdr[hi] = '\0';
    count = atoi(hdr);
    if (count > CLIP_MAX_ENTRIES) count = CLIP_MAX_ENTRIES;
    for (i = 0; i < count; i++) {
        if (!clip_read_frame(fd, g_history[i].text, sizeof(g_history[i].text), &g_history[i].len)) break;
    }
    g_history_count = i;
    return g_history_count;
}

static i32 clip_cmd_list(void)
{
    i32 fd = clip_connect_or_start_daemon();
    i32 i;
    if (fd < 0) { fprintf(stderr, "wlcb: cannot reach/start the daemon\n"); return 1; }
    write(fd, "LIST\n", 5);
    clip_fetch_history(fd);
    close(fd);
    for (i = 0; i < g_history_count; i++) {
        char preview[80];
        size_t plen = g_history[i].len < sizeof(preview) - 1 ? g_history[i].len : sizeof(preview) - 1;
        memcpy(preview, g_history[i].text, plen);
        preview[plen] = '\0';
        {
            char *nl = strchr(preview, '\n');
            if (nl) *nl = '\0'; /* one-line preview only */
        }
        printf("%2d: %s\n", i, preview);
    }
    return 0;
}

static i32 clip_cmd_select(i32 idx)
{
    i32 fd = clip_connect_or_start_daemon();
    char cmd[32];
    i32 n;
    if (fd < 0) { fprintf(stderr, "wlcb: cannot reach/start the daemon\n"); return 1; }
    n = snprintf(cmd, sizeof(cmd), "SELECT %d\n", idx);
    write(fd, cmd, (size_t)n);
    {
        char reply[8];
        read(fd, reply, sizeof(reply));
    }
    close(fd);
    return 0;
}

/* ============================================================
 * TUI - same real raw-termios + fuzzy_match.h pattern this project's
 * own wifi.c TUI already established, applied here to clipboard
 * history instead of wifi networks.
 * ============================================================ */

static void clip_tui(void)
{
    struct termios orig, raw;
    i32 fd;
    char query[128];
    i32 query_len = 0;
    i32 visible[CLIP_MAX_ENTRIES];
    i32 visible_count;
    i32 selected = 0;
    Arena scratch;

    fd = clip_connect_or_start_daemon();
    if (fd < 0) { fprintf(stderr, "wlcb: cannot reach/start the daemon\n"); return; }
    write(fd, "LIST\n", 5);
    clip_fetch_history(fd);
    close(fd);

    if (g_history_count == 0) { printf("(clipboard history is empty)\n"); return; }

    tcgetattr(STDIN_FILENO, &orig);
    raw = orig;
    raw.c_lflag &= (unsigned)~(ECHO | ICANON | ISIG);
    raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);

    memset(&scratch, 0, sizeof(scratch));
    for (;;) {
        i32 i;
        u8 b;
        ssize_t n;

        visible_count = 0;
        for (i = 0; i < g_history_count; i++) {
            char preview[200];
            size_t plen = g_history[i].len < sizeof(preview) - 1 ? g_history[i].len : sizeof(preview) - 1;
            i32 score;
            memcpy(preview, g_history[i].text, plen);
            preview[plen] = '\0';
            {
                char *nl = strchr(preview, '\n');
                if (nl) *nl = '\0';
            }
            score = query_len == 0 ? 0 : fuzzy_match(&scratch, preview, strlen(preview), query, (size_t)query_len);
            if (query_len == 0 || score >= 0) visible[visible_count++] = i;
        }
        if (selected >= visible_count) selected = visible_count > 0 ? visible_count - 1 : 0;

        printf("\033[2J\033[H");
        printf("clipboard history - type to filter, \342\206\221/\342\206\223 move, Enter to activate, Esc/Ctrl-C quit\n\n");
        for (i = 0; i < visible_count; i++) {
            char preview[80];
            size_t plen = g_history[visible[i]].len < sizeof(preview) - 1 ? g_history[visible[i]].len : sizeof(preview) - 1;
            memcpy(preview, g_history[visible[i]].text, plen);
            preview[plen] = '\0';
            {
                char *nl = strchr(preview, '\n');
                if (nl) *nl = '\0';
            }
            printf("%s%2d: %s\033[0m\n", i == selected ? "\033[7m" : "", visible[i], preview);
        }
        printf("\n> %.*s", query_len, query);
        fflush(stdout);

        n = read(STDIN_FILENO, &b, 1);
        if (n <= 0 || b == 3) break;
        if (b == '\r' || b == '\n') {
            if (visible_count > 0) clip_cmd_select(visible[selected]);
            break;
        }
        if (b == 127 || b == 8) { if (query_len > 0) { query_len--; selected = 0; } continue; }
        if (b == 14) { if (selected + 1 < visible_count) selected++; continue; }
        if (b == 16) { if (selected > 0) selected--; continue; }
        if (b == 27) {
            fd_set fds; struct timeval tv;
            FD_ZERO(&fds); FD_SET(STDIN_FILENO, &fds);
            tv.tv_sec = 0; tv.tv_usec = 50000;
            if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0) {
                u8 seq[2];
                if (read(STDIN_FILENO, &seq[0], 1) == 1 && seq[0] == '[' && read(STDIN_FILENO, &seq[1], 1) == 1) {
                    if (seq[1] == 'A' && selected > 0) selected--;
                    else if (seq[1] == 'B' && selected + 1 < visible_count) selected++;
                }
                continue;
            }
            break;
        }
        if (b >= 0x20 && b < 0x7f && query_len < (i32)sizeof(query) - 1) { query[query_len++] = (char)b; selected = 0; }
    }

    tcsetattr(STDIN_FILENO, TCSANOW, &orig);
    printf("\033[2J\033[H");
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "-p") == 0) { g_primary = 1; argc--; argv++; }
    if (argc >= 2 && strcmp(argv[1], "copy") == 0) return clip_cmd_copy(argc - 2, argv + 2);
    if (argc >= 2 && strcmp(argv[1], "paste") == 0) return clip_cmd_paste();
    if (argc >= 2 && strcmp(argv[1], "list") == 0) return clip_cmd_list();
    if (argc >= 3 && strcmp(argv[1], "select") == 0) return clip_cmd_select(atoi(argv[2]));
    if (argc == 1) { clip_tui(); return 0; }

    fprintf(stderr, "usage: wlcb [-p] copy [TEXT...] | paste | list | select N\n       wlcb   (interactive TUI)\n");
    return 2;
}
