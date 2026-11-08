/* api.c - the hackable surface of the compositor: ONE command language (fifo, socket and key bindings all run it), a
 * settings registry described by data, one-line text events for subscribers, user hook scripts, and key bindings / window
 * rules / settings loaded from plain text files at runtime (`reload`). See docs/hacking.md; `desktop --dump-api` prints it all.
 * Part of the dwl compositor (dwl: https://codeberg.org/dwl/dwl, see src/LICENSES/). */
#include "dwl.h"
#include "audio.h"
#include <ctype.h>
#include <stdarg.h>
#include <strings.h>

#define SVALN 160
#define MAXSET 128
#define MAXWIN 256
#define MAXBIND 256
#define MAXRULE 128
#define MAXSUB 16

/* ---- config directory ---------------------------------------------------------------------------------------------- */
static const char *
cfgdir(void)
{
	static char d[512];
	const char *e = getenv("DESKTOP_CONFIG_DIR"), *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
	if (e && *e) snprintf(d, sizeof d, "%s", e);
	else if (x && *x) snprintf(d, sizeof d, "%s/desktop", x);
	else snprintf(d, sizeof d, "%s/.config/desktop", h ? h : "");
	return d;
}

static FILE *
cfgopen(const char *name)
{
	char p[640];
	snprintf(p, sizeof p, "%s/%s", cfgdir(), name);
	return fopen(p, "r");
}

static char *
trim(char *s)
{
	char *e;
	while (isspace((unsigned char)*s)) s++;
	e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
	return s;
}

/* ---- settings registry --------------------------------------------------------------------------------------------- */
enum { K_INT, K_BOOL, K_FLOAT, K_COLOR, K_STR, K_CHOICE, K_UINT };

typedef struct {
	const char *key;
	int kind;
	void *ptr;            /* int* (K_INT/K_BOOL/K_CHOICE), unsigned* (K_UINT), float* (K_FLOAT), float[4] (K_COLOR), NULL = buf */
	const char *extra;    /* "min:max[:step]" for numbers, "a,b,c" for choices */
	const char *apply;    /* live | restart | new (new outputs / new devices only) */
	const char *label;
	void (*after)(void);
	char def[SVALN], buf[SVALN];
	char dtype[8];        /* declared type of a schema-file setting: bool number text choice color */
	char keybuf[48], labelbuf[96], extrabuf[96], applybuf[16]; /* storage for settings declared in the schema file */
} Setting;

static Setting S[MAXSET];
static int nS;
static int def_layout = 0; /* index into layouts, as named by LAYNAMES */
static float def_mfact = 0.55f;
static int def_nmaster = 1;
static int api_loading; /* settings are being read from a file: no events, no hooks */

static const char *LAYNAMES = "tile,float,monocle,deck,grid,stack,tabbed";

static void
get_value(Setting *s, char *o, size_t n)
{
	switch (s->kind) {
	case K_INT: case K_BOOL: snprintf(o, n, "%d", *(int *)s->ptr); break;
	case K_UINT: snprintf(o, n, "%u", *(unsigned *)s->ptr); break;
	case K_FLOAT: snprintf(o, n, "%g", *(float *)s->ptr); break;
	case K_COLOR: {
		float *c = s->ptr;
		int a = (int)lroundf(c[3] * 255);
		if (a == 255) snprintf(o, n, "#%02x%02x%02x", (int)lroundf(c[0] * 255), (int)lroundf(c[1] * 255), (int)lroundf(c[2] * 255));
		else snprintf(o, n, "#%02x%02x%02x%02x", (int)lroundf(c[0] * 255), (int)lroundf(c[1] * 255), (int)lroundf(c[2] * 255), a);
		break;
	}
	case K_CHOICE: {
		const char *p = s->extra;
		int i = *(int *)s->ptr;
		while (i-- > 0 && p && (p = strchr(p, ','))) p++;
		if (!p) { o[0] = 0; break; }
		snprintf(o, n, "%.*s", (int)strcspn(p, ","), p);
		break;
	}
	default: snprintf(o, n, "%s", s->ptr ? (char *)s->ptr : s->buf);
	}
}

static int
parse_bool(const char *v, int *out)
{
	if (!strcmp(v, "1") || !strcasecmp(v, "on") || !strcasecmp(v, "true") || !strcasecmp(v, "yes")) *out = 1;
	else if (!strcmp(v, "0") || !strcasecmp(v, "off") || !strcasecmp(v, "false") || !strcasecmp(v, "no")) *out = 0;
	else return -1;
	return 0;
}

/* number range "min:max[:step]" -> lo, hi */
static void
range(const char *extra, double *lo, double *hi)
{
	*lo = -1e9; *hi = 1e9;
	if (extra && *extra) { char *e; *lo = strtod(extra, &e); if (*e == ':') *hi = strtod(e + 1, NULL); }
}

static int
set_value(Setting *s, const char *v, char *err, size_t en)
{
	double lo, hi, d;
	char *e;
	int b, i;
	switch (s->kind) {
	case K_BOOL:
		if (parse_bool(v, &b)) { snprintf(err, en, "%s wants 0/1/on/off", s->key); return -1; }
		*(int *)s->ptr = b;
		break;
	case K_INT: case K_UINT: case K_FLOAT:
		d = strtod(v, &e);
		range(s->extra, &lo, &hi);
		if (!*v || *e) { snprintf(err, en, "%s wants a number", s->key); return -1; }
		if (d < lo || d > hi) { snprintf(err, en, "%s must be %g..%g", s->key, lo, hi); return -1; }
		if (s->kind == K_INT) *(int *)s->ptr = (int)lround(d);
		else if (s->kind == K_UINT) *(unsigned *)s->ptr = (unsigned)lround(d);
		else *(float *)s->ptr = (float)d;
		break;
	case K_COLOR: {
		unsigned long x;
		size_t l = strlen(v);
		float *c = s->ptr;
		if (*v != '#' || (l != 7 && l != 9) || strspn(v + 1, "0123456789abcdefABCDEF") != l - 1) {
			snprintf(err, en, "%s wants #rrggbb or #rrggbbaa", s->key); return -1; }
		x = strtoul(v + 1, NULL, 16);
		if (l == 7) x = x << 8 | 0xff;
		c[0] = ((x >> 24) & 255) / 255.0f; c[1] = ((x >> 16) & 255) / 255.0f; c[2] = ((x >> 8) & 255) / 255.0f; c[3] = (x & 255) / 255.0f;
		break;
	}
	case K_CHOICE: {
		const char *p = s->extra;
		for (i = 0; p && *p; i++) {
			size_t n = strcspn(p, ",");
			if (strlen(v) == n && !strncmp(p, v, n)) { *(int *)s->ptr = i; goto ok; }
			p += n + (p[n] == ',');
		}
		snprintf(err, en, "%s must be one of %s", s->key, s->extra);
		return -1;
	}
	default:
		if (!strcmp(s->dtype, "bool")) {
			if (parse_bool(v, &b)) { snprintf(err, en, "%s wants 0/1/on/off", s->key); return -1; }
			v = b ? "1" : "0";
		} else if (!strcmp(s->dtype, "number")) {
			d = strtod(v, &e);
			range(s->extra, &lo, &hi);
			if (!*v || *e || d < lo || d > hi) { snprintf(err, en, "%s must be a number %g..%g", s->key, lo, hi); return -1; }
		} else if (!strcmp(s->dtype, "choice")) {
			const char *p = s->extra;
			for (; p && *p; p += strcspn(p, ",") + (p[strcspn(p, ",")] == ',')) {
				size_t n = strcspn(p, ",");
				if (strlen(v) == n && !strncmp(p, v, n)) goto store;
			}
			snprintf(err, en, "%s must be one of %s", s->key, s->extra);
			return -1;
		} else if (!strcmp(s->dtype, "color")) {
			size_t l = strlen(v);
			if (*v != '#' || (l != 7 && l != 9)) { snprintf(err, en, "%s wants #rrggbb", s->key); return -1; }
		}
store:
		snprintf(s->ptr ? (char *)s->ptr : s->buf, SVALN, "%s", v);
	}
ok:
	return 0;
}

