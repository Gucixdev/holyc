/* demo.c - the gallery, written as DATA + EVENTS: the screen is one indented spec (SPEC below), the values live in the app's model, and the
 * program is a set of handlers for named events. Two tabs (main, extras), a dmenu-style button bar on top, every widget in its own titled
 * panel, scrolling pages, mouse everywhere. `demo --snapshot [W H [tab]]` prints one frame as plain text (no tty needed). */
#include "lazytui.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NPROC 8
#define HIST 10
#define GHIST 200
#define NBLIP 7
#define NDOCS 320
enum { DT_TIMER, DT_DATE, DT_TIMEF, DT_DATEC, DT_TIME12, DT_TIMESP, DT_N };   /* which datetime.c control the keyboard reaches, in the combined panel */

typedef struct { const char *name; double cpu, mem, net, hist[HIST]; int nh; } proc;


typedef struct {
	lt_app *app;
	int theme_i;
	double time, acc, interval, hold;
	bool paused;
	/* springs */
	lt_anim anim;
	double z[3], web[6];
	int a_z[3], a_web[6];
	double gcpu[GHIST], gmem[GHIST]; int ng;
	proc procs[NPROC];
	lt_blip blips[NBLIP]; double blip_rate[NBLIP];
	double pdone, ptotal, prate;
	/* search */
	lt_doc docs[NDOCS]; char *doctext[NDOCS], *doctags[NDOCS]; int ndocs;
	/* the countries on the maps */
	int sel;                                   /* selected country index, -1 none */
	int mapmode;                               /* 0 climate, 1 political, 2 population heat, 3 GDP heat */
	bool labels, borders, cities;
	lt_heat_point *pop, *gdp; int ncountries;
	char info[160];
	/* the newer widgets (theme gallery + editors) */
	lt_pixeleditor *pe;
	lt_nodeeditor *ne;
	lt_gradientpicker *gp;
	lt_colorpicker *cpk;
	lt_timer *timer;
	lt_datepicker *datepick;
	lt_datepicker *datepick_compact;
	lt_timepicker *timepick;
	lt_timepicker *timepick_h12;
	lt_timepicker *timepick_spin;
	lt_sequencer *seq;
	int dt_focus;
	lt_rect dt_rect[DT_N];
	double tagw[12];
	double heat[20 * 7];                       /* a GitHub-contributions-style grid, HCOLS x HROWS */
	double histsample[300];
} S;

#define HCOLS 20
#define HROWS 7
static const char *TAGWORDS[] = {"net", "system", "ui", "shell", "hardware", "log", "disk", "pkg", "config", "media", "power", "security"};
#define NTAGWORDS ((int)(sizeof TAGWORDS / sizeof TAGWORDS[0]))

/* ------------------------------------------------------------------ the screen, as data */
static const char *SPEC =
	"col\n"
	"  row size=1\n"
	"    buttonbar id=tabs group=top items=\"main,extras,new\" active=tab size=26\n"
	"    buttonbar id=actions group=top items=\"Refresh,Theme,Pause,Quit\" prompt=\"lazytui\" size=fill\n"
	"  pages bind=tab\n"
	"    scroll id=page_main\n"
	"      panel title=\"bars\" size=11\n"
	"        progressbar name=progressbar namew=14 bind=progress size=1\n"
	"        ringbar name=ringbar namew=14 bind=ringtext size=1\n"
	"        zonebar name=zonebar namew=14 legend=1 zones=\"used=z_used:err,cache=z_cache:warn,free=z_free:ok\" size=4\n"
	"        pacmanbar name=pacmanbar namew=14 size=1\n"
	"        dropdown id=theme name=dropdown namew=14 items=\"%THEMES%\" bind=theme_i size=1\n"
	"        dropdown id=country name=\"tree dropdown\" namew=14 tree=1 bind=country_node size=1\n"
	"      panel title=\"search (type tag:x / filter: / regex: / mcts: right in the box - no mode picker)\" size=17\n"
	"        searchbar id=q name=search namew=14 prompt=\"> \" bind=query size=1\n"
	"        text bind=searchstatus style=dim size=1\n"
	"        list id=results\n"
	"      row size=13\n"
	"        panel title=\"graph\"\n"
	"          custom type=graph nofocus=1\n"
	"        panel title=\"processes\"\n"
	"          custom type=table nofocus=1\n"
	"      row size=13\n"
	"        panel title=\"tree\"\n"
	"          treeview id=tree\n"
	"        panel title=\"selection\"\n"
	"          text bind=info\n"
	"      panel title=\"breadcrumbs\" size=3\n"
	"        breadcrumbs id=bc bind=bc_node\n"
	"    scroll id=page_extras\n"
	"      panel title=\"world map (wheel zoom, drag, click a country)\" size=36\n"
	"        custom id=map type=map\n"
	"      row size=26\n"
	"        panel title=\"heatmap\"\n"
	"          custom id=heatmap type=heatmap\n"
	"        panel title=\"greymap\"\n"
	"          custom id=greymap type=greymap\n"
	"      row size=26\n"
	"        panel title=\"wiremap\"\n"
	"          custom id=wiremap type=wiremap\n"
	"        panel title=\"globe\"\n"
	"          custom type=globe nofocus=1 animated=1\n"
	"      row size=24\n"
	"        panel title=\"globe (wireframe)\"\n"
	"          custom type=globewire nofocus=1 animated=1\n"
	"        panel title=\"radar\"\n"
	"          custom type=radar nofocus=1 animated=1\n"
	"      row size=18\n"
	"        panel title=\"webchart (n=6)\"\n"
	"          custom type=webchart nofocus=1\n"
	"        panel title=\"rain\"\n"
	"          rain seed=7\n"
	"      row size=14\n"
	"        panel title=\"trichart (webchart, n=3)\"\n"
	"          custom type=trichart nofocus=1\n"
	"        panel title=\"pentachart (webchart, n=5)\"\n"
	"          custom type=pentachart nofocus=1\n"
	"    scroll id=page_new\n"
	"      panel title=\"spinners\" size=7\n"
	"        custom type=spinners nofocus=1 animated=1\n"
	"      panel title=\"pie chart\" size=12\n"
	"        custom type=piechart nofocus=1\n"
	"      panel title=\"gauges\" size=10\n"
	"        custom type=gauges nofocus=1\n"
	"      row size=14\n"
	"        panel title=\"line chart\"\n"
	"          custom type=linechart nofocus=1\n"
	"        panel title=\"bar chart\"\n"
	"          custom type=barchart nofocus=1\n"
	"      row size=14\n"
	"        panel title=\"histogram\"\n"
	"          custom type=histogram nofocus=1\n"
	"        panel title=\"heatgrid\"\n"
	"          custom type=heatgrid nofocus=1\n"
	"      panel title=\"tag cloud\" size=6\n"
	"        custom type=tagcloud nofocus=1\n"
	"      panel title=\"minimap\" size=8\n"
	"        custom type=minimap nofocus=1 animated=1\n"
	"      row size=16\n"
	"        panel title=\"pixel editor\"\n"
	"          custom id=pixedit type=pixedit\n"
	"        panel title=\"node editor\"\n"
	"          custom id=nodedit type=nodedit\n"
	"      row size=12\n"
	"        panel title=\"gradient picker (Tab cycles, Del removes, r resets)\"\n"
	"          custom id=gradpick type=gradpick\n"
	"        panel title=\"colour picker (feeds the gradient stop above, r resets)\"\n"
	"          custom id=colorpick type=colorpick\n"
	"      panel title=\"date & time (every widget from datetime.c, one panel: Tab/S-Tab cycles which control the keyboard reaches)\" size=28\n"
	"        custom id=dtall type=dtall animated=1\n"
	"      panel title=\"step sequencer (click a step, or arrows+Enter; r clears it)\" size=7\n"
	"        custom id=seq type=seq animated=1\n"
	"  keybar hints=\"wheel:scroll;Tab:mode/focus;S-Tab:focus;f1/f2/f3:tab;+/-:refresh rate;ctrl-t:theme;esc:quit\" size=2\n";

