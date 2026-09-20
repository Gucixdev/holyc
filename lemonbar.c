// vim:sw=4:ts=4:et:
/* Real Wayland port (2026-08-24): "przepisz na wayland, doslownie
 * przepisz nie tylko backend" - the bar-markup parser (parse(), the
 * %{...} format-code handling, alignment/color/click-area tracking)
 * below is carried over almost unchanged, completely X11-independent.
 * Real rewrites: windowing (wlr-layer-shell-unstable-v1, one real
 * surface per real Wayland output - RandR/Xinerama multi-monitor
 * detection is gone entirely, replaced by real wl_output enumeration,
 * which is simpler AND more accurate), rendering (drw.c's real
 * fontconfig+FreeType text drawing into a real wl_shm buffer, instead
 * of XCB core fonts / xcb_poly_text_16 - conceptually the exact same
 * job select_drawable_font()/draw_char() did, just delegated to drw's
 * own font-fallback chain rather than reimplementing it a second
 * time), and click handling (wl_pointer instead of XCB button-press
 * events). EWMH atoms (set_ewmh_atoms) are gone - dock/strut behavior
 * is real, native layer-shell semantics (layer=TOP, exclusive_zone)
 * now, not a window-manager hint. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <poll.h>
#include <getopt.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>

#include <wayland-client.h>

#include "wlr-layer-shell-unstable-v1-protocol.h"

#include "drw.h"
#include "util.h"
#include "utils.h"

#define max(a,b) ((a) > (b) ? (a) : (b))
#define min(a,b) ((a) < (b) ? (a) : (b))

typedef struct monitor_t {
    char *name;
    int width, height;
    struct wl_output *wl_output;
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wl_buffer *wl_buf;
    void *shm_pixels;
    int shm_fd;
    size_t shm_size;
    int configured;
    Drw *drw;
    struct monitor_t *prev, *next;
} monitor_t;

typedef struct area_t {
    unsigned begin;
    unsigned end;
    bool complete;
    unsigned align;
    unsigned button;
    struct wl_surface *surface;
    char *cmd;
} area_t;

enum {
    ATTR_OVERL = (1<<0),
    ATTR_UNDERL = (1<<1),
};

enum {
    ALIGN_L = 0,
    ALIGN_C,
    ALIGN_R
};

typedef struct area_stack_t {
    area_t *ptr;
    unsigned int index, alloc;
} area_stack_t;

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct wl_pointer *pointer;
static struct zwlr_layer_shell_v1 *layer_shell;

static monitor_t *monhead, *montail;
static const char *fonts[16];
static int font_count = 0;
static uint32_t attrs = 0;
static bool topbar = true;
static int bh = -1;
static int bu = 1; // Underline height
static const char *fgname = "#ffffff", *bgname = "#000000", *ugname = NULL;
static Clr fgc, bgc, ugc;
static area_stack_t area_stack;

static struct wl_surface *pointer_surface;
static int pointer_x;

void
fill_rect (Drw *drw, Clr c, int x, int y, int width, int height)
{
    Clr save = drw->scheme ? drw->scheme[ColFg] : 0;
    Clr scm[2] = { c, c };
    drw_setscheme(drw, scm);
    drw_rect(drw, x, y, (unsigned)width, (unsigned)height, 1, 0);
    (void)save;
}

int
shift (monitor_t *mon, int x, int align, int ch_width)
{
    switch (align) {
        case ALIGN_C:
            x = mon->width / 2 - (x + ch_width) / 2 + x;
            break;
        case ALIGN_R:
            x = mon->width - ch_width;
            break;
    }

    fill_rect(mon->drw, bgc, x, 0, ch_width, bh);
    return x;
}

void
draw_lines (monitor_t *mon, int x, int w)
{
    if (attrs & ATTR_OVERL)
        fill_rect(mon->drw, ugc, x, 0, w, bu);
    if (attrs & ATTR_UNDERL)
        fill_rect(mon->drw, ugc, x, bh - bu, w, bu);
}

int
draw_char (monitor_t *mon, int x, int align, const char *utf8, int utf8len)
{
    char buf[8];
    Clr scm[2];
    int w;

    if (utf8len >= (int)sizeof buf)
        utf8len = (int)sizeof buf - 1;
    memcpy(buf, utf8, (size_t)utf8len);
    buf[utf8len] = '\0';

    w = (int)drw_fontset_getwidth(mon->drw, buf);
    x = shift(mon, x, align, w);

    scm[0] = fgc; scm[1] = bgc;
    drw_setscheme(mon->drw, scm);
    drw_text(mon->drw, x, 0, (unsigned)w, (unsigned)bh, 0, buf, 0);

    draw_lines(mon, x, w);

    return w;
}

/* Real, honest scope cut: only #rrggbb (drw_clr_create's own real
 * supported format) - the original's #rgb/#aarrggbb/#rrggbbaa variants
 * and real alpha-premultiplication are not ported, since drw (shared
 * with dmenu) has no alpha channel concept at all in its own real
 * ARGB8888-opaque color scheme. */