static Setting *
find(const char *key)
{
	for (int i = 0; i < nS; i++)
		if (!strcmp(S[i].key, key)) return &S[i];
	return NULL;
}

static Setting *
reg(const char *key, int kind, void *ptr, const char *extra, const char *apply, const char *label, void (*after)(void))
{
	Setting *s;
	if (nS >= MAXSET || find(key)) return NULL;
	s = &S[nS++];
	s->key = key; s->kind = kind; s->ptr = ptr; s->extra = extra; s->apply = apply; s->label = label; s->after = after;
	get_value(s, s->def, sizeof s->def);
	return s;
}

/* live apply hooks */
static void
after_border(void)
{
	Client *c;
	Monitor *m;
	wl_list_for_each(c, &clients, link) {
		c->bw = c->isfullscreen ? 0 : borderpx;
		if (c->scene && c->border[0]) resize(c, c->geom, 0);
	}
	wl_list_for_each(m, &mons, link) arrange(m);
}

static void
after_colors(void)
{
	Client *c;
	wl_list_for_each(c, &clients, link)
		if (c->border[0])
			client_set_border_color(c, c->isurgent ? urgentcolor : c == focused_client ? focuscolor : bordercolor);
	if (root_bg) wlr_scene_rect_set_color(root_bg, rootcolor);
	{ Monitor *m; wl_list_for_each(m, &mons, link) tabs_refresh(m); }
}

static void
after_repeat(void)
{
	if (kb_group) wlr_keyboard_set_repeat_info(&kb_group->wlr_group->keyboard, repeat_rate, repeat_delay);
}

static void
after_layout(void)
{
	Monitor *m;
	if (def_layout < 0 || def_layout >= (int)nlayouts) return;
	if (!selmon) return;
	monsetlt(selmon, &layouts[def_layout], 0); /* only the focused monitor, only its shown workspace */
	arrange(selmon);
	printstatus();
}

static void
after_tabs(void)
{
	Monitor *m;
	wl_list_for_each(m, &mons, link) arrange(m);
}

static void
after_mfact(void)
{
	Monitor *m;
	if (!selmon) return;
	m = selmon; /* only the focused monitor */
	m->mfact = def_mfact; m->nmaster = def_nmaster; arrange(m);
}

static void
after_anim(void)
{
	if (!enable_animation) fx_snap_all();
}

static int bar_pos; /* 0 bottom, 1 top: read by the session script */
static char kb_layout[SVALN], kb_variant[SVALN], kb_options[SVALN];

static void
after_keymap(void)
{
	struct xkb_context *ctx;
	struct xkb_keymap *km;
	struct xkb_rule_names r = { .layout = kb_layout[0] ? kb_layout : NULL, .variant = kb_variant[0] ? kb_variant : NULL,
		.options = kb_options[0] ? kb_options : NULL };
	if (!kb_group) return;
	ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if ((km = xkb_keymap_new_from_names(ctx, &r, XKB_KEYMAP_COMPILE_NO_FLAGS))) {
		wlr_keyboard_set_keymap(&kb_group->wlr_group->keyboard, km);
		xkb_keymap_unref(km);
	} else
		fprintf(stderr, "desktop: keymap %s/%s/%s does not compile, kept the old one\n", kb_layout, kb_variant, kb_options);
	xkb_context_unref(ctx);
}

static void
regsettings(void)
{
	if (nS) return;
	/* appearance */
	reg("border_width", K_UINT, &borderpx, "0:20", "live", "Window border width (px)", after_border);
	reg("border_color", K_COLOR, bordercolor, NULL, "live", "Border colour, unfocused", after_colors);
	reg("focus_color", K_COLOR, focuscolor, NULL, "live", "Border colour, focused", after_colors);
	reg("urgent_color", K_COLOR, urgentcolor, NULL, "live", "Border colour, urgent", after_colors);
	reg("root_color", K_COLOR, rootcolor, NULL, "live", "Background colour", after_colors);
	/* tiling */
	reg("default_layout", K_CHOICE, &def_layout, LAYNAMES, "live", "Layout (all outputs, new ones too)", after_layout);
	reg("mfact", K_FLOAT, &def_mfact, "0.1:0.9:0.05", "live", "Master area share", after_mfact);
	reg("nmaster", K_INT, &def_nmaster, "0:10", "live", "Windows in the master area", after_mfact);
	/* behaviour */
	reg("sloppy_focus", K_BOOL, &sloppyfocus, NULL, "live", "Focus follows mouse", NULL);
	reg("snap", K_UINT, &snap, "0:200", "live", "Snap distance when dragging (px)", NULL);
	reg("tearing", K_BOOL, &tearing_allowed, NULL, "live", "Allow tearing for fullscreen games", NULL);
	reg("legacy_protocols", K_BOOL, &legacy_protocols, NULL, "restart", "Offer older wlr-* protocols too (screencopy, data-control, ...)", NULL);
	reg("adaptive_sync", K_BOOL, &adaptive_sync, NULL, "new", "Variable refresh rate (new outputs)", NULL);
	/* animation */
	reg("enable_animation", K_BOOL, &enable_animation, NULL, "live", "Spring animation of window movement", after_anim);
	reg("animation_stiffness", K_FLOAT, &animation_stiffness, "20:1000:10", "live", "Animation stiffness", NULL);
	reg("animation_damping", K_FLOAT, &animation_damping, "1:100:1", "live", "Animation damping", NULL);
	/* input */
	reg("repeat_rate", K_INT, &repeat_rate, "1:100", "live", "Key repeat rate (per s)", after_repeat);
	reg("repeat_delay", K_INT, &repeat_delay, "50:2000:50", "live", "Key repeat delay (ms)", after_repeat);
	reg("tap_to_click", K_BOOL, &tap_to_click, NULL, "live", "Touchpad tap to click", pointers_reconfigure);
	reg("tap_and_drag", K_BOOL, &tap_and_drag, NULL, "live", "Touchpad tap and drag", pointers_reconfigure);
	reg("natural_scrolling", K_BOOL, &natural_scrolling, NULL, "live", "Natural scrolling", pointers_reconfigure);
	reg("disable_while_typing", K_BOOL, &disable_while_typing, NULL, "live", "Touchpad off while typing", pointers_reconfigure);
	reg("left_handed", K_BOOL, &left_handed, NULL, "live", "Left-handed mouse", pointers_reconfigure);
	reg("kb_layout", K_STR, kb_layout, NULL, "live", "Keyboard layout (xkb, e.g. pl)", after_keymap);
	reg("kb_variant", K_STR, kb_variant, NULL, "live", "Keyboard variant", after_keymap);
	reg("kb_options", K_STR, kb_options, NULL, "live", "Keyboard options (e.g. ctrl:nocaps)", after_keymap);
	/* volume: the `volume` command and the panel (audio.c) */
	reg("volume_step", K_INT, &volume_step, "1:25", "live", "Volume step (percent per key press / wheel notch)", NULL);
	reg("volume_osd_ms", K_INT, &volume_osd_ms, "0:10000:100", "live", "Volume panel timeout after a change (ms, 0 = no panel)", NULL);
	reg("volume_max", K_INT, &volume_max, "100:150:5", "live", "Volume ceiling (percent)", NULL);
	/* bar and session: read by the `session` script and the bar, so they need a restart of the session */
	reg("bar_position", K_CHOICE, &bar_pos, "bottom,top", "restart", "Bar position", NULL);
	reg("bar_font", K_STR, NULL, NULL, "restart", "Bar font (fontconfig name)", NULL);
	reg("tab_bar_height", K_UINT, &tab_bar_height, "0:100", "live", "Tab bar height of tabbed / stack (px, 0 = no bar)", after_tabs);
	reg("tab_font", K_STR, tab_font, NULL, "live", "Tab bar font (fontconfig name)", after_tabs);
}