/* ------------------------------------------------------------------ data */
static double frand(void) { return (double)rand() / RAND_MAX; }
static double walk(double v, double step, double lo, double hi) { v += (frand() - 0.5) * 2 * step; return v < lo ? lo : v > hi ? hi : v; }

/* SPEC has a %THEMES% placeholder: the theme dropdown lists whatever the library ships */
static char *spec_with_themes(void)
{
	size_t cap = strlen(SPEC) + 1024;
	char *out = malloc(cap), names[1024] = "";
	for (int i = 0; i < lt_theme_count(); i++) { strcat(names, i ? "," : ""); strcat(names, lt_theme_name(i)); }
	const char *at = strstr(SPEC, "%THEMES%");
	snprintf(out, cap, "%.*s%s%s", (int)(at - SPEC), SPEC, names, at + 8);
	return out;
}

static lt_theme theme_of(int i) { return lt_theme_at(i); }

static void refresh(S *s, bool animate)
{
	lt_app *a = s->app;
	lt_app_invalidate(a);
	if (animate) {
		for (int i = 0; i < 3; i++) lt_anim_to(&s->anim, s->a_z[i], 5 + frand() * 60);
		for (int i = 0; i < 6; i++) lt_anim_to(&s->anim, s->a_web[i], walk(s->web[i], 40, 8, 100));
	}
	double cpu = walk(s->ng ? s->gcpu[s->ng - 1] : 40, 22, 2, 98), mem = walk(s->ng ? s->gmem[s->ng - 1] : 50, 8, 10, 95);
	if (s->ng == GHIST) { memmove(s->gcpu, s->gcpu + 1, (GHIST - 1) * sizeof(double)); memmove(s->gmem, s->gmem + 1, (GHIST - 1) * sizeof(double)); s->ng--; }
	s->gcpu[s->ng] = cpu; s->gmem[s->ng] = mem; s->ng++;
	for (int i = 0; i < NPROC; i++) {
		proc *p = &s->procs[i];
		p->cpu = walk(p->cpu, i == 0 ? 25 : 8, 0, 100);
		p->mem = walk(p->mem, 1.5, 0.1, 60);
		p->net = walk(p->net, 120, 0, 2000);
		if (p->nh == HIST) { memmove(p->hist, p->hist + 1, (HIST - 1) * sizeof(double)); p->nh--; }
		p->hist[p->nh++] = p->cpu;
	}
	char b[160];
	snprintf(b, sizeof b, "LAZYTUI // refresh %.2fs // theme %s // frame %.2f ms // text flows through the whole bar", s->interval, lt_theme_name(s->theme_i), lt_app_ui(a) ? lt_ui_frame_ms(lt_app_ui(a)) : 0.0);
	lt_app_set_text(a, "ringtext", b);
}

static void make_docs(S *s)
{
	static const char *verbs[] = {"open", "close", "restart", "edit", "show", "list", "update", "remove", "start", "stop", "toggle", "reset", "monitor", "backup", "restore", "configure"};
	static const struct { const char *obj, *tags; } objs[] = {{"network manager", "net system"}, {"terminal window", "ui shell"}, {"bluetooth devices", "net hardware"}, {"kernel log", "system log"},
		{"btrfs snapshot", "disk system"}, {"package database", "pkg system"}, {"font size", "ui config"}, {"wallpaper", "ui media"}, {"keyboard layout", "hardware config"},
		{"display resolution", "hardware ui"}, {"audio output", "hardware media"}, {"battery status", "hardware power"}, {"ssh keys", "net security"}, {"firewall rules", "net security"},
		{"cron jobs", "system shell"}, {"clipboard history", "ui shell"}, {"screenshot tool", "ui media"}, {"gpu driver", "hardware system"}, {"swap file", "disk system"}, {"docker containers", "system net"}};
	s->ndocs = 0;
	for (size_t o = 0; o < sizeof objs / sizeof objs[0] && s->ndocs < NDOCS; o++)
		for (size_t v = 0; v < sizeof verbs / sizeof verbs[0] && s->ndocs < NDOCS; v++) {
			char b[96];
			snprintf(b, sizeof b, "%s %s", verbs[v], objs[o].obj);
			s->doctext[s->ndocs] = strdup(b);
			s->doctags[s->ndocs] = strdup(objs[o].tags);
			s->docs[s->ndocs] = (lt_doc){s->doctext[s->ndocs], s->doctags[s->ndocs]};
			s->ndocs++;
		}
}

static void run_search(S *s)
{
	lt_app *a = s->app;
	lt_list *l = lt_app_list(a, "results");
	int out[NDOCS];
	lt_search_mode mode;
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	int n = lt_search_auto(lt_app_text(a, "query"), s->docs, s->ndocs, out, NDOCS, &mode);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	lt_list_set_order(l, out, n);
	char b[96];
	snprintf(b, sizeof b, "%d of %d   mode %s   %.2f ms   (try tag:net, -tag:ui, filter:, regex:^, mcts:)", n, s->ndocs, lt_search_mode_name(mode), (double)(t1.tv_nsec - t0.tv_nsec) / 1e6 + (double)(t1.tv_sec - t0.tv_sec) * 1e3);
	lt_app_set_text(a, "searchstatus", b);
}