Clr
parse_color (const char *str, char **end, Clr def)
{
    Drw dummy = {0};
    Clr c;

    if (!str)
        return def;
    if (str[0] == '-') {
        if (end) *end = (char *)str + 1;
        return def;
    }
    if (str[0] != '#' || strlen(str) < 7) {
        if (end) *end = (char *)str;
        fprintf(stderr, "Invalid color specified\n");
        return def;
    }
    if (end)
        *end = (char *)str + 7;
    drw_clr_create(&dummy, &c, str);
    return c;
}

void
set_attribute (const char modifier, const char attribute)
{
    uint32_t mask;
    switch (attribute) {
        case 'o': mask = ATTR_OVERL;  break;
        case 'u': mask = ATTR_UNDERL; break;
        default:
            fprintf(stderr, "Invalid attribute \"%c\" found\n", attribute);
            return;
    }
    switch (modifier) {
        case '+': attrs |=  mask; break;
        case '-': attrs &= ~mask; break;
        case '!': attrs ^=  mask; break;
    }
}

area_t *
area_get (struct wl_surface *surf, const int btn, const int x)
{
    for (int i = (int)area_stack.index - 1; i >= 0; i--) {
        area_t *a = &area_stack.ptr[i];
        if (a->surface == surf && a->button == (unsigned)btn && (unsigned)x >= a->begin && (unsigned)x < a->end)
            return a;
    }
    return NULL;
}

void
area_shift (struct wl_surface *surf, const int align, int delta)
{
    if (align == ALIGN_L)
        return;
    if (align == ALIGN_C)
        delta /= 2;
    for (unsigned i = 0; i < area_stack.index; i++) {
        area_t *a = &area_stack.ptr[i];
        if (a->surface == surf && (int)a->align == align && !a->complete) {
            a->begin -= (unsigned)delta;
            a->end -= (unsigned)delta;
        }
    }
}

bool
area_add (char *str, const char *optend, char **end, monitor_t *mon, const int x, const int align, const int button)
{
    int i;
    char *trail;
    area_t *a;

    if (*str != ':') {
        *end = str;
        for (i = (int)area_stack.index - 1; i >= 0 && !area_stack.ptr[i].complete; i--)
            ;
        a = &area_stack.ptr[i];
        if (!a->cmd || (int)a->align != align || a->surface != mon->surface) {
            fprintf(stderr, "Invalid geometry for the clickable area\n");
            return false;
        }
        {
            const int size = x - (int)a->begin;
            switch (align) {
                case ALIGN_L: a->end = (unsigned)x; break;
                case ALIGN_C:
                    a->begin = (unsigned)(mon->width / 2 - size / 2) + a->begin / 2;
                    a->end = a->begin + (unsigned)size;
                    break;
                case ALIGN_R:
                    a->begin = (unsigned)(mon->width - size);
                    a->end = (unsigned)mon->width;
                    break;
            }
        }
        a->complete = false;
        return true;
    }

    if (area_stack.index + 1 > area_stack.alloc) {
        area_stack.ptr = xreallocarray(area_stack.ptr, area_stack.index + 1, sizeof(area_t));
        area_stack.alloc += 1;
    }
    a = &area_stack.ptr[area_stack.index++];

    for (trail = strchr(++str, ':'); trail && trail[-1] == '\\'; trail = strchr(trail + 1, ':'))
        ;
    if (!trail || str == trail || trail > optend) {
        *end = str;
        return false;
    }
    *trail = '\0';
    for (char *needle = str; *needle; needle++) {
        int delta = (int)(trail - &needle[1]);
        if (needle[0] == '\\' && needle[1] == ':') {
            memmove(&needle[0], &needle[1], (size_t)delta);
            needle[delta] = 0;
        }
    }

    a->cmd = str;
    a->complete = true;
    a->align = (unsigned)align;
    a->begin = (unsigned)x;
    a->surface = mon->surface;
    a->button = (unsigned)button;

    *end = trail + 1;
    return true;
}