/* ---- events and hooks ---------------------------------------------------------------------------------------------- */
static int subs[MAXSUB], nsubs;

typedef struct { const char *type, *name, *fields; } EvDoc;
static const EvDoc evdocs[] = {
	{ "open",    "on-window-open",    "ID OUTPUT TAGS APPID TITLE...    a window appeared" },
	{ "close",   "on-window-close",   "ID APPID                         a window went away" },
	{ "focus",   "on-focus",          "ID APPID TITLE...                focus moved (ID 0 = nothing focused)" },
	{ "title",   "on-title-change",   "ID TITLE...                      a window changed its title" },
	{ "move",    "on-window-move",    "ID OUTPUT TAGS                   a window changed tags or output (TAGS is a bitmask)" },
	{ "float",   "on-float-change",   "ID 0|1                           a window became floating / tiled" },
	{ "urgent",  "on-urgent",         "ID APPID                         a window asked for attention" },
	{ "tag",     "on-tag-change",     "OUTPUT VIEW OCCUPIED URGENT      visible tags / occupied tags / urgent tags (bitmasks) of an output changed" },
	{ "layout",  "on-layout-change",  "OUTPUT SYMBOL NAME               the layout of an output changed" },
	{ "output",  "on-output",         "OUTPUT                           an output appeared" },
	{ "set",     "on-setting-change", "KEY VALUE                        a setting changed (set command, panel, reload)" },
	{ "reload",  "on-reload",         "                                 `reload` finished" },
	{ "user",    "on-user",           "NAME TEXT...                     sent by `emit NAME TEXT...`" },
	{ "start",   "on-start",          "(hook only)                      the compositor is up; the ctl socket works" },
};

typedef struct { const char *type, *out, *app, *title, *layout, *key, *val; int id; unsigned tags, occ, urg; int flag; } Ev;

static void
hook_run(const char *name, const char *line, const Ev *e)
{
	char path[700];
	pid_t pid;
	snprintf(path, sizeof path, "%s/hooks/%s", cfgdir(), name);
	if (access(path, X_OK))
		return;
	if ((pid = fork()) != 0)
		return;
	setsid();
	{
		char b[32];
		int nul = open("/dev/null", O_RDWR);
		dup2(nul, 0); dup2(2, 1);
		setenv("DESKTOP_EVENT", e->type, 1);
		setenv("DESKTOP_LINE", line, 1);
		snprintf(b, sizeof b, "%d", e->id); setenv("DESKTOP_WINDOW", b, 1);
		setenv("DESKTOP_APPID", e->app ? e->app : "", 1);
		setenv("DESKTOP_TITLE", e->title ? e->title : "", 1);
		setenv("DESKTOP_OUTPUT", e->out ? e->out : "", 1);
		snprintf(b, sizeof b, "%u", e->tags); setenv("DESKTOP_TAGS", b, 1);
		snprintf(b, sizeof b, "%u", e->occ); setenv("DESKTOP_OCCUPIED", b, 1);
		snprintf(b, sizeof b, "%u", e->urg); setenv("DESKTOP_URGENT", b, 1);
		setenv("DESKTOP_LAYOUT", e->layout ? e->layout : "", 1);
		setenv("DESKTOP_KEY", e->key ? e->key : "", 1);
		setenv("DESKTOP_VALUE", e->val ? e->val : "", 1);
		if (getenv("XDG_RUNTIME_DIR")) {
			snprintf(path, sizeof path, "%s/dwl.sock", getenv("XDG_RUNTIME_DIR"));
			setenv("DESKTOP_SOCKET", path, 1);
		}
	}
	snprintf(path, sizeof path, "%s/hooks/%s", cfgdir(), name);
	execl(path, path, (char *)NULL);
	_exit(127);
}

static void
clean(char *s)
{
	for (; *s; s++) if (*s == '\n' || *s == '\r') *s = ' ';
}