static void fill_lists(S *s)
{
	lt_app *a = s->app;
	lt_list *l = lt_app_list(a, "results");
	for (int i = 0; i < s->ndocs; i++) lt_list_add(l, s->docs[i].text, i);
	lt_tree *t = lt_app_tree(a, "tree");
	int src = lt_tree_add(t, -1, "src", 1), core = lt_tree_add(t, src, "core", 2);
	lt_tree_add(t, core, "solver.c", 3); lt_tree_add(t, core, "layout.c", 4); lt_tree_add(t, core, "anim.c", 5);
	int w = lt_tree_add(t, src, "widgets", 6);
	lt_tree_add(t, w, "list.c", 7); lt_tree_add(t, w, "tree.c", 8); lt_tree_add(t, w, "dropdown.c", 9); lt_tree_add(t, w, "globe.c", 10); lt_tree_add(t, w, "app.c", 11);
	int te = lt_tree_add(t, -1, "tests", 12);
	lt_tree_add(t, te, "test_solver.c", 13); lt_tree_add(t, te, "test_layout.c", 14); lt_tree_add(t, te, "test_app.c", 15);
	int ex = lt_tree_add(t, -1, "examples", 16);
	lt_tree_add(t, ex, "demo.c", 17);
	lt_tree_add(t, -1, "DESIGN.md", 18);
	lt_tree_expand_all(t, true);
	/* the same real layout again, for the standalone breadcrumbs widget below (its own lt_tree - kinds don't share one) */
	lt_tree *bt = lt_app_tree(a, "bc");
	int bsrc = lt_tree_add(bt, -1, "src", 1), bcore = lt_tree_add(bt, bsrc, "core", 2);
	lt_tree_add(bt, bcore, "solver.c", 3); lt_tree_add(bt, bcore, "layout.c", 4); lt_tree_add(bt, bcore, "anim.c", 5);
	int bw = lt_tree_add(bt, bsrc, "widgets", 6);
	lt_tree_add(bt, bw, "list.c", 7); lt_tree_add(bt, bw, "tree.c", 8); lt_tree_add(bt, bw, "nav.c", 9);
	int bte = lt_tree_add(bt, -1, "tests", 10);
	lt_tree_add(bt, bte, "test_solver.c", 11); lt_tree_add(bt, bte, "test_kinds.c", 12);
	int bex = lt_tree_add(bt, -1, "examples", 13);
	lt_tree_add(bt, bex, "demo.c", 14);
	/* the dropdown tree: continents, then their countries */
	lt_tree *ct = lt_app_tree(a, "country");
	int nc = lt_country_count();
	static const char *conts[] = {"Africa", "Antarctica", "Asia", "Europe", "North America", "Oceania", "South America"};
	int cn[7];
	for (int i = 0; i < 7; i++) cn[i] = lt_tree_add(ct, -1, conts[i], -1);
	for (int i = 0; i < nc; i++) {
		lt_country info;
		lt_country_info(i, &info);
		for (int k = 0; k < 7; k++) if (!strcmp(info.continent, conts[k])) { lt_tree_add(ct, cn[k], info.name_pl, i); break; }
	}
	s->ncountries = nc;
	s->pop = calloc((size_t)nc, sizeof *s->pop);
	s->gdp = calloc((size_t)nc, sizeof *s->gdp);
	for (int i = 0; i < nc; i++) { lt_country info; lt_country_info(i, &info); s->pop[i] = (lt_heat_point){info.iso, info.population_m}; s->gdp[i] = (lt_heat_point){info.iso, info.gdp_bn}; }
}

static void update_info(S *s)
{
	lt_country ci;
	if (s->sel >= 0 && lt_country_info(s->sel, &ci))
		snprintf(s->info, sizeof s->info, "%s / %s (%s)  %s  %.1f M people  GDP %.0f bn USD", ci.name_pl, ci.name_en, ci.iso, ci.continent, ci.population_m, ci.gdp_bn);
	else snprintf(s->info, sizeof s->info, "no country selected - click one on the map (extras tab) or pick it in the tree dropdown");
	lt_app_set_text(s->app, "info", s->info);
}

/* ------------------------------------------------------------------ custom widgets */
static lt_color green(const lt_theme *t) { return t->s[LT_T_TEXT].fg != LT_DEFAULT ? t->s[LT_T_TEXT].fg : LT_PAL(2); }

static void map_opts(S *s, lt_map_opts *o, int flags)
{
	memset(o, 0, sizeof *o);
	o->flags = flags | (s->borders ? LT_MAP_BORDERS : 0) | (s->labels ? LT_MAP_LABELS : 0) | (s->cities ? LT_MAP_CITIES : 0);
	lt_country ci;
	o->selected = s->sel >= 0 && lt_country_info(s->sel, &ci) ? ci.iso : NULL;
	if (s->mapmode == 1) o->flags |= LT_MAP_POLITICAL;
	if (s->mapmode == 2) { o->heat = s->pop; o->nheat = s->ncountries; o->heat_lo = 0; o->heat_hi = 200; }
	if (s->mapmode == 3) { o->heat = s->gdp; o->nheat = s->ncountries; o->heat_lo = 0; o->heat_hi = 3000; }
}


static void draw_graph(lt_app *a, const char *id, lt_canvas *c, lt_rect r, const lt_theme *t, bool f, void *ctx) { (void)a; (void)id; (void)f; S *s = ctx; lt_graph(c, r, t, s->gcpu, s->ng, 0, 100, LT_T_OK); }

static void draw_table(lt_app *a, const char *id, lt_canvas *c, lt_rect in, const lt_theme *t, bool f, void *ctx)
{
	(void)a; (void)id; (void)f;
	S *s = ctx;
	lt_rect all = {0, 0, lt_canvas_w(c), lt_canvas_h(c)};
	char row[160];
	int nw = in.w > 70 ? 20 : in.w > 50 ? 14 : 8;
	snprintf(row, sizeof row, "%-*s %6s %6s %8s  history", nw, "process", "cpu", "mem", "net");
	lt_text(c, all, in.x, in.y, row, t->s[LT_T_ACCENT], in.w);
	for (int i = 0; i < NPROC && i + 1 < in.h; i++) {
		proc *p = &s->procs[i];
		snprintf(row, sizeof row, "%-*.*s %5.1f%% %5.1f%% %6.0fK/s", nw, nw, p->name, p->cpu, p->mem, p->net);
		lt_text(c, all, in.x, in.y + 1 + i, row, t->s[LT_T_TEXT], in.w);
		int hx = in.x + nw + 1 + 6 + 1 + 6 + 1 + 8 + 2 + 1;
		if (hx + 4 < in.x + in.w) lt_sparkline(c, (lt_rect){hx, in.y + 1 + i, in.x + in.w - hx, 1}, t, p->hist, p->nh, 0, 100, p->cpu > 50 ? LT_T_WARN : LT_T_OK);
	}
}

/* lt_webchart already takes an arbitrary axis count - a "trichart" is just n=3, a pentagon is n=5, this hexagon is n=6. Three panels
 * below show each shape side by side rather than adding a redundant new widget for a number that's already a parameter. */
static void draw_trichart(lt_app *a, const char *id, lt_canvas *c, lt_rect r, const lt_theme *t, bool f, void *ctx)
{
	(void)a; (void)id; (void)f;
	S *s = ctx;
	lt_axis ax[3] = {{"CPU", s->web[0]}, {"MEM", s->web[1]}, {"NET", s->web[2]}};
	lt_webchart(c, r, t, ax, 3, 100, 4, LT_DEFAULT);
}
static void draw_pentachart(lt_app *a, const char *id, lt_canvas *c, lt_rect r, const lt_theme *t, bool f, void *ctx)
{
	(void)a; (void)id; (void)f;
	S *s = ctx;
	lt_axis ax[5] = {{"CPU", s->web[0]}, {"MEM", s->web[1]}, {"NET", s->web[2]}, {"DISK", s->web[3]}, {"GPU", s->web[4]}};
	lt_webchart(c, r, t, ax, 5, 100, 4, LT_DEFAULT);
}
static void draw_webchart(lt_app *a, const char *id, lt_canvas *c, lt_rect r, const lt_theme *t, bool f, void *ctx)
{
	(void)a; (void)id; (void)f;
	S *s = ctx;
	lt_axis ax[6] = {{"CPU", s->web[0]}, {"MEM", s->web[1]}, {"NET", s->web[2]}, {"DISK", s->web[3]}, {"GPU", s->web[4]}, {"TEMP", s->web[5]}};
	lt_webchart(c, r, t, ax, 6, 100, 4, LT_DEFAULT);
}
static void draw_radar(lt_app *a, const char *id, lt_canvas *c, lt_rect r, const lt_theme *t, bool f, void *ctx) { (void)a; (void)id; (void)f; S *s = ctx; lt_radar(c, r, t, s->time * 100.0, s->blips, NBLIP, LT_DEFAULT); }