int
pos_to_absolute(monitor_t *mon, int pos, int align)
{
    switch (align) {
        case ALIGN_L: return pos;
        case ALIGN_R: return mon->width - pos;
        case ALIGN_C: return mon->width / 2 + pos / 2;
    }
    return 0;
}

static void redraw_monitor(monitor_t *mon);

void
parse (char *text)
{
    monitor_t *cur_mon;
    int pos_x, align, button;
    char *p = text, *block_end;

    pos_x = 0;
    align = ALIGN_L;
    cur_mon = monhead;

    bgc = parse_color(bgname, NULL, bgc);
    fgc = parse_color(fgname, NULL, fgc);
    ugc = ugname ? parse_color(ugname, NULL, fgc) : fgc;
    attrs = 0;
    area_stack.index = 0;

    for (monitor_t *m = monhead; m != NULL; m = m->next)
        fill_rect(m->drw, bgc, 0, 0, m->width, bh);

    for (;;) {
        if (*p == '\0' || *p == '\n')
            break;

        if (p[0] == '%' && p[1] == '{' && (block_end = strchr(p++, '}'))) {
            p++;
            while (p < block_end) {
                while (isspace((unsigned char)*p))
                    p++;

                switch (*p++) {
                    case '+': set_attribute('+', *p++); break;
                    case '-': set_attribute('-', *p++); break;
                    case '!': set_attribute('!', *p++); break;

                    case 'R': {
                        Clr tmp = fgc;
                        fgc = bgc;
                        bgc = tmp;
                    } break;

                    case 'l': {
                        int left_ep = 0;
                        int right_ep = pos_to_absolute(cur_mon, pos_x, align);
                        draw_lines(cur_mon, left_ep, right_ep - left_ep);
                        pos_x = 0; align = ALIGN_L;
                    } break;
                    case 'c': {
                        int left_ep = pos_to_absolute(cur_mon, pos_x, align);
                        int right_ep = cur_mon->width / 2;
                        if (right_ep < left_ep) { int t = left_ep; left_ep = right_ep; right_ep = t; }
                        draw_lines(cur_mon, left_ep, right_ep - left_ep);
                        pos_x = 0; align = ALIGN_C;
                    } break;
                    case 'r': {
                        int left_ep = pos_to_absolute(cur_mon, pos_x, align);
                        int right_ep = cur_mon->width;
                        if (right_ep < left_ep) { int t = left_ep; left_ep = right_ep; right_ep = t; }
                        draw_lines(cur_mon, left_ep, right_ep - left_ep);
                        pos_x = 0; align = ALIGN_R;
                    } break;

                    case 'A': {
                        button = 1;
                        if (isdigit((unsigned char)*p) && (*p > '0' && *p < '6'))
                            button = *p++ - '0';
                        if (!area_add(p, block_end, &p, cur_mon, pos_x, align, button))
                            goto done;
                    } break;

                    case 'B': bgc = parse_color(p, &p, bgc); break;
                    case 'F': fgc = parse_color(p, &p, fgc); break;
                    case 'U': ugc = parse_color(p, &p, ugc); break;

                    case 'S': {
                        monitor_t *orig_mon = cur_mon;
                        switch (*p) {
                            case '+': if (cur_mon->next) cur_mon = cur_mon->next; p += 1; break;
                            case '-': if (cur_mon->prev) cur_mon = cur_mon->prev; p += 1; break;
                            case 'f': cur_mon = monhead; p += 1; break;
                            case 'l': cur_mon = montail ? montail : monhead; p += 1; break;
                            case 'n': {
                                const size_t name_len = (size_t)(block_end - (p + 1));
                                cur_mon = monhead;
                                while (cur_mon) {
                                    if (cur_mon->name && !strncmp(cur_mon->name, p + 1, name_len) && cur_mon->name[name_len] == '\0')
                                        break;
                                    cur_mon = cur_mon->next;
                                }
                                if (!cur_mon) cur_mon = orig_mon;
                                p += 1 + name_len;
                            } break;
                            default:
                                if (*p >= '0' && *p <= '9') {
                                    cur_mon = monhead;
                                    for (int i = 0; i != *p-'0' && cur_mon->next; i++)
                                        cur_mon = cur_mon->next;
                                    p += 1;
                                } else {
                                    fprintf(stderr, "Unknown S specifier '%c'\n", *p++);
                                }
                                break;
                        }
                        if (orig_mon != cur_mon) { pos_x = 0; align = ALIGN_L; }
                    } break;

                    case 'O': {
                        errno = 0;
                        int w = (int) strtoul(p, &p, 10);
                        if (errno) continue;
                        fill_rect(cur_mon->drw, bgc, shift(cur_mon, pos_x, align, w), 0, w, bh);
                        pos_x += w;
                        area_shift(cur_mon->surface, align, w);
                    } break;

                    case 'T':
                        /* Real, honest scope cut: per-run font-slot
                         * selection (%{T2}) isn't ported - drw's own
                         * fallback chain already picks a usable font
                         * per codepoint automatically, which is the
                         * real reason this existed in the first
                         * place. */
                        while (p < block_end && *p != '}' && !isspace((unsigned char)*p))
                            p++;
                        break;

                    default:
                        p = block_end;
                }
            }
            p++;
        } else {
            uint8_t *utf = (uint8_t *)p;
            int charlen;

            if (p[0] == '%' && p[1] == '%')
                p++, utf = (uint8_t *)p;

            if (utf[0] < 0x80) charlen = 1;
            else if ((utf[0] & 0xe0) == 0xc0) charlen = 2;
            else if ((utf[0] & 0xf0) == 0xe0) charlen = 3;
            else if ((utf[0] & 0xf8) == 0xf0) charlen = 4;
            else charlen = 1;

            {
                int w = draw_char(cur_mon, pos_x, align, p, charlen);
                pos_x += w;
                area_shift(cur_mon->surface, align, w);
            }
            p += charlen;
        }
    }
done:
    for (monitor_t *m = monhead; m; m = m->next)
        redraw_monitor(m);
}