static void
emit(const Ev *e)
{
	char line[1024], app[96] = "", out[96] = "";
	int n = 0, k;
	if (api_loading) return;
	snprintf(app, sizeof app, "%s", e->app && *e->app ? e->app : "?");
	for (char *p = app; *p; p++) if (isspace((unsigned char)*p)) *p = '_';
	snprintf(out, sizeof out, "%s", e->out ? e->out : "");
	if (!strcmp(e->type, "open")) n = snprintf(line, sizeof line, "open %d %s %u %s %s", e->id, out, e->tags, app, e->title ? e->title : "");
	else if (!strcmp(e->type, "close")) n = snprintf(line, sizeof line, "close %d %s", e->id, app);
	else if (!strcmp(e->type, "focus")) n = e->id ? snprintf(line, sizeof line, "focus %d %s %s", e->id, app, e->title ? e->title : "") : snprintf(line, sizeof line, "focus 0");
	else if (!strcmp(e->type, "title")) n = snprintf(line, sizeof line, "title %d %s", e->id, e->title ? e->title : "");
	else if (!strcmp(e->type, "move")) n = snprintf(line, sizeof line, "move %d %s %u", e->id, out, e->tags);
	else if (!strcmp(e->type, "float")) n = snprintf(line, sizeof line, "float %d %d", e->id, e->flag);
	else if (!strcmp(e->type, "urgent")) n = snprintf(line, sizeof line, "urgent %d %s", e->id, app);
	else if (!strcmp(e->type, "tag")) n = snprintf(line, sizeof line, "tag %s %u %u %u", out, e->tags, e->occ, e->urg);
	else if (!strcmp(e->type, "layout")) n = snprintf(line, sizeof line, "layout %s %s %s", out, e->layout, e->key ? e->key : "");
	else if (!strcmp(e->type, "output")) n = snprintf(line, sizeof line, "output %s", out);
	else if (!strcmp(e->type, "set")) n = snprintf(line, sizeof line, "set %s %s", e->key, e->val);
	else if (!strcmp(e->type, "reload")) n = snprintf(line, sizeof line, "reload");
	else if (!strcmp(e->type, "user")) n = snprintf(line, sizeof line, "user %s %s", e->key, e->val ? e->val : "");
	else return;
	if (n >= (int)sizeof line - 2) n = sizeof line - 2;
	line[n] = 0;
	clean(line);
	line[n] = '\n'; line[n + 1] = 0;
	for (int i = 0; i < nsubs; ) {
		if (write(subs[i], line, n + 1) < 0 && errno != EINTR) { /* gone, or too slow to keep up: drop it */
			close(subs[i]); subs[i] = subs[--nsubs];
		} else i++;
	}
	line[n] = 0;
	for (k = 0; k < (int)(sizeof evdocs / sizeof *evdocs); k++)
		if (!strcmp(evdocs[k].type, e->type)) { hook_run(evdocs[k].name, line, e); break; }
}

/* ---- state snapshot: events come from diffing it against the compositor each time printstatus() runs ---------------- */
static struct Win { Client *c; int id, alive, urgent, floating; unsigned tags; char app[64], title[128]; Monitor *mon; } W[MAXWIN];
static int nW, nextid;
static struct Out { Monitor *m; unsigned view, occ, urg; char sym[16]; } O[8];
static int nO, last_focus = -1;

static const char *
layname(const char *sym)
{
	for (size_t i = 0; i < nlayouts; i++)
		if (!strcmp(layouts[i].symbol, sym)) { const char *p = LAYNAMES; for (size_t k = 0; k < i; k++) p = strchr(p, ',') + 1; static char b[16]; snprintf(b, sizeof b, "%.*s", (int)strcspn(p, ","), p); return b; }
	return "?";
}

static int
winid(Client *c)
{
	for (int i = 0; i < nW; i++) if (W[i].c == c) return W[i].id;
	return 0;
}

void
api_sync(void)
{
	Client *c;
	Monitor *m;
	int i, f = 0;
	if (api_loading) return;
	for (i = 0; i < nW; i++) W[i].alive = 0;
	wl_list_for_each(c, &clients, link) {
		struct Win *w = NULL;
		const char *app = client_get_appid(c), *title = client_get_title(c);
		Ev e = { .app = app, .title = title, .out = c->mon ? c->mon->wlr_output->name : "", .tags = c->tags };
		for (i = 0; i < nW; i++) if (W[i].c == c) { w = &W[i]; break; }
		if (!w) {
			if (nW >= MAXWIN) continue;
			w = &W[nW++];
			memset(w, 0, sizeof *w);
			w->c = c; w->id = ++nextid; w->mon = c->mon; w->tags = c->tags; w->floating = c->isfloating; w->urgent = c->isurgent;
			snprintf(w->app, sizeof w->app, "%s", app ? app : ""); snprintf(w->title, sizeof w->title, "%s", title ? title : "");
			e.type = "open"; e.id = w->id; emit(&e);
		} else {
			e.id = w->id;
			if (strcmp(w->title, title ? title : "")) { snprintf(w->title, sizeof w->title, "%s", title ? title : ""); e.type = "title"; emit(&e); }
			if (w->tags != c->tags || w->mon != c->mon) { w->tags = c->tags; w->mon = c->mon; e.type = "move"; emit(&e); }
			if (w->floating != c->isfloating) { w->floating = c->isfloating; e.type = "float"; e.flag = w->floating; emit(&e); }
			if (c->isurgent && !w->urgent) { e.type = "urgent"; emit(&e); }
			w->urgent = c->isurgent;
		}
		w->alive = 1;
	}
	for (i = 0; i < nW; ) {
		if (!W[i].alive) {
			Ev e = { .type = "close", .id = W[i].id, .app = W[i].app };
			emit(&e);
			W[i] = W[--nW];
		} else i++;
	}
	if (selmon && (c = focustop(selmon))) f = winid(c);
	if (f != last_focus) {
		Ev e = { .type = "focus", .id = f };
		last_focus = f;
		if (f) { e.app = client_get_appid(c); e.title = client_get_title(c); e.out = selmon->wlr_output->name; }
		emit(&e);
	}
	wl_list_for_each(m, &mons, link) {
		struct Out *o = NULL;
		unsigned occ = 0, urg = 0;
		Ev e = { .out = m->wlr_output->name };
		for (i = 0; i < nO; i++) if (O[i].m == m) o = &O[i];
		wl_list_for_each(c, &clients, link)
			if (c->mon == m) { occ |= c->tags; if (c->isurgent) urg |= c->tags; }
		if (!o) {
			if (nO >= 8) continue;
			o = &O[nO++];
			memset(o, 0, sizeof *o);
			o->m = m; o->view = m->tagset[m->seltags]; o->occ = occ; o->urg = urg;
			snprintf(o->sym, sizeof o->sym, "%s", m->ltsymbol);
			e.type = "output"; emit(&e);
			continue;
		}
		if (o->view != m->tagset[m->seltags] || o->occ != occ || o->urg != urg) {
			o->view = m->tagset[m->seltags]; o->occ = occ; o->urg = urg;
			e.type = "tag"; e.tags = o->view; e.occ = occ; e.urg = urg; emit(&e);
		}
		if (strcmp(o->sym, m->ltsymbol)) {
			snprintf(o->sym, sizeof o->sym, "%s", m->ltsymbol);
			e.type = "layout"; e.layout = m->ltsymbol; e.key = layname(m->ltsymbol); emit(&e);
		}
	}
}

/* ---- runtime key bindings and window rules ------------------------------------------------------------------------- */
static struct Bind { Key key; char cmd[256]; } B[MAXBIND];
static int nB;
static struct RRule { char app[64], title[64]; uint32_t tags; int float_, monitor, hasfloat, hasmon; } R[MAXRULE];
static int nR;
static int api_exec_line(char *line, FILE *out);

static void
runbind(const Arg *a)
{
	char line[256];
	FILE *null = fopen("/dev/null", "w");
	snprintf(line, sizeof line, "%s", (const char *)a->v);
	api_exec_line(line, null ? null : stderr);
	if (null) fclose(null);
}