static void draw_globe(lt_app *a, const char *id, lt_canvas *c, lt_rect r, const lt_theme *t, bool f, void *ctx)
{
	(void)a; (void)id; (void)f;
	S *s = ctx;
	lt_map_opts o;
	map_opts(s, &o, 0);
	o.lon0 = s->time * 14.0 + 20.0; o.tilt = 23.4;
	lt_globe_ex(c, r, &o);
	int col, row;
	if (lt_globe_project(r, o.lon0, o.tilt, 21.0, 52.2, &col, &row)) lt_put(c, (lt_rect){0, 0, lt_canvas_w(c), lt_canvas_h(c)}, col, row, lt_canvas_caps(c)->unicode ? 0x25C9 : 'O', LT_STYLE(LT_RGB(255, 60, 60), LT_DEFAULT, LT_BOLD));
	(void)t;
}
static void draw_globewire(lt_app *a, const char *id, lt_canvas *c, lt_rect r, const lt_theme *t, bool f, void *ctx)
{
	(void)a; (void)id; (void)f;
	S *s = ctx;
	lt_map_opts o;
	map_opts(s, &o, LT_MAP_WIRE);
	o.lon0 = s->time * 14.0 + 20.0; o.tilt = 23.4; o.line = green(t);
	lt_globe_ex(c, r, &o);
}
/* the four zoomable maps: each has its own view (zoom / centre), kind 0 = the main map, 1 heat, 2 grey, 3 wire */
enum { MAP_MAIN, MAP_HEAT, MAP_GREY, MAP_WIRE };
typedef struct { S *s; lt_mapview v; int kind; int heat_gdp; } mapw;
static mapw MAPW[4];
static const char *MODES[] = {"climate", "political", "heat: population", "heat: GDP", "grey", "wire"};

static void mapw_opts(mapw *m, lt_map_opts *o, const lt_theme *t)
{
	S *s = m->s;
	int flags = 0;
	switch (m->kind) {
	case MAP_HEAT: break;
	case MAP_GREY: flags = LT_MAP_GREY; break;
	case MAP_WIRE: flags = LT_MAP_WIRE | LT_MAP_GRID; break;
	default: flags = s->mapmode == 1 ? LT_MAP_POLITICAL : s->mapmode == 4 ? LT_MAP_GREY : s->mapmode == 5 ? LT_MAP_WIRE | LT_MAP_GRID : 0;
	}
	memset(o, 0, sizeof *o);
	o->flags = flags | (s->borders ? LT_MAP_BORDERS : 0) | (s->labels ? LT_MAP_LABELS : 0) | (s->cities ? LT_MAP_CITIES : 0);
	lt_country ci;
	o->selected = s->sel >= 0 && lt_country_info(s->sel, &ci) ? ci.iso : NULL;
	bool gdp = m->kind == MAP_MAIN ? s->mapmode == 3 : m->heat_gdp;
	if (m->kind == MAP_HEAT || (m->kind == MAP_MAIN && (s->mapmode == 2 || s->mapmode == 3))) {
		o->heat = gdp ? s->gdp : s->pop; o->nheat = s->ncountries; o->heat_lo = 0; o->heat_hi = gdp ? 3000 : 200;
	}
	o->line = (o->flags & LT_MAP_WIRE) ? green(t) : LT_DEFAULT;
	lt_mapview_apply(&m->v, o);
}

static void draw_mapw(lt_app *a, const char *id, lt_canvas *c, lt_rect r, const lt_theme *t, bool focused, void *ctx)
{
	(void)a; (void)id;
	mapw *m = ctx;
	S *s = m->s;
	lt_map_opts o;
	mapw_opts(m, &o, t);
	lt_rect mapr = {r.x, r.y, r.w, r.h - 1};
	lt_worldmap_ex(c, mapr, &o);
	lt_rect all = {0, 0, lt_canvas_w(c), lt_canvas_h(c)};
	char b[400];
	const char *what = m->kind == MAP_MAIN ? MODES[s->mapmode] : m->kind == MAP_HEAT ? (m->heat_gdp ? MODES[3] : MODES[2]) : m->kind == MAP_GREY ? "grey" : "wire";
	snprintf(b, sizeof b, "zoom %.1fx | %s%s | %s", m->v.zoom, what, focused ? " | wheel zoom, drag pan, 0 reset, m mode, l/b/c" : " | click to focus", s->info);
	lt_fill(c, (lt_rect){r.x, r.y + r.h - 1, r.w, 1}, ' ', t->s[LT_T_TEXT]);
	lt_text(c, all, r.x, r.y + r.h - 1, b, t->s[LT_T_DIM], r.w);
}

static lt_wres event_mapw(lt_app *a, const char *id, lt_rect r, const lt_event *ev, void *ctx)
{
	(void)id;
	mapw *m = ctx;
	S *s = m->s;
	lt_rect mapr = {r.x, r.y, r.w, r.h - 1};
	lt_map_opts o = {0};
	lt_wres w = lt_mapview_event(&m->v, mapr, &o, ev);
	if (w == LT_W_ACTIVATE) {
		s->sel = m->v.country;
		update_info(s);
		lt_app_set(a, "country_sel", s->sel);
		return LT_W_CHANGED;
	}
	if (w != LT_W_IGNORED) return w;
	if (ev->kind == LT_EV_KEY) {
		switch (ev->key) {
		case 'm': case 'h':
			if (m->kind == MAP_MAIN) s->mapmode = (s->mapmode + 1) % 6; else m->heat_gdp = !m->heat_gdp;
			return LT_W_CHANGED;
		case 'l': s->labels = !s->labels; return LT_W_CHANGED;
		case 'b': s->borders = !s->borders; return LT_W_CHANGED;
		case 'c': s->cities = !s->cities; return LT_W_CHANGED;
		}
	}
	return LT_W_IGNORED;
}