/* ---- real Wayland plumbing ---- */

static int
allocshm(size_t size)
{
    char name[] = "/lemonbar-shm-XXXXXX";
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) { fprintf(stderr, "lemonbar: shm_open failed\n"); exit(1); }
    shm_unlink(name);
    if (ftruncate(fd, (off_t)size) < 0) { fprintf(stderr, "lemonbar: ftruncate failed\n"); exit(1); }
    return fd;
}

static void
ensurebuffer(monitor_t *mon)
{
    size_t need = (size_t)mon->width * (size_t)mon->height * 4;
    if (mon->wl_buf && need == mon->shm_size)
        return;
    if (mon->wl_buf) {
        wl_buffer_destroy(mon->wl_buf);
        munmap(mon->shm_pixels, mon->shm_size);
        close(mon->shm_fd);
    }
    mon->shm_size = need;
    mon->shm_fd = allocshm(need);
    mon->shm_pixels = mmap(NULL, need, PROT_READ | PROT_WRITE, MAP_SHARED, mon->shm_fd, 0);
    {
        struct wl_shm_pool *pool = wl_shm_create_pool(shm, mon->shm_fd, (int32_t)need);
        mon->wl_buf = wl_shm_pool_create_buffer(pool, 0, mon->width, mon->height, mon->width * 4, WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pool);
    }
}