static void
nop(const Arg *a)
{
}

static int
parse_chord(char *s, uint32_t *mod, xkb_keysym_t *sym)
{
	char *tok, *next;
	*mod = 0; *sym = XKB_KEY_NoSymbol;
	for (tok = s; tok; tok = next) {
		next = strchr(tok, '+');
		if (next && next[1]) *next++ = 0; else next = NULL;
		if (next) {
			if (!strcasecmp(tok, "super") || !strcasecmp(tok, "mod4") || !strcasecmp(tok, "logo") || !strcasecmp(tok, "win")) *mod |= WLR_MODIFIER_LOGO;
			else if (!strcasecmp(tok, "shift")) *mod |= WLR_MODIFIER_SHIFT;
			else if (!strcasecmp(tok, "ctrl") || !strcasecmp(tok, "control")) *mod |= WLR_MODIFIER_CTRL;
			else if (!strcasecmp(tok, "alt") || !strcasecmp(tok, "mod1")) *mod |= WLR_MODIFIER_ALT;
			else if (!strcasecmp(tok, "none")) ;
			else return -1;
		} else {
			*sym = xkb_keysym_from_name(tok, XKB_KEYSYM_NO_FLAGS);
			if (*sym == XKB_KEY_NoSymbol) *sym = xkb_keysym_from_name(tok, XKB_KEYSYM_CASE_INSENSITIVE);
		}
	}
	return *sym == XKB_KEY_NoSymbol ? -1 : 0;
}

static int
add_bind(char *chord, char *cmd, char *err, size_t en)
{
	uint32_t mod; xkb_keysym_t sym;
	char c2[96];
	snprintf(c2, sizeof c2, "%s", chord);
	if (parse_chord(c2, &mod, &sym)) { snprintf(err, en, "bad key '%s' (write Super+Shift+Return)", chord); return -1; }
	for (int i = 0; i < nB; i++)
		if (B[i].key.mod == mod && B[i].key.keysym == sym) { snprintf(B[i].cmd, sizeof B[i].cmd, "%s", cmd); B[i].key.func = strcmp(cmd, "none") ? runbind : nop; return 0; }
	if (nB >= MAXBIND) { snprintf(err, en, "too many bindings"); return -1; }
	snprintf(B[nB].cmd, sizeof B[nB].cmd, "%s", cmd);
	{
		Key k = { .mod = mod, .keysym = sym, .func = strcmp(cmd, "none") ? runbind : nop, .arg = { .v = B[nB].cmd } };
		memcpy(&B[nB].key, &k, sizeof k); /* Key.arg is const: build it whole */
	}
	nB++;
	return 0;
}

/* called by keybinding() before the compiled table: a runtime binding wins */
const Key *
api_keybinding(uint32_t mods, xkb_keysym_t sym)
{
	for (int i = 0; i < nB; i++)
		if (CLEANMASK(mods) == CLEANMASK(B[i].key.mod) && sym == B[i].key.keysym) return &B[i].key;
	return NULL;
}

/* rules.conf line: key=value pairs, keys app title tags float monitor; "#" starts a comment */
static void
add_rule(char *line)
{
	struct RRule *r;
	char *tok, *save = NULL;
	if (nR >= MAXRULE) return;
	r = &R[nR];
	memset(r, 0, sizeof *r);
	for (tok = strtok_r(line, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save)) {
		char *v = strchr(tok, '=');
		if (!v) return;
		*v++ = 0;
		if (!strcmp(tok, "app")) snprintf(r->app, sizeof r->app, "%s", v);
		else if (!strcmp(tok, "title")) snprintf(r->title, sizeof r->title, "%s", v);
		else if (!strcmp(tok, "tags")) { int n = atoi(v); if (n >= 1 && n <= TAGCOUNT) r->tags = 1u << (n - 1); }
		else if (!strcmp(tok, "float")) { int b; if (!parse_bool(v, &b)) { r->float_ = b; r->hasfloat = 1; } }
		else if (!strcmp(tok, "monitor")) { r->monitor = atoi(v); r->hasmon = 1; }
		else return;
	}
	if (r->app[0] || r->title[0]) nR++;
}

/* called by applyrules() after the compiled rules: runtime rules win */
void
api_rules(Client *c, const char *appid, const char *title, uint32_t *newtags, Monitor **mon)
{
	for (int i = 0; i < nR; i++) {
		struct RRule *r = &R[i];
		Monitor *m;
		int k = 0;
		if ((r->title[0] && !strstr(title ? title : "", r->title)) || (r->app[0] && !strstr(appid ? appid : "", r->app)))
			continue;
		if (r->hasfloat) c->isfloating = r->float_;
		if (r->tags) *newtags = r->tags;
		if (r->hasmon)
			wl_list_for_each(m, &mons, link)
				if (r->monitor == k++) *mon = m;
	}
}

/* ---- file loading -------------------------------------------------------------------------------------------------- */
static void
load_schema(void)
{
	FILE *f = cfgopen("settings.schema");
	char line[512];
	if (!f) return;
	/* key|type|extra|apply|default|label   type: bool number text choice color */
	while (fgets(line, sizeof line, f)) {
		char *p = trim(line), *fld[6] = {0};
		int n = 0, kind;
		Setting *s;
		if (!*p || *p == '#') continue;
		for (fld[n++] = p; n < 6 && (p = strchr(p, '|')); fld[n++] = ++p) *p = 0;
		if (n < 6 || find(trim(fld[0]))) continue;
		for (int i = 0; i < 6; i++) fld[i] = trim(fld[i]);
		kind = !strcmp(fld[1], "bool") ? K_BOOL : !strcmp(fld[1], "number") ? K_FLOAT : !strcmp(fld[1], "choice") ? K_CHOICE : !strcmp(fld[1], "color") ? K_COLOR : K_STR;
		if (nS >= MAXSET) break;
		s = &S[nS++];
		memset(s, 0, sizeof *s);
		snprintf(s->keybuf, sizeof s->keybuf, "%s", fld[0]); s->key = s->keybuf;
		snprintf(s->extrabuf, sizeof s->extrabuf, "%s", fld[2]); s->extra = s->extrabuf;
		snprintf(s->applybuf, sizeof s->applybuf, "%s", fld[3]); s->apply = s->applybuf;
		snprintf(s->labelbuf, sizeof s->labelbuf, "%s", fld[5]); s->label = s->labelbuf;
		/* storage of a user setting is a string whatever its type, so the compositor needs no code per setting */
		(void)kind;
		s->kind = K_STR; s->ptr = NULL;
		snprintf(s->dtype, sizeof s->dtype, "%s", fld[1]);
		snprintf(s->buf, sizeof s->buf, "%s", fld[4]);
		snprintf(s->def, sizeof s->def, "%s", fld[4]);
	}
	fclose(f);
}