/* ------------------------------------------------------------------ the newer widgets (theme gallery + editors) */
static void draw_spinners(lt_app *a, const char *id, lt_canvas *c, lt_rect r, const lt_theme *t, bool f, void *ctx)
{
	(void)a; (void)id; (void)f;
	S *s = ctx;
	static const char *names[5] = {"line", "dots", "arc", "wave", "bounce"};
	lt_rect all = {0, 0, lt_canvas_w(c), lt_canvas_h(c)};
	for (int i = 0; i < 5 && i < r.h; i++)                                      /* parallax left out: it wants its own tall panel to read well, not one line */
	{
		lt_text(c, all, r.x, r.y + i, names[i], t->s[LT_T_DIM], 8);
		lt_spinner(c, (lt_rect){r.x + 8, r.y + i, r.w - 8, 1}, t, (lt_spin_kind)i, s->time, NULL);
	}
}
static void draw_piechart(lt_app *a, const char *id, lt_canvas *c, lt_rect r, const lt_theme *t, bool f, void *ctx)
{ (void)a; (void)id; (void)f; S *s = ctx; lt_slice sl[3] = {{"used", s->z[0], LT_DEFAULT}, {"cache", s->z[1], LT_DEFAULT}, {"free", s->z[2], LT_DEFAULT}}; lt_piechart(c, r, t, sl, 3, true, true); }
static void draw_gauges(lt_app *a, const char *id, lt_canvas *c, lt_rect r, const lt_theme *t, bool f, void *ctx)
{
	(void)a; (void)id; (void)f;
	S *s = ctx;
	double cpu = s->ng ? s->gcpu[s->ng - 1] / 100.0 : 0, mem = s->ng ? s->gmem[s->ng - 1] / 100.0 : 0;
	int half = r.w / 2;
	lt_gauge(c, (lt_rect){r.x, r.y, half, r.h}, t, cpu, "CPU", false);
	lt_gauge(c, (lt_rect){r.x + half, r.y, r.w - half, r.h}, t, mem, "MEM", true);
}
static void draw_linechart(lt_app *a, const char *id, lt_canvas *c, lt_rect r, const lt_theme *t, bool f, void *ctx)
{
	(void)a; (void)id; (void)f;
	S *s = ctx;
	lt_series ss[2] = {{"cpu", s->gcpu, s->ng, LT_DEFAULT}, {"mem", s->gmem, s->ng, LT_DEFAULT}};
	lt_chart_opts o = {0};
	o.title = "history"; o.grid = true; o.legend = true;
	o.autoscale = false; o.ymin = 0; o.ymax = 100;                              /* both series are percentages: a fixed 0..100 axis reads much better than one that rescales every frame */
	lt_linechart(c, r, t, ss, 2, &o);
}
static void draw_barchart(lt_app *a, const char *id, lt_canvas *c, lt_rect r, const lt_theme *t, bool f, void *ctx)
{
	(void)a; (void)id; (void)f;
	S *s = ctx;
	double vals[NPROC];
	const char *labels[NPROC];
	for (int i = 0; i < NPROC; i++) { vals[i] = s->procs[i].cpu; labels[i] = s->procs[i].name; }
	lt_chart_opts o = {0};
	o.values = true;
	lt_barchart(c, r, t, labels, vals, NPROC, &o);
}
static void draw_histogram(lt_app *a, const char *id, lt_canvas *c, lt_rect r, const lt_theme *t, bool f, void *ctx)
{ (void)a; (void)id; (void)f; S *s = ctx; lt_histogram(c, r, t, s->histsample, 300, 12, 0, 100, NULL); }
static void draw_heatgrid(lt_app *a, const char *id, lt_canvas *c, lt_rect r, const lt_theme *t, bool f, void *ctx)
{
	(void)a; (void)id; (void)f;
	S *s = ctx;
	static const char *const dow[7] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
	lt_heatgrid(c, r, t, s->heat, HCOLS, HROWS, 0, 8, 2, dow);
}
static void draw_tagcloud(lt_app *a, const char *id, lt_canvas *c, lt_rect r, const lt_theme *t, bool f, void *ctx)
{ (void)a; (void)id; (void)f; S *s = ctx; lt_tagcloud(c, r, t, TAGWORDS, s->tagw, NTAGWORDS); }
static void draw_minimap(lt_app *a, const char *id, lt_canvas *c, lt_rect r, const lt_theme *t, bool f, void *ctx)
{
	/* an overview of the whole 320-doc search corpus, not cpu noise: density is each entry's text length (a real code-editor minimap
	 * shows the same thing - longer lines, more ink), marks are every "net"-tagged doc, and the viewport slowly pans across it. */
	(void)a; (void)id; (void)f;
	S *s = ctx;
	if (s->ndocs == 0) return;
	uint8_t density[NDOCS];
	int lo = 1000, hi = 0;
	for (int i = 0; i < s->ndocs; i++) { int len = (int)strlen(s->doctext[i]); if (len < lo) lo = len; if (len > hi) hi = len; }
	for (int i = 0; i < s->ndocs; i++) {
		int len = (int)strlen(s->doctext[i]);
		density[i] = hi > lo ? (uint8_t)(60 + (len - lo) * 195 / (hi - lo)) : 128;
	}
	int marks[NDOCS], nm = 0;
	for (int i = 0; i < s->ndocs; i++) if (strstr(s->doctags[i], "net")) marks[nm++] = i;
	int page = s->ndocs > 24 ? 24 : s->ndocs;
	int span = s->ndocs - page;
	int offset = span > 0 ? (int)fmod(s->time * 6.0, (double)span) : 0;
	lt_minimap(c, r, t, density, s->ndocs, offset, page, marks, nm);
}
static void draw_pixedit(lt_app *a, const char *id, lt_canvas *c, lt_rect r, const lt_theme *t, bool focused, void *ctx)
{
	(void)a; (void)id;
	S *s = ctx;
	lt_rect body = {r.x, r.y, r.w, r.h - 1}, all = {0, 0, lt_canvas_w(c), lt_canvas_h(c)};
	lt_pixeleditor_draw(s->pe, c, body, t, focused);
	lt_fill(c, (lt_rect){r.x, r.y + r.h - 1, r.w, 1}, ' ', t->s[LT_T_TEXT]);
	lt_text(c, all, r.x, r.y + r.h - 1, "click a tool on the left, then paint with the mouse (drag) or arrows+Enter; r resets", t->s[LT_T_DIM], r.w);
}
static lt_wres event_pixedit(lt_app *a, const char *id, lt_rect r, const lt_event *ev, void *ctx) { (void)a; (void)id; S *s = ctx; lt_rect body = {r.x, r.y, r.w, r.h - 1}; return lt_pixeleditor_event(s->pe, body, ev); }
static void draw_nodedit(lt_app *a, const char *id, lt_canvas *c, lt_rect r, const lt_theme *t, bool focused, void *ctx)
{
	(void)a; (void)id;
	S *s = ctx;
	lt_rect body = {r.x, r.y, r.w, r.h - 1}, all = {0, 0, lt_canvas_w(c), lt_canvas_h(c)};
	lt_nodeeditor_draw(s->ne, c, body, t, focused);
	lt_fill(c, (lt_rect){r.x, r.y + r.h - 1, r.w, 1}, ' ', t->s[LT_T_TEXT]);
	char hint[96];
	if (lt_nodeeditor_connecting(s->ne)) snprintf(hint, sizeof hint, "click the target node ('c'/Esc to cancel)");
	else snprintf(hint, sizeof hint, "drag: move/pan | wheel/+-: zoom (%.1fx) | c: connect | d: delete | 0: reset", lt_nodeeditor_zoom(s->ne));
	lt_text(c, all, r.x, r.y + r.h - 1, hint, t->s[LT_T_DIM], r.w);
}
static lt_wres event_nodedit(lt_app *a, const char *id, lt_rect r, const lt_event *ev, void *ctx) { (void)a; (void)id; S *s = ctx; lt_rect body = {r.x, r.y, r.w, r.h - 1}; return lt_nodeeditor_event(s->ne, body, ev); }
static void draw_gradpick(lt_app *a, const char *id, lt_canvas *c, lt_rect r, const lt_theme *t, bool focused, void *ctx) { (void)a; (void)id; S *s = ctx; lt_gradientpicker_draw(s->gp, c, r, t, focused); }
static lt_wres event_gradpick(lt_app *a, const char *id, lt_rect r, const lt_event *ev, void *ctx) { (void)a; (void)id; S *s = ctx; return lt_gradientpicker_event(s->gp, r, ev); }
static void draw_colorpick(lt_app *a, const char *id, lt_canvas *c, lt_rect r, const lt_theme *t, bool focused, void *ctx) { (void)a; (void)id; S *s = ctx; lt_colorpicker_draw(s->cpk, c, r, t, focused); }
static lt_wres event_colorpick(lt_app *a, const char *id, lt_rect r, const lt_event *ev, void *ctx)
{
	(void)a; (void)id;
	S *s = ctx;
	lt_wres res = lt_colorpicker_event(s->cpk, r, ev);
	if (res == LT_W_CHANGED) lt_gradientpicker_set_color(s->gp, lt_colorpicker_color(s->cpk));            /* the picker feeds whichever gradient stop is selected */
	return res;
}
/* every datetime.c widget, in the one panel the demo gives them: this custom widget lays out clock/timer/date/time itself and runs its
 * own tiny Tab-cycled focus among the interactive pieces (dt_focus), since the app layer only ever focuses this whole node as one unit. */