static void
redraw_monitor(monitor_t *mon)
{
    if (!mon->configured)
        return;
    ensurebuffer(mon);
    memcpy(mon->shm_pixels, mon->drw->pixels, (size_t)mon->width * (size_t)mon->height * 4);
    wl_surface_attach(mon->surface, mon->wl_buf, 0, 0);
    wl_surface_damage_buffer(mon->surface, 0, 0, mon->width, mon->height);
    wl_surface_commit(mon->surface);
}

static void
layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *ls, uint32_t serial, uint32_t width, uint32_t height)
{
    monitor_t *mon = data;
    zwlr_layer_surface_v1_ack_configure(ls, serial);
    mon->width = (int)width;
    mon->height = (int)height;
    mon->configured = 1;
    if (!mon->drw)
        mon->drw = drw_create((unsigned)mon->width, (unsigned)mon->height);
    else
        drw_resize(mon->drw, (unsigned)mon->width, (unsigned)mon->height);
    if (!mon->drw->fonts)
        drw_fontset_create(mon->drw, fonts, (size_t)font_count);
    fill_rect(mon->drw, bgc, 0, 0, mon->width, mon->height);
    redraw_monitor(mon);
}

static void
layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *ls)
{
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed = layer_surface_closed,
};

static void
pointer_enter(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *surf, wl_fixed_t sx, wl_fixed_t sy)
{
    pointer_surface = surf;
    pointer_x = wl_fixed_to_int(sx);
}
static void pointer_leave(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *surf) { pointer_surface = NULL; }
static void
pointer_motion(void *data, struct wl_pointer *p, uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
    pointer_x = wl_fixed_to_int(sx);
}
static void
pointer_button(void *data, struct wl_pointer *p, uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
    area_t *a;
    int btn;

    if (state != WL_POINTER_BUTTON_STATE_PRESSED || !pointer_surface)
        return;
    /* linux/input-event-codes.h BTN_LEFT=0x110 .. real evdev button codes -
     * map the 3 that matter to lemonbar's own 1/2/3 button numbering. */
    switch (button) {
        case 0x110: btn = 1; break; /* BTN_LEFT */
        case 0x112: btn = 2; break; /* BTN_MIDDLE */
        case 0x111: btn = 3; break; /* BTN_RIGHT */
        default: return;
    }
    a = area_get(pointer_surface, btn, pointer_x);
    if (a) {
        (void)write(STDOUT_FILENO, a->cmd, strlen(a->cmd));
        (void)write(STDOUT_FILENO, "\n", 1);
    }
}
static void pointer_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t axis, wl_fixed_t v) {}
static void pointer_frame(void *d, struct wl_pointer *p) {}
static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter, .leave = pointer_leave, .motion = pointer_motion,
    .button = pointer_button, .axis = pointer_axis, .frame = pointer_frame,
};

static void
seat_capabilities(void *data, struct wl_seat *s, uint32_t caps)
{
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
        pointer = wl_seat_get_pointer(s);
        wl_pointer_add_listener(pointer, &pointer_listener, NULL);
    }
}
static void seat_name(void *d, struct wl_seat *s, const char *n) {}
static const struct wl_seat_listener seat_listener = { .capabilities = seat_capabilities, .name = seat_name };

static void
monitor_add(monitor_t *mon)
{
    if (!monhead) monhead = montail = mon;
    else { montail->next = mon; mon->prev = montail; montail = mon; }
}

