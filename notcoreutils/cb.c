/* cb - a clipboard manager: owns the X11 CLIPBOARD selection (the ICCCM
 * pattern: XSetSelectionOwner/SelectionRequest/XConvertSelection), keeps a RAM ring buffer of
 * history (not just the single most recent entry), and is reachable
 * from anywhere: plain CLI (`cb copy`/`cb paste`,
 * scriptable), an interactive TUI (`cb` with no args - fuzzy-
 * search history, Enter to re-activate an old entry), and any real
 * GUI app on the system (since it's a real CLIPBOARD owner, Ctrl+V in
 * a browser/GUI text field works exactly like it would against any
 * other clipboard manager).
 *
 * Daemon + client, one binary: the first `copy`/`paste`/TUI
 * invocation auto-starts a background daemon (X11 connection + a
 * Unix socket at $XDG_RUNTIME_DIR/cb.sock, or /tmp/.cb-
 * <uid>.sock if that's unset) if one isn't already running; every
 * later invocation is just a client talking to it over the socket.
 * The daemon holds ALL clipboard state in RAM (a fixed-size static
 * ring buffer - no dynamic allocation, no disk-backed history file)
 * for as long as it's alive, matching how a real clipboard manager
 * has to work: X11 selections are only served by whoever currently
 * owns them, so history has to live somewhere that outlives any one
 * `cb copy` invocation.
 *
 * usage: cb copy [TEXT...]   (stdin if no TEXT given)
 *        cb paste            (prints the current entry)
 *        cb list             (prints every history entry, numbered)
 *        cb select N         (re-activates history entry N)
 *        cb                  (interactive TUI, no args, real tty only)
 */

#define BASE_TYPE_IMPLEMENTATION
#include "base.h"
#define ARENA_ALLOC_IMPLEMENTATION
#include "arena_alloc.h"
#define FUZZY_MATCH_IMPLEMENTATION
#include "fuzzy_match.h"
#include "patches.h"
#include <X11/Xlib.h>
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
    if (xdg && xdg[0]) snprintf(out, cap, "%s/cb.sock", xdg);
    else snprintf(out, cap, "/tmp/.cb-%d.sock", (i32)getuid());
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
 * daemon: X11 ICCCM ownership (same real pattern already proven in
 * this codebase's own st.c clipboard patch) + Unix socket server
 * ============================================================ */

static Display *g_dpy;
static Window g_win;

static void clip_own_selection(void)
{
    Atom clipboard;
    if (!g_dpy) return; /* defensive: real usage always has XOpenDisplay succeed before the daemon serves any client (see clip_run_daemon), but never dereference a display that isn't there */
    clipboard = XInternAtom(g_dpy, "CLIPBOARD", False);
    XSetSelectionOwner(g_dpy, clipboard, g_win, CurrentTime);
    XFlush(g_dpy);
}

static void clip_handle_selection_request(XSelectionRequestEvent *req)
{
    XSelectionEvent notify;
    Atom utf8 = XInternAtom(g_dpy, "UTF8_STRING", False);
    const char *text = g_history_count > 0 ? g_history[0].text : "";
    size_t len = g_history_count > 0 ? g_history[0].len : 0;

    XChangeProperty(g_dpy, req->requestor, req->property, utf8, 8, PropModeReplace,
                     (const unsigned char *)text, (int)len);

    notify.type = SelectionNotify;
    notify.requestor = req->requestor;
    notify.selection = req->selection;
    notify.target = req->target;
    notify.property = req->property;
    notify.time = req->time;
    XSendEvent(g_dpy, req->requestor, False, 0, (XEvent *)&notify);
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
    i32 xfd;

    g_dpy = XOpenDisplay(NULL);
    if (!g_dpy) { fprintf(stderr, "cb: cannot open X display - is DISPLAY set?\n"); exit(1); }
    g_win = XCreateSimpleWindow(g_dpy, DefaultRootWindow(g_dpy), 0, 0, 1, 1, 0, 0, 0);
    xfd = ConnectionNumber(g_dpy);

    clip_socket_path(sockpath, sizeof(sockpath));
    listen_fd = clip_listen_socket(sockpath);
    if (listen_fd < 0) { fprintf(stderr, "cb: cannot listen on %s: %s\n", sockpath, strerror(errno)); exit(1); }

    signal(SIGPIPE, SIG_IGN); /* a client that disconnects mid-write must not kill the whole daemon */

    for (;;) {
        fd_set fds;
        i32 maxfd = listen_fd > xfd ? listen_fd : xfd;
        FD_ZERO(&fds);
        FD_SET(listen_fd, &fds);
        FD_SET(xfd, &fds);
        if (select(maxfd + 1, &fds, NULL, NULL, NULL) < 0) { if (errno == EINTR) continue; break; }

        if (FD_ISSET(xfd, &fds)) {
            while (XPending(g_dpy) > 0) {
                XEvent ev;
                XNextEvent(g_dpy, &ev);
                if (ev.type == SelectionRequest) clip_handle_selection_request(&ev.xselectionrequest);
                /* SelectionClear (another app took ownership) needs no
                 * action - our own history stays exactly as it was, we
                 * just aren't the live owner until the next copy. */
            }
        }
        if (FD_ISSET(listen_fd, &fds)) {
            i32 cfd = accept(listen_fd, NULL, NULL);
            if (cfd >= 0) clip_handle_client(cfd);
        }
    }
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
    if (fd < 0) { fprintf(stderr, "cb: cannot reach/start the daemon\n"); return 1; }
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
    if (fd < 0) { fprintf(stderr, "cb: cannot reach/start the daemon\n"); return 1; }
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
    if (fd < 0) { fprintf(stderr, "cb: cannot reach/start the daemon\n"); return 1; }
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
    if (fd < 0) { fprintf(stderr, "cb: cannot reach/start the daemon\n"); return 1; }
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
    if (fd < 0) { fprintf(stderr, "cb: cannot reach/start the daemon\n"); return; }
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
    if (argc >= 2 && strcmp(argv[1], "copy") == 0) return clip_cmd_copy(argc - 2, argv + 2);
    if (argc >= 2 && strcmp(argv[1], "paste") == 0) return clip_cmd_paste();
    if (argc >= 2 && strcmp(argv[1], "list") == 0) return clip_cmd_list();
    if (argc >= 3 && strcmp(argv[1], "select") == 0) return clip_cmd_select(atoi(argv[2]));
    if (argc == 1) { clip_tui(); return 0; }

    fprintf(stderr, "usage: cb copy [TEXT...] | paste | list | select N\n       cb   (interactive TUI)\n");
    return 2;
}