static bool pt_in(lt_rect r, int x, int y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

static void draw_dtall(lt_app *a, const char *id, lt_canvas *c, lt_rect r, const lt_theme *t, bool focused, void *ctx)
{
	(void)a; (void)id;
	S *s = ctx;
	lt_rect all = {0, 0, lt_canvas_w(c), lt_canvas_h(c)};
	time_t nowsecs = time(NULL);
	struct tm when = *localtime(&nowsecs);
	int y = r.y;

	int digw = r.w / 6, bigw = r.w * 2 / 6;                    /* the big font needs the most room; give it the least-cramped share */
	lt_clock_draw(c, (lt_rect){r.x, y, digw, 1}, t, LT_CLOCK_DIGITAL, when, true);
	lt_clock_draw(c, (lt_rect){r.x + digw, y, bigw, 5}, t, LT_CLOCK_BIG, when, false);
	lt_clock_draw(c, (lt_rect){r.x + digw + bigw + 1, y, r.w - digw - bigw - 1, 5}, t, LT_CLOCK_ANALOG, when, false);
	y += 6;

	s->dt_rect[DT_TIMER] = (lt_rect){r.x, y, r.w, 1};
	lt_timer_draw(s->timer, c, s->dt_rect[DT_TIMER], t, focused && s->dt_focus == DT_TIMER, s->time);
	y += 2;

	int halfw = r.w / 2;
	s->dt_rect[DT_DATE] = (lt_rect){r.x, y, halfw - 1, 9};
	lt_datepicker_draw(s->datepick, c, s->dt_rect[DT_DATE], t, focused && s->dt_focus == DT_DATE);
	s->dt_rect[DT_TIMEF] = (lt_rect){r.x + halfw, y, r.w - halfw, 1};
	lt_timepicker_draw(s->timepick, c, s->dt_rect[DT_TIMEF], t, focused && s->dt_focus == DT_TIMEF);
	y += 10;

	int thirdw = r.w / 3;
	s->dt_rect[DT_DATEC] = (lt_rect){r.x, y, thirdw - 1, 1};
	lt_datepicker_draw(s->datepick_compact, c, s->dt_rect[DT_DATEC], t, focused && s->dt_focus == DT_DATEC);
	s->dt_rect[DT_TIME12] = (lt_rect){r.x + thirdw, y, thirdw - 1, 1};
	lt_timepicker_draw(s->timepick_h12, c, s->dt_rect[DT_TIME12], t, focused && s->dt_focus == DT_TIME12);
	s->dt_rect[DT_TIMESP] = (lt_rect){r.x + 2 * thirdw, y, r.w - 2 * thirdw, 3};
	lt_timepicker_draw(s->timepick_spin, c, s->dt_rect[DT_TIMESP], t, focused && s->dt_focus == DT_TIMESP);
	y += 4;

	if (focused && y < r.y + r.h) lt_text(c, all, r.x, y, "Tab/S-Tab: cycle which control below responds to the keyboard", t->s[LT_T_DIM], r.w);
}

static lt_wres event_dtall(lt_app *a, const char *id, lt_rect r, const lt_event *ev, void *ctx)
{
	(void)a; (void)id; (void)r;
	S *s = ctx;
	int base = ev->key & 0xFFFFFF;
	if (ev->kind == LT_EV_KEY && !(ev->key & (LT_MOD_CTRL | LT_MOD_ALT))) {
		if (base == LT_K_TAB) { s->dt_focus = (s->dt_focus + 1) % DT_N; return LT_W_HANDLED; }
		if (base == LT_K_BTAB) { s->dt_focus = (s->dt_focus + DT_N - 1) % DT_N; return LT_W_HANDLED; }
	}
	if (ev->kind == LT_EV_MOUSE && base == LT_K_CLICK)
		for (int i = 0; i < DT_N; i++) if (pt_in(s->dt_rect[i], ev->x, ev->y)) { s->dt_focus = i; break; }
	lt_rect target = s->dt_rect[s->dt_focus];
	switch (s->dt_focus) {
	case DT_TIMER: return lt_timer_event(s->timer, target, ev, s->time);
	case DT_DATE: return lt_datepicker_event(s->datepick, target, ev);
	case DT_TIMEF: return lt_timepicker_event(s->timepick, target, ev);
	case DT_DATEC: return lt_datepicker_event(s->datepick_compact, target, ev);
	case DT_TIME12: return lt_timepicker_event(s->timepick_h12, target, ev);
	case DT_TIMESP: return lt_timepicker_event(s->timepick_spin, target, ev);
	}
	return LT_W_IGNORED;
}
static void draw_seq(lt_app *a, const char *id, lt_canvas *c, lt_rect r, const lt_theme *t, bool focused, void *ctx)
{
	(void)a; (void)id;
	S *s = ctx;
	lt_rect body = {r.x, r.y, r.w, r.h - 1}, all = {0, 0, lt_canvas_w(c), lt_canvas_h(c)};
	lt_sequencer_set_playhead(s->seq, (int)(s->time * 8.0) % lt_sequencer_steps(s->seq));   /* 8 steps/s: this pattern plays back at 120 BPM */
	lt_sequencer_draw(s->seq, c, body, t, focused);
	lt_fill(c, (lt_rect){r.x, r.y + r.h - 1, r.w, 1}, ' ', t->s[LT_T_TEXT]);
	lt_text(c, all, r.x, r.y + r.h - 1, "click a step, or arrows+Enter; r clears the pattern (no audio - lazytui only draws the grid)", t->s[LT_T_DIM], r.w);
}
static lt_wres event_seq(lt_app *a, const char *id, lt_rect r, const lt_event *ev, void *ctx) { (void)a; (void)id; S *s = ctx; lt_rect body = {r.x, r.y, r.w, r.h - 1}; return lt_sequencer_event(s->seq, body, ev); }

/* ------------------------------------------------------------------ event handlers */
static void set_theme(S *s, int i)
{
	s->theme_i = ((i % lt_theme_count()) + lt_theme_count()) % lt_theme_count();
	lt_app_set(s->app, "theme_i", s->theme_i);
	if (lt_app_ui(s->app)) lt_ui_set_theme(lt_app_ui(s->app), theme_of(s->theme_i));
	refresh(s, false);
}

static void on_app(lt_app *a, const lt_appevent *ev, void *ctx)
{
	S *s = ctx;
	const char *k = ev->kind;
	if (!strcmp(k, "tick")) {
		if (s->paused) return;
		double dt = ev->number;
		s->time += dt;
		if (lt_app_number(a, "progress") < 1.0) { double p = lt_app_number(a, "progress") + dt / 14.0; if (p >= 1.0) { p = 1.0; s->hold = 0.9; } lt_app_set(a, "progress", p); }
		else if ((s->hold -= dt) <= 0) lt_app_set(a, "progress", 0);               /* the progress bar loads 0..100 steadily, holds, starts over */
		s->prate = 6.5 * 1048576.0 * (0.85 + 0.15 * sin(s->time * 0.7));
		s->pdone += s->prate * dt;
		if (s->pdone >= s->ptotal) s->pdone = 0;
		lt_app_set(a, "done", s->pdone); lt_app_set(a, "total", s->ptotal); lt_app_set(a, "rate", s->prate);
		lt_anim_tick(&s->anim, dt);
		lt_app_set(a, "z_used", s->z[0]); lt_app_set(a, "z_cache", s->z[1]); lt_app_set(a, "z_free", s->z[2]);
		for (int i = 0; i < NBLIP; i++) { s->blips[i].bearing = fmod(s->blips[i].bearing + s->blip_rate[i] * dt + 360, 360); s->blips[i].range = fmin(0.95, fmax(0.15, s->blips[i].range + 0.02 * sin(s->time * 0.3 + i) * dt)); }
		s->acc += dt;
		if (s->acc >= s->interval) { s->acc = 0; refresh(s, true); }
	} else if (!strcmp(k, "theme")) set_theme(s, s->theme_i + 1);
	else if (!strcmp(k, "pause")) s->paused = !s->paused;
	else if (!strcmp(k, "refresh")) refresh(s, true);
	else if (!strcmp(k, "faster")) { s->interval = fmax(0.1, s->interval * 0.7); }
	else if (!strcmp(k, "slower")) { s->interval = fmin(5.0, s->interval * 1.4); }
	else if (!strcmp(k, "page0")) lt_app_set(a, "tab", 0);
	else if (!strcmp(k, "page1")) lt_app_set(a, "tab", 1);
	else if (!strcmp(k, "page2")) lt_app_set(a, "tab", 2);
	else if (!strcmp(k, "escape")) { if (*lt_app_focused(a)) lt_app_focus(a, ""); else lt_app_quit(a); }
	else if (!strcmp(k, "quit")) lt_app_quit(a);
}

static void on_widget(lt_app *a, const lt_appevent *ev, void *ctx)
{
	S *s = ctx;
	const char *id = ev->id, *k = ev->kind;
	if (!strcmp(id, "actions") && !strcmp(k, "activate")) {
		lt_appevent e = *ev;
		static const char *names[] = {"refresh", "theme", "pause", "quit"};
		if (ev->index >= 0 && ev->index < 4) { e.id = "app"; e.kind = names[ev->index]; on_app(a, &e, s); }
	} else if (!strcmp(id, "theme") && !strcmp(k, "change")) set_theme(s, ev->index);
	else if (!strcmp(id, "q") && !strcmp(k, "change")) run_search(s);
	else if (!strcmp(id, "country") && !strcmp(k, "change")) {
		lt_tree *t = lt_app_tree(a, "country");
		int cid = lt_tree_id(t, ev->index);
		if (cid >= 0) { s->sel = cid; update_info(s); }
	} else if (!strcmp(id, "keys") && !strcmp(k, "activate")) {
		static const char *hint_events[] = {NULL, NULL, "page1", NULL, "theme", "escape"};
		(void)hint_events;
	}
}

static void init_data(S *s)
{
	static const char *names[NPROC] = {"Hyprland", "plymouthd", "Godot_v4.7.2-st", "foot", "bsc", "pipewire-pulse", "kworker/u48:3", "systemd"};
	for (int i = 0; i < NPROC; i++) { s->procs[i].name = names[i]; s->procs[i].cpu = 5 + frand() * 40; s->procs[i].mem = 1 + frand() * 20; s->procs[i].net = frand() * 300; }
	for (int i = 0; i < 6; i++) s->web[i] = 30 + frand() * 60;
	static const char *tags[NBLIP] = {"ALFA", "B2", "C", "D", "E", "F", NULL};
	for (int i = 0; i < NBLIP; i++) { s->blips[i] = (lt_blip){frand() * 360, 0.25 + frand() * 0.65, tags[i]}; s->blip_rate[i] = (frand() - 0.5) * 6; }
	s->z[0] = 40; s->z[1] = 35; s->z[2] = 25;
	s->ptotal = 137.0 * 1048576.0; s->pdone = 21.0 * 1048576.0; s->prate = 6.5 * 1048576.0;
	s->interval = 0.8; s->sel = -1; s->labels = true; s->borders = true;
	lt_anim_init(&s->anim);
	for (int i = 0; i < 3; i++) s->a_z[i] = lt_anim_bind(&s->anim, &s->z[i], lt_spring_preset(s->z[i], "firm"));      /* stiff and critically damped: no bounce */
	for (int i = 0; i < 6; i++) s->a_web[i] = lt_anim_bind(&s->anim, &s->web[i], lt_spring_preset(s->web[i], "smooth"));
	for (int i = 0; i < 60; i++) refresh(s, false);
	make_docs(s);
	for (int i = 0; i < 300; i++) s->histsample[i] = frand() * frand() * 100;                 /* skewed low, like a real latency sample */
	{                                                                            /* a believable weekly rhythm (a GitHub-style commit graph), not pure noise: quiet Mondays, a
	                                                                              * midweek peak, weekends tapering off, the occasional day off and the occasional big push */
		static const double weekday_base[7] = {1.0, 4.0, 5.5, 6.0, 5.5, 4.5, 1.5};
		for (int row = 0; row < HROWS; row++)
			for (int col = 0; col < HCOLS; col++) {
				double v = weekday_base[row] + (frand() - 0.5) * 3.0;
				if (frand() < 0.08) v = 0;
				if (frand() < 0.05) v += 4;
				s->heat[row * HCOLS + col] = v < 0 ? 0 : v > 8 ? 8 : v;
			}
	}
	for (int i = 0; i < NTAGWORDS; i++) { s->tagw[i] = 0; for (int d = 0; d < s->ndocs; d++) if (strstr(s->doctags[d], TAGWORDS[i])) s->tagw[i]++; }

	s->pe = lt_pixeleditor_new(24, 11);
	lt_pixeleditor_set_toolbar(s->pe, true);
	lt_pixeleditor_set_toolbar_side(s->pe, LT_SIDE_LEFT);
	lt_pixeleditor_set_color(s->pe, LT_RGB(0, 255, 65));
	for (int i = 2; i < 10; i++) lt_pixeleditor_set(s->pe, i, 2, LT_RGB(0, 255, 65));            /* a small seed shape: an "L", paint the rest with the mouse */
	for (int i = 2; i < 8; i++) lt_pixeleditor_set(s->pe, 2, i, LT_RGB(0, 255, 65));

	s->ne = lt_nodeeditor_new();
	int nd_spec = lt_nodeeditor_add(s->ne, "spec", 1, 1), nd_layout = lt_nodeeditor_add(s->ne, "layout", 20, 1);
	int nd_canvas = lt_nodeeditor_add(s->ne, "canvas", 20, 6), nd_screen = lt_nodeeditor_add(s->ne, "screen", 1, 6);
	lt_nodeeditor_connect(s->ne, nd_spec, nd_layout); lt_nodeeditor_connect(s->ne, nd_layout, nd_canvas); lt_nodeeditor_connect(s->ne, nd_canvas, nd_screen);
	lt_nodeeditor_select(s->ne, nd_spec);

	s->gp = lt_gradientpicker_new();
	lt_gradientpicker_select(s->gp, 0); lt_gradientpicker_set_color(s->gp, LT_RGB(0, 40, 10));
	lt_gradientpicker_select(s->gp, 1); lt_gradientpicker_set_color(s->gp, LT_RGB(190, 255, 205));
	lt_gradientpicker_add_stop(s->gp, 0.5, LT_RGB(0, 255, 65));

	s->cpk = lt_colorpicker_new(LT_RGB(0, 255, 65));

	s->timer = lt_timer_new();
	lt_timer_set_countdown(s->timer, 60.0);
	lt_timer_start(s->timer, s->time);
	time_t nowsecs = time(NULL);
	struct tm nowtm = *localtime(&nowsecs);
	s->datepick = lt_datepicker_new(nowtm.tm_year + 1900, nowtm.tm_mon + 1, nowtm.tm_mday);
	s->timepick = lt_timepicker_new(nowtm.tm_hour, nowtm.tm_min, nowtm.tm_sec);
	lt_timepicker_set_seconds(s->timepick, true);
	s->datepick_compact = lt_datepicker_new(nowtm.tm_year + 1900, nowtm.tm_mon + 1, nowtm.tm_mday);
	lt_datepicker_set_kind(s->datepick_compact, LT_DATE_COMPACT);
	s->timepick_h12 = lt_timepicker_new(nowtm.tm_hour, nowtm.tm_min, nowtm.tm_sec);
	lt_timepicker_set_kind(s->timepick_h12, LT_TIME_H12);
	s->timepick_spin = lt_timepicker_new(nowtm.tm_hour, nowtm.tm_min, nowtm.tm_sec);
	lt_timepicker_set_kind(s->timepick_spin, LT_TIME_SPINBOX);

	s->seq = lt_sequencer_new(4, 16);
	lt_sequencer_set_group(s->seq, 4);
	static const char *const seqnames[4] = {"Kick", "Snare", "HiHat", "Clap"};
	for (int i = 0; i < 4; i++) lt_sequencer_set_track_name(s->seq, i, seqnames[i]);
	for (int st = 0; st < 16; st += 4) lt_sequencer_set(s->seq, 0, st, true);            /* kick: every downbeat */
	lt_sequencer_set(s->seq, 1, 4, true); lt_sequencer_set(s->seq, 1, 12, true);          /* snare: the backbeat */
	for (int st = 0; st < 16; st += 2) lt_sequencer_set(s->seq, 2, st, true);             /* hihat: every 8th */
	lt_sequencer_set(s->seq, 3, 8, true);                                                 /* clap: one accent */
}

static void register_all(S *s)
{
	lt_app *a = s->app;
	lt_app_custom(a, "graph", draw_graph, NULL, s);
	lt_app_custom(a, "table", draw_table, NULL, s);
	lt_app_custom(a, "webchart", draw_webchart, NULL, s);
	lt_app_custom(a, "trichart", draw_trichart, NULL, s);
	lt_app_custom(a, "pentachart", draw_pentachart, NULL, s);
	lt_app_custom(a, "radar", draw_radar, NULL, s);
	lt_app_custom(a, "globe", draw_globe, NULL, s);
	lt_app_custom(a, "globewire", draw_globewire, NULL, s);
	static const char *mapnames[4] = {"map", "heatmap", "greymap", "wiremap"};
	for (int k = 0; k < 4; k++) { memset(&MAPW[k], 0, sizeof MAPW[k]); MAPW[k].s = s; MAPW[k].kind = k; lt_mapview_init(&MAPW[k].v); lt_app_custom(a, mapnames[k], draw_mapw, event_mapw, &MAPW[k]); }
	lt_app_custom(a, "spinners", draw_spinners, NULL, s);
	lt_app_custom(a, "piechart", draw_piechart, NULL, s);
	lt_app_custom(a, "gauges", draw_gauges, NULL, s);
	lt_app_custom(a, "linechart", draw_linechart, NULL, s);
	lt_app_custom(a, "barchart", draw_barchart, NULL, s);
	lt_app_custom(a, "histogram", draw_histogram, NULL, s);
	lt_app_custom(a, "heatgrid", draw_heatgrid, NULL, s);
	lt_app_custom(a, "tagcloud", draw_tagcloud, NULL, s);
	lt_app_custom(a, "minimap", draw_minimap, NULL, s);
	lt_app_custom(a, "pixedit", draw_pixedit, event_pixedit, s);
	lt_app_custom(a, "nodedit", draw_nodedit, event_nodedit, s);
	lt_app_custom(a, "gradpick", draw_gradpick, event_gradpick, s);
	lt_app_custom(a, "colorpick", draw_colorpick, event_colorpick, s);
	lt_app_custom(a, "dtall", draw_dtall, event_dtall, s);
	lt_app_custom(a, "seq", draw_seq, event_seq, s);
	lt_app_on(a, "app", "*", on_app, s);
	lt_app_on(a, "*", "change", on_widget, s);
	lt_app_on(a, "*", "activate", on_widget, s);
	lt_app_key(a, "ctrl-t", "theme"); lt_app_key(a, "ctrl-p", "pause"); lt_app_key(a, "ctrl-r", "refresh"); lt_app_key(a, "enter", "refresh");
	lt_app_key(a, "+", "faster"); lt_app_key(a, "=", "faster"); lt_app_key(a, "-", "slower"); lt_app_key(a, "_", "slower");
	lt_app_key(a, "f1", "page0"); lt_app_key(a, "f2", "page1"); lt_app_key(a, "f3", "page2"); lt_app_key(a, "alt-1", "page0"); lt_app_key(a, "alt-2", "page1"); lt_app_key(a, "alt-3", "page2");
	lt_app_key(a, "esc", "escape"); lt_app_key(a, "ctrl-q", "quit");
}

int main(int argc, char **argv)
{
	static S s;
	srand(7);
	s.app = lt_app_new();
	char err[200];
	char *spec = spec_with_themes();
	bool loaded = lt_app_load(s.app, spec, err, sizeof err);
	free(spec);
	if (!loaded) { fprintf(stderr, "demo: spec error: %s\n", err); return 1; }
	register_all(&s);
	init_data(&s);
	fill_lists(&s);
	lt_app_set(s.app, "country_node", -1);
	lt_app_set(s.app, "z_used", s.z[0]); lt_app_set(s.app, "z_cache", s.z[1]); lt_app_set(s.app, "z_free", s.z[2]);
	lt_app_set(s.app, "progress", 0.0);
	update_info(&s);
	refresh(&s, false);
	run_search(&s);
	if (argc > 1 && !strcmp(argv[1], "--snapshot")) {
		lt_caps caps = lt_caps_detect();
		int w = argc > 2 ? atoi(argv[2]) : 110, h = argc > 3 ? atoi(argv[3]) : 34;
		lt_app_set(s.app, "tab", argc > 4 ? atoi(argv[4]) : 0);
		lt_app_set(s.app, "progress", 0.37);
		s.time = 3.0;
		lt_canvas *c = lt_canvas_new(w, h, caps);
		lt_theme th = theme_of(0);
		if (argc > 5) lt_theme_by_name(argv[5], &th);
		lt_canvas_clear(c, th.s[LT_T_TEXT]);
		lt_app_render(s.app, c, &th);
		char *d = lt_canvas_dump(c);
		fputs(d, stdout);
		free(d);
		return 0;
	}
	lt_tty *t = lt_tty_open((lt_tty_opts){.alt_screen = true, .mouse = true, .kitty = true});
	if (!t) { fprintf(stderr, "demo: needs a terminal (try --snapshot)\n"); return 1; }
	lt_ui *ui = lt_ui_new(t);
	lt_ui_set_theme(ui, theme_of(0));
	lt_app_attach(s.app, ui);
	lt_ui_run(ui);
	if (getenv("LT_STATS")) { FILE *f = fopen(getenv("LT_STATS"), "w"); if (f) { fprintf(f, "frames %lu avg %.3f ms max %.3f ms\n", lt_ui_frames(ui), lt_ui_frame_avg_ms(ui), lt_ui_frame_max_ms(ui)); fclose(f); } }
	lt_ui_free(ui);
	lt_tty_close(t);
	lt_app_free(s.app);
	return 0;
}