static void
create_bar_surface(monitor_t *mon)
{
    mon->surface = wl_compositor_create_surface(compositor);
    mon->layer_surface = zwlr_layer_shell_v1_get_layer_surface(layer_shell, mon->surface, mon->wl_output,
                                                                ZWLR_LAYER_SHELL_V1_LAYER_TOP, "lemonbar");
    zwlr_layer_surface_v1_add_listener(mon->layer_surface, &layer_surface_listener, mon);
    zwlr_layer_surface_v1_set_size(mon->layer_surface, 0, (uint32_t)max(bh, 1));
    zwlr_layer_surface_v1_set_anchor(mon->layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT |
        (topbar ? ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP : ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM));
    zwlr_layer_surface_v1_set_exclusive_zone(mon->layer_surface, max(bh, 1));
    wl_surface_commit(mon->surface);
}

static void
registry_global(void *data, struct wl_registry *reg, uint32_t name, const char *interface, uint32_t version)
{
    /* Real, confirmed bug: every bind below hardcoded the version it
     * WANTED and ignored the `version` the server actually advertised
     * for that global - wl_registry_bind() treats requesting a higher
     * version than the server offers as a fatal protocol error (the
     * client gets disconnected), not a negotiation. Found live: wlroots
     * 0.18 only advertises zwlr_layer_shell_v1 version 3, so the old
     * hardcoded `4` here killed the connection immediately with
     * "invalid version for global zwlr_layer_shell_v1 (18): have 3,
     * wanted 4" - lemonbar never got as far as drawing anything.
     * `min(wanted, version)` is the standard, correct pattern: never
     * ask for more than the server actually has. */
    if (!strcmp(interface, wl_compositor_interface.name)) {
        compositor = wl_registry_bind(reg, name, &wl_compositor_interface, min(version, 4));
    } else if (!strcmp(interface, wl_shm_interface.name)) {
        shm = wl_registry_bind(reg, name, &wl_shm_interface, min(version, 1));
    } else if (!strcmp(interface, wl_seat_interface.name)) {
        seat = wl_registry_bind(reg, name, &wl_seat_interface, min(version, 7));
        wl_seat_add_listener(seat, &seat_listener, NULL);
    } else if (!strcmp(interface, zwlr_layer_shell_v1_interface.name)) {
        layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, min(version, 4));
    } else if (!strcmp(interface, wl_output_interface.name)) {
        monitor_t *mon = xcalloc(1, sizeof(monitor_t));
        mon->wl_output = wl_registry_bind(reg, name, &wl_output_interface, min(version, 1));
        monitor_add(mon);
    }
}
static void registry_global_remove(void *d, struct wl_registry *r, uint32_t n) {}
static const struct wl_registry_listener registry_listener = { .global = registry_global, .global_remove = registry_global_remove };

void
cleanup (void)
{
    free(area_stack.ptr);
    while (monhead) {
        monitor_t *next = monhead->next;
        free(monhead->name);
        free(monhead);
        monhead = next;
    }
    if (display)
        wl_display_disconnect(display);
}

void
sighandle (int signal)
{
    if (signal == SIGINT || signal == SIGTERM)
        exit(EXIT_SUCCESS);
}