static const char *
decl_type(Setting *s)
{
	switch (s->kind) {
	case K_BOOL: return "bool";
	case K_INT: case K_UINT: case K_FLOAT: return "number";
	case K_COLOR: return "color";
	case K_CHOICE: return "choice";
	}
	return s->dtype[0] ? s->dtype : "text";
}

static int
set_key(const char *key, const char *val, char *err, size_t en)
{
	Setting *s = find(key);
	if (!s) { /* unknown key: a free-form setting, kept and announced, so scripts can use it */
		if (nS >= MAXSET || !*key || strlen(key) >= sizeof s->keybuf || strspn(key, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.") != strlen(key)) {
			snprintf(err, en, "bad or too many keys"); return -1; }
		s = &S[nS++];
		memset(s, 0, sizeof *s);
		snprintf(s->keybuf, sizeof s->keybuf, "%s", key); s->key = s->keybuf; s->kind = K_STR; s->apply = "live"; s->label = "";
	}
	if (set_value(s, val, err, en)) return -1;
	if (s->after && !api_loading) s->after();
	if (!api_loading) {
		char cur[SVALN];
		Ev e = { .type = "set", .key = s->key, .val = cur };
		get_value(s, cur, sizeof cur);
		emit(&e);
	}
	return 0;
}

static void
load_settings(void)
{
	FILE *f = cfgopen("settings.conf");
	char line[512];
	if (!f) return;
	while (fgets(line, sizeof line, f)) {
		char *p = trim(line), *v, err[96];
		if (!*p || *p == '#' || !(v = strchr(p, '='))) continue;
		*v++ = 0;
		if (set_key(trim(p), trim(v), err, sizeof err))
			fprintf(stderr, "desktop: settings.conf: %s\n", err);
	}
	fclose(f);
}

static void
load_keys(void)
{
	FILE *f = cfgopen("keys.conf");
	char line[512], err[128];
	nB = 0;
	if (!f) return;
	/* "Super+Shift+Return = spawn foot"   "Super+q = none" (swallow the key) */
	while (fgets(line, sizeof line, f)) {
		char *p = trim(line), *v;
		if (!*p || *p == '#' || !(v = strchr(p, '='))) continue;
		*v++ = 0;
		if (add_bind(trim(p), trim(v), err, sizeof err)) fprintf(stderr, "desktop: keys.conf: %s\n", err);
	}
	fclose(f);
}

static void
load_rules(void)
{
	FILE *f = cfgopen("rules.conf");
	char line[512];
	nR = 0;
	if (!f) return;
	while (fgets(line, sizeof line, f)) {
		char *p = trim(line);
		if (*p && *p != '#') add_rule(p);
	}
	fclose(f);
}

static void
load_all(void)
{
	api_loading = 1;
	regsettings();
	load_schema();
	load_settings();
	load_keys();
	load_rules();
	api_loading = 0;
}

/* ---- commands ------------------------------------------------------------------------------------------------------ */
typedef struct { const char *name, *args, *help; void (*fn)(char *a, FILE *o); } Cmd;

static void err_(FILE *o, const char *fmt, ...)
{
	va_list ap;
	fputs("error: ", o);
	va_start(ap, fmt); vfprintf(o, fmt, ap); va_end(ap);
	fputc('\n', o);
}

static int
tagarg(const char *a, uint32_t *mask)
{
	int n = a ? atoi(a) : 0;
	if (a && !strcmp(a, "all")) { *mask = ~0u; return 0; }
	if (n < 1 || n > TAGCOUNT) return -1;
	*mask = 1u << (n - 1);
	return 0;
}

static void c_focus(char *a, FILE *o)
{
	Arg g = {0};
	if (!a || !*a || !strcmp(a, "next")) { g.i = 1; focusstack(&g); }
	else if (!strcmp(a, "prev")) { g.i = -1; focusstack(&g); }
	else if (!strcmp(a, "left") || !strcmp(a, "right") || !strcmp(a, "up") || !strcmp(a, "down")) {
		g.i = a[0] == 'l' ? WLR_DIRECTION_LEFT : a[0] == 'r' ? WLR_DIRECTION_RIGHT : a[0] == 'u' ? WLR_DIRECTION_UP : WLR_DIRECTION_DOWN;
		focusdir(&g);
	} else if (atoi(a) > 0) {
		int id = atoi(a);
		for (int i = 0; i < nW; i++)
			if (W[i].id == id) {
				Client *c = W[i].c;
				if (c->mon && !VISIBLEON(c, c->mon)) { selmon = c->mon; g.ui = c->tags; view(&g); }
				focusclient(c, 1);
				fputs("ok\n", o);
				return;
			}
		err_(o, "no window %d", id);
		return;
	} else { err_(o, "focus next|prev|left|right|up|down|ID"); return; }
	fputs("ok\n", o);
}
static void c_view(char *a, FILE *o)
{
	Arg g = {0};
	if (a && !strcmp(a, "prev")) { view(&g); fputs("ok\n", o); return; }
	if (tagarg(a, &g.ui)) { err_(o, "view N|all|prev (N is 1..%d)", TAGCOUNT); return; }
	view(&g); fputs("ok\n", o);
}
static void c_toggleview(char *a, FILE *o) { Arg g = {0}; if (tagarg(a, &g.ui)) { err_(o, "toggleview N"); return; } toggleview(&g); fputs("ok\n", o); }
static void c_tag(char *a, FILE *o) { Arg g = {0}; if (tagarg(a, &g.ui)) { err_(o, "tag N|all"); return; } tag(&g); fputs("ok\n", o); }
static void c_toggletag(char *a, FILE *o) { Arg g = {0}; if (tagarg(a, &g.ui)) { err_(o, "toggletag N"); return; } toggletag(&g); fputs("ok\n", o); }
static void c_layout(char *a, FILE *o)
{
	Arg g = {0};
	int n = -1;
	if (!a || !*a || !strcmp(a, "next")) { setlayout(&g); fputs("ok\n", o); return; }
	if (isdigit((unsigned char)*a)) n = atoi(a);
	else { const char *p = LAYNAMES; for (int i = 0; p && *p; i++) { size_t l = strcspn(p, ","); if (strlen(a) == l && !strncmp(a, p, l)) n = i; p += l + (p[l] == ','); } }
	if (n < 0 || n >= (int)nlayouts) { err_(o, "layout %s|next", LAYNAMES); return; }
	g.v = &layouts[n]; setlayout(&g); fputs("ok\n", o);
}
static void c_tab(char *a, FILE *o)
{
	if (!selmon || !tabs_layout(selmon)) { err_(o, "tab: the focused output is not in tabbed or stack layout"); return; }
	if (a && !strcmp(a, "next")) tabs_select(selmon, 1, -1);
	else if (a && !strcmp(a, "prev")) tabs_select(selmon, -1, -1);
	else if (a && isdigit((unsigned char)*a) && atoi(a) >= 1) tabs_select(selmon, 0, atoi(a) - 1);
	else { err_(o, "tab next|prev|N (N is 1-based)"); return; }
	fputs("ok\n", o);
}
static void c_mfact(char *a, FILE *o)
{
	Arg g = {0};
	char *e;
	double v = a ? strtod(a, &e) : 0;
	if (!a || !*a || *e) { err_(o, "mfact +0.05|-0.05|0.55"); return; }
	g.f = (a[0] == '+' || a[0] == '-') ? (float)v : (float)v + 1.0f;
	setmfact(&g); fputs("ok\n", o);
}
static void c_nmaster(char *a, FILE *o) { Arg g = { .i = a ? atoi(a) : 0 }; if (!g.i) { err_(o, "nmaster +1|-1"); return; } incnmaster(&g); fputs("ok\n", o); }
static void c_kill(char *a, FILE *o) { Arg g = {0}; killclient(&g); fputs("ok\n", o); }
static void c_float(char *a, FILE *o) { Arg g = {0}; togglefloating(&g); fputs("ok\n", o); }
static void c_fullscreen(char *a, FILE *o) { Arg g = {0}; togglefullscreen(&g); fputs("ok\n", o); }
static void c_zoom(char *a, FILE *o) { Arg g = {0}; zoom(&g); fputs("ok\n", o); }
static void c_focusmon(char *a, FILE *o) { Arg g = { .i = a && a[0] == 'l' ? WLR_DIRECTION_LEFT : WLR_DIRECTION_RIGHT }; focusmon(&g); fputs("ok\n", o); }
static void c_tagmon(char *a, FILE *o) { Arg g = { .i = a && a[0] == 'l' ? WLR_DIRECTION_LEFT : WLR_DIRECTION_RIGHT }; tagmon(&g); fputs("ok\n", o); }
static void c_quit(char *a, FILE *o) { Arg g = {0}; fputs("ok\n", o); quit(&g); }
static void c_spawn(char *a, FILE *o)
{
	const char *argv[] = { "/bin/sh", "-c", a, NULL };
	Arg g = { .v = argv };
	if (!a || !*a) { err_(o, "spawn SHELLCMD"); return; }
	spawn(&g); fputs("ok\n", o);
}
static void c_list(char *a, FILE *o)
{
	Client *c;
	Monitor *m;
	wl_list_for_each(m, &mons, link)
		wl_list_for_each(c, &clients, link) {
			if (c->mon != m) continue;
			fprintf(o, "%d %s %u %d %d %d geom %d %d %d %d shown %d %d moving %d\t%s\t%s\n", winid(c), m->wlr_output->name, c->tags,
				c->isfloating, c == focustop(m), c->isfullscreen, c->geom.x, c->geom.y, c->geom.width, c->geom.height,
				c->scene ? c->scene->node.x : 0, c->scene ? c->scene->node.y : 0, c->fx_moving,
				client_get_appid(c) ? client_get_appid(c) : "", client_get_title(c) ? client_get_title(c) : "");
		}
}
static void c_get(char *a, FILE *o)
{
	Setting *s = a ? find(a) : NULL;
	char v[SVALN];
	if (!s) { err_(o, "no setting '%s' (settings lists them)", a ? a : ""); return; }
	get_value(s, v, sizeof v);
	fprintf(o, "%s\n", v);
}
static void c_set(char *a, FILE *o)
{
	char *v, err[128];
	if (!a || !(v = strchr(a, ' '))) { err_(o, "set KEY VALUE"); return; }
	*v++ = 0;
	if (set_key(a, trim(v), err, sizeof err)) err_(o, "%s", err); else fputs("ok\n", o);
}
static void c_settings(char *a, FILE *o)
{
	char v[SVALN];
	for (int i = 0; i < nS; i++) { get_value(&S[i], v, sizeof v); fprintf(o, "%s=%s\n", S[i].key, v); }
}
/* key|type|extra|apply|default|current|label  - everything the settings panel needs, one line per setting */
static void c_schema(char *a, FILE *o)
{
	char v[SVALN], lab[96];
	for (int i = 0; i < nS; i++) {
		Setting *s = &S[i];
		snprintf(lab, sizeof lab, "%s", s->label && *s->label ? s->label : s->key);
		get_value(s, v, sizeof v);
		fprintf(o, "%s|%s|%s|%s|%s|%s|%s\n", s->key, decl_type(s), s->extra ? s->extra : "", s->apply ? s->apply : "live", s->def, v, lab);
	}
}
static void c_reload(char *a, FILE *o)
{
	Ev e = { .type = "reload" };
	load_all();
	emit(&e);
	fprintf(o, "ok %d bindings %d rules %d settings\n", nB, nR, nS);
}
static void c_bind(char *a, FILE *o)
{
	char *v, err[128];
	if (!a || !(v = strchr(a, '='))) { err_(o, "bind Super+Shift+t = COMMAND"); return; }
	*v++ = 0;
	if (add_bind(trim(a), trim(v), err, sizeof err)) err_(o, "%s", err); else fputs("ok\n", o);
}
static void c_unbind(char *a, FILE *o)
{
	char err[128];
	if (!a || add_bind(trim(a), "none", err, sizeof err)) { err_(o, "unbind Super+q"); return; }
	fputs("ok\n", o);
}
static void c_binds(char *a, FILE *o)
{
	for (int i = 0; i < nB; i++) fprintf(o, "mod %u sym %u = %s\n", B[i].key.mod, B[i].key.keysym, B[i].cmd);
}
static void c_emit(char *a, FILE *o)
{
	Ev e = { .type = "user" };
	char *v;
	if (!a || !*a) { err_(o, "emit NAME [TEXT]"); return; }
	if ((v = strchr(a, ' '))) *v++ = 0;
	e.key = a; e.val = v ? v : "";
	emit(&e); fputs("ok\n", o);
}
static void c_volume(char *a, FILE *o)
{
	char st[160];
	if (audio_command(a, o)) {
		Ev e = { .type = "user", .key = "volume", .val = st };
		audio_state(st, sizeof st);
		emit(&e);
		audio_osd();
	}
}
static void c_help(char *a, FILE *o);
static void c_events(char *a, FILE *o)
{
	for (size_t i = 0; i < sizeof evdocs / sizeof *evdocs; i++)
		if (strcmp(evdocs[i].type, "start")) fprintf(o, "%s %s\n", evdocs[i].type, evdocs[i].fields);
}

static const Cmd cmds[] = {
	{ "focus", "next|prev|left|right|up|down|ID", "move focus; ID is a window id from `list` or the open event", c_focus },
	{ "view", "N|all|prev", "show tag N (1..9) on the selected output; prev = the previous view", c_view },
	{ "toggleview", "N", "add or remove tag N from the view", c_toggleview },
	{ "tag", "N|all", "move the focused window to tag N", c_tag },
	{ "toggletag", "N", "toggle tag N on the focused window", c_toggletag },
	{ "tab", "next|prev|N", "tabbed / stack on the focused output: show the next / previous / Nth window", c_tab },
	{ "layout", "NAME|N|next", "tile float monocle deck grid stack tabbed (or 0..6); on the focused output and its shown workspace only; no argument = swap with the previous layout", c_layout },
	{ "mfact", "+0.05|-0.05|0.55", "master area share, relative or absolute", c_mfact },
	{ "nmaster", "+1|-1", "windows in the master area", c_nmaster },
	{ "kill", "", "close the focused window", c_kill },
	{ "float", "", "toggle floating of the focused window", c_float },
	{ "fullscreen", "", "toggle fullscreen of the focused window", c_fullscreen },
	{ "zoom", "", "swap the focused window with the master", c_zoom },
	{ "focusmon", "left|right", "focus the output to the left / right", c_focusmon },
	{ "tagmon", "left|right", "send the focused window to the output to the left / right", c_tagmon },
	{ "spawn", "SHELLCMD", "run a shell command", c_spawn },
	{ "quit", "", "leave the compositor", c_quit },
	{ "list", "", "one line per window: ID OUT TAGS FLOAT FOCUSED FULLSCREEN geom X Y W H shown X Y moving 0|1 <TAB>APPID<TAB>TITLE", c_list },
	{ "state", "", "JSON of every output and window (socket only)", NULL },
	{ "save", "PATH", "write the state JSON to PATH (socket only)", NULL },
	{ "restore", "PATH [nospawn]", "put windows back as saved in PATH (socket only)", NULL },
	{ "get", "KEY", "value of a setting", c_get },
	{ "set", "KEY VALUE", "change a setting now (live where the schema says so); unknown keys become free-form settings", c_set },
	{ "settings", "", "KEY=VALUE for every setting", c_settings },
	{ "schema", "", "KEY|TYPE|EXTRA|APPLY|DEFAULT|CURRENT|LABEL for every setting (what `desktop settings` draws)", c_schema },
	{ "reload", "", "re-read settings.schema, settings.conf, keys.conf, rules.conf", c_reload },
	{ "bind", "CHORD = COMMAND", "add or replace a key binding now (Super+Shift+t = spawn foot; COMMAND none swallows the key)", c_bind },
	{ "unbind", "CHORD", "swallow a key that is bound in the compiled table", c_unbind },
	{ "binds", "", "list runtime bindings", c_binds },
	{ "volume", "up|down|micup|micdown [PCT] | mute|micmute [on|off] | set spk|mic PCT | get | osd", "speaker / microphone volume and mute through wpctl (or the fake test backend); shows the volume panel; get prints `spk PCT MUTED mic PCT MUTED seq N osd MS step PCT max PCT`", c_volume },
	{ "emit", "NAME [TEXT]", "broadcast `user NAME TEXT` to subscribers and the on-user hook", c_emit },
	{ "subscribe", "", "keep the connection open: after `ok subscribed` every event arrives as one text line (socket only)", NULL },
	{ "events", "", "list the event lines", c_events },
	{ "commands", "", "list the commands", c_help },
};

static void
c_help(char *a, FILE *o)
{
	for (size_t i = 0; i < sizeof cmds / sizeof *cmds; i++) fprintf(o, "%s %s\t%s\n", cmds[i].name, cmds[i].args, cmds[i].help);
}

static int
api_exec_line(char *line, FILE *out)
{
	char *arg;
	line = trim(line);
	if (!*line) return 0;
	if ((arg = strchr(line, ' '))) { *arg++ = 0; arg = trim(arg); }
	for (size_t i = 0; i < sizeof cmds / sizeof *cmds; i++)
		if (!strcmp(line, cmds[i].name) && cmds[i].fn) { cmds[i].fn(arg, out); return 1; }
	err_(out, "unknown command '%s' (commands lists them)", line);
	return 0;
}

/* run one command line; the reply goes to `out`. Used by the fifo, the socket and key bindings */
void
api_exec(char *line, FILE *out)
{
	api_exec_line(line, out);
}

/* the socket handler hands the connection of a `subscribe` here and does not close it */
int
api_subscribe(int fd)
{
	if (nsubs >= MAXSUB) return -1;
	fd_set_nonblock(fd);
	subs[nsubs++] = fd;
	return 0;
}

void
api_init(void)
{
	load_all();
}

void
api_start(void)
{
	Ev e = { .type = "start" };
	hook_run("on-start", "start", &e);
}

/* defaults for new outputs, from createmon() */
void
api_monitor_defaults(Monitor *m)
{
	if (def_layout >= 0 && def_layout < (int)nlayouts) monsetlt(m, &layouts[def_layout], 1);
	if (def_mfact != 0.55f) m->mfact = def_mfact;
	if (def_nmaster != 1) m->nmaster = def_nmaster;
}

/* `desktop --dump-api`: everything a script can rely on */
void
dwl_dumpapi(void)
{
	char v[SVALN];
	regsettings();
	load_schema();
	puts("# desktop API  (ctl socket: $XDG_RUNTIME_DIR/dwl.sock, one request line -> reply; fifo: $XDG_RUNTIME_DIR/dwl.cmd, no reply)");
	puts("\n[commands]");
	c_help(NULL, stdout);
	puts("\n[events]   (subscribe, then one line each; the first word is the type)");
	c_events(NULL, stdout);
	puts("\n[hooks]    (executable files in ~/.config/desktop/hooks/, run in the background; env DESKTOP_EVENT DESKTOP_LINE DESKTOP_WINDOW");
	puts("           DESKTOP_APPID DESKTOP_TITLE DESKTOP_OUTPUT DESKTOP_TAGS DESKTOP_OCCUPIED DESKTOP_URGENT DESKTOP_LAYOUT DESKTOP_KEY DESKTOP_VALUE DESKTOP_SOCKET)");
	for (size_t i = 0; i < sizeof evdocs / sizeof *evdocs; i++) printf("%s  <- %s\n", evdocs[i].name, evdocs[i].type);
	puts("\n[settings]   key | type | extra | applies | default | label");
	for (int i = 0; i < nS; i++) {
		char lab[96];
		snprintf(lab, sizeof lab, "%s", S[i].label && *S[i].label ? S[i].label : S[i].key);
		get_value(&S[i], v, sizeof v);
		printf("%s | %s | %s | %s | %s | %s\n", S[i].key, decl_type(&S[i]), S[i].extra ? S[i].extra : "", S[i].apply, S[i].def, lab);
	}
	puts("\n[files]  (in ~/.config/desktop/, or $DESKTOP_CONFIG_DIR)");
	puts("settings.schema  extra settings, one per line:  key|bool/number/text/choice/color|extra|live/restart|default|label");
	puts("settings.conf    key = value   (written by `desktop settings`, read at start and on reload)");
	puts("keys.conf        Super+Shift+Return = spawn foot     (any command above; `none` swallows the key)");
	puts("rules.conf       app=firefox title=Private tags=3 float=1 monitor=0   (all keys optional but app or title)");
	puts("hooks/           on-start on-window-open ... (see [hooks])");
	puts("bar.conf         segments of the status bar (see docs/hacking.md)");
}