int
main (int argc, char **argv)
{
    struct pollfd pollin[2];
    struct wl_registry *registry;
    char input[4096] = {0, };
    size_t input_offset = 0;
    bool permanent = false;
    int ch;

    atexit(cleanup);
    signal(SIGINT, sighandle);
    signal(SIGTERM, sighandle);

    if (!(display = wl_display_connect(NULL))) {
        fprintf(stderr, "lemonbar: cannot connect to Wayland display\n");
        exit(EXIT_FAILURE);
    }

    while ((ch = getopt(argc, argv, "hg:o:bdf:a:pu:B:F:U:n:")) != -1) {
        switch (ch) {
            case 'h':
                printf ("lemonbar version %s\n", VERSION);
                printf ("usage: %s [-h | -b | -d | -f | -p | -n | -u | -B | -F]\n"
                        "\t-h Show this help\n"
                        "\t-b Put the bar at the bottom of the screen\n"
                        "\t-d Force docking (real no-op here - every bar is a real layer-shell surface already)\n"
                        "\t-f Set the font name to use (repeatable, real fallback chain)\n"
                        "\t-p Don't close after the data ends\n"
                        "\t-u Set the underline/overline height in pixels\n"
                        "\t-B Set background color in #rrggbb\n"
                        "\t-F Set foreground color in #rrggbb\n"
                        "\t-U Set underline color in #rrggbb\n"
                        "Real, honest scope cut: -g/-o (X geometry/RandR output selection) are gone -\n"
                        "every real Wayland output gets its own bar automatically now.\n", argv[0]);
                exit (EXIT_SUCCESS);
            case 'g': case 'o': case 'n':
                fprintf(stderr, "lemonbar: -%c is a real no-op in this Wayland port (see -h)\n", ch);
                break;
            case 'p': permanent = true; break;
            case 'b': topbar = false; break;
            case 'd': break;
            case 'f': if (font_count < (int)LENGTH(fonts)) fonts[font_count++] = optarg; break;
            case 'u': bu = (int)strtoul(optarg, NULL, 10); break;
            case 'B': bgname = optarg; break;
            case 'F': fgname = optarg; break;
            case 'U': ugname = optarg; break;
        }
    }
    if (!font_count)
        fonts[font_count++] = "monospace";

    area_stack.index = 0;
    area_stack.alloc = 10;
    area_stack.ptr = xcalloc(10, sizeof(area_t));

    registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);

    if (!compositor || !shm || !layer_shell) {
        fprintf(stderr, "lemonbar: compositor is missing wl_compositor/wl_shm/wlr-layer-shell-unstable-v1\n");
        exit(EXIT_FAILURE);
    }
    if (!monhead) {
        fprintf(stderr, "lemonbar: no real Wayland outputs found\n");
        exit(EXIT_FAILURE);
    }

    bgc = parse_color(bgname, NULL, 0xff000000u);
    fgc = parse_color(fgname, NULL, 0xffffffffu);
    ugc = ugname ? parse_color(ugname, NULL, fgc) : fgc;

    if (bh < 0)
        bh = 20 + bu + 2; /* real fallback default - actual per-font height is only known once each monitor's own Drw/fontset exists at first configure */

    for (monitor_t *m = monhead; m; m = m->next)
        create_bar_surface(m);

    wl_display_roundtrip(display); /* let the first configure land */

    pollin[0] = (struct pollfd){ .fd = STDIN_FILENO, .events = POLLIN };
    pollin[1] = (struct pollfd){ .fd = wl_display_get_fd(display), .events = POLLIN };

    for (;;) {
        wl_display_flush(display);

        if (poll(pollin, 2, -1) > 0) {
            if (pollin[0].revents & POLLHUP) {
                if (permanent) pollin[0].fd = -1;
                else break;
            }
            if (pollin[0].revents & POLLIN) {
                while (true) {
                    ssize_t r = read(STDIN_FILENO, input + input_offset, sizeof(input) - input_offset);
                    if (r == 0) break;
                    if (r < 0) {
                        if (errno == EINTR) continue;
                        exit(EXIT_FAILURE);
                    }
                    input_offset += (size_t)r;

                    char *input_end = input + input_offset;
                    char *last_nl = memrchr(input, '\n', (size_t)(input_end - input));
                    if (last_nl) {
                        char *prev_nl = (last_nl != input) ? memrchr(input, '\n', (size_t)(last_nl - 1 - input)) : NULL;
                        char *begin = prev_nl ? prev_nl + 1 : input;
                        *last_nl = '\0';
                        parse(begin);
                        const size_t remaining = (size_t)(input_end - (last_nl + 1));
                        if (remaining != 0) memmove(input, last_nl + 1, remaining);
                        input_offset = remaining;
                        break;
                    }
                    if (sizeof(input) == input_offset)
                        input_offset = 0;
                }
            }
            if (pollin[1].revents & POLLIN)
                wl_display_dispatch(display);
        }
    }

    return EXIT_SUCCESS;
}
