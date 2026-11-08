/* tabs.c - the tabbed and stack layouts and their title bar.
 *
 *   tabbed  one window is shown; a bar on top lists every tiled window of the view side by side (like i3 tabbed)
 *   stack   one window is shown; the titles of all windows are stacked vertically above it (like i3 stacking)
 *
 * The bar is ONE wlr_scene_buffer per monitor, redrawn into a software pixel buffer with drw.c (the same fontconfig/FreeType
 * code dmenu and the status bar use; its drw_* symbols are exported from the clients group for this) and handed to the scene
 * graph. Colours come from theme.c (bordercolor, focuscolor, urgentcolor). Hidden windows have their scene node disabled by
 * the layout; arrange() enables everything again before the next layout runs. */
#include <drm_fourcc.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include "dwl.h"
#include "drw.h"

unsigned int tab_bar_height = 22;           /* px; 0 = no bar (the layouts then behave like monocle) */
char tab_font[96] = "monospace:size=10";

#define MAXTAB 64

static Drw *tdrw;
static char tfont_loaded[96];

struct tabbuf { struct wlr_buffer base; uint32_t *data; };

static void tb_destroy(struct wlr_buffer *b) { struct tabbuf *t = wl_container_of(b, t, base); free(t->data); free(t); }
static bool tb_begin(struct wlr_buffer *b, uint32_t flags, void **data, uint32_t *fmt, size_t *stride)
{
	struct tabbuf *t = wl_container_of(b, t, base);
	if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE) return false;
	*data = t->data; *fmt = DRM_FORMAT_ARGB8888; *stride = (size_t)b->width * 4;
	return true;
}
static void tb_end(struct wlr_buffer *b) {}
static const struct wlr_buffer_impl tb_impl = { .destroy = tb_destroy, .begin_data_ptr_access = tb_begin, .end_data_ptr_access = tb_end };

int
tabs_layout(Monitor *m)
{
	const Layout *l = m->lt[m->sellt];
	return l->arrange == tabbed ? 1 : l->arrange == stack ? 2 : 0;
}

/* tiled windows of the view, oldest first (a new tab opens on the right / at the bottom, as in i3) */
static int
tablist(Monitor *m, Client **out)
{
	Client *c;
	int n = 0;
	wl_list_for_each_reverse(c, &clients, link)
		if (VISIBLEON(c, m) && !c->isfloating && !c->isfullscreen && n < MAXTAB)
			out[n++] = c;
	return n;
}

static Clr
clr(const float *f)
{
	return ((Clr)(f[3] * 255 + .5f) << 24) | ((Clr)(f[0] * 255 + .5f) << 16) | ((Clr)(f[1] * 255 + .5f) << 8) | (Clr)(f[2] * 255 + .5f);
}

/* black or white text, whichever reads on the background */
static Clr
fgfor(const float *bg)
{
	return (0.299f * bg[0] + 0.587f * bg[1] + 0.114f * bg[2]) > 0.55f ? 0xff000000 : 0xffffffff;
}

static unsigned int
barh(void)
{
	return tab_bar_height;
}

void
tabs_hide(Monitor *m)
{
	if (m->tabbuf)
		wlr_scene_node_set_enabled(&m->tabbuf->node, 0);
}

void
tabs_forget(Monitor *m)
{
	if (m->tabbuf) { wlr_scene_node_destroy(&m->tabbuf->node); m->tabbuf = NULL; }
	m->tabsel = NULL;
}

/* redraw the bar for the clients in list */
static void
tabs_draw(Monitor *m, Client **l, int n, int vertical, Client *sel)
{
	unsigned int h = barh(), w = (unsigned)m->w.width, bh, i;
	struct tabbuf *t;
	const char *title;

	if (!h || !n) { tabs_hide(m); return; }
	bh = vertical ? h * (unsigned)n : h;
	if (bh > (unsigned)m->w.height / 2) bh = (unsigned)m->w.height / 2;
	if (!tdrw) tdrw = drw_create(w, bh);
	else drw_resize(tdrw, w, bh);
	if (!tdrw) return;
	if (!tdrw->fonts || strcmp(tfont_loaded, tab_font)) {
		const char *f[] = { tab_font, "monospace:size=10" };
		if (tdrw->fonts) { drw_fontset_free(tdrw->fonts); tdrw->fonts = NULL; }
		drw_fontset_create(tdrw, f, 2);
		snprintf(tfont_loaded, sizeof tfont_loaded, "%s", tab_font);
	}
	if (!tdrw->fonts) return;
	for (i = 0; i < w * bh; i++) tdrw->pixels[i] = clr(rootcolor);
	for (i = 0; i < (unsigned)n; i++) {
		const float *bg = l[i]->isurgent ? urgentcolor : l[i] == sel ? focuscolor : bordercolor;
		Clr scm[2] = { fgfor(bg), clr(bg) };
		unsigned int x, y, tw, th = h;
		if (vertical) { x = 0; y = i * h; tw = w; }
		else { x = (unsigned)((unsigned long)w * i / (unsigned)n); tw = (unsigned)((unsigned long)w * (i + 1) / (unsigned)n) - x; y = 0; }
		if (y + th > bh) break;
		drw_setscheme(tdrw, scm);
		title = client_get_title(l[i]);
		drw_text(tdrw, (int)x, (int)y, tw > 1 ? tw - 1 : tw, th - (vertical ? 1 : 0), 6, title && *title ? title : "(untitled)", 0);
	}
	if (!(t = calloc(1, sizeof *t)) || !(t->data = malloc((size_t)w * bh * 4))) { free(t); return; }
	memcpy(t->data, tdrw->pixels, (size_t)w * bh * 4);
	wlr_buffer_init(&t->base, &tb_impl, (int)w, (int)bh);
	if (!m->tabbuf)
		m->tabbuf = wlr_scene_buffer_create(layers[LyrTile], NULL);
	wlr_scene_buffer_set_buffer(m->tabbuf, &t->base);
	wlr_buffer_drop(&t->base);
	wlr_scene_node_set_position(&m->tabbuf->node, m->w.x, m->w.y);
	wlr_scene_node_set_enabled(&m->tabbuf->node, 1);
	wlr_scene_node_raise_to_top(&m->tabbuf->node);
}

static void
tablayout(Monitor *m, int vertical)
{
	Client *l[MAXTAB], *c, *sel;
	int n = tablist(m, l), i;
	unsigned int bh = 0;
	struct wlr_box box;

	m->ntabs = n;
	if (!n) { m->tabsel = NULL; tabs_hide(m); return; }
	sel = focustop(m);
	for (i = 0, c = NULL; i < n; i++) if (l[i] == sel) c = sel;
	if (!c) c = (m->tabsel && VISIBLEON(m->tabsel, m) && !m->tabsel->isfloating) ? m->tabsel : l[0];
	sel = c;
	if (barh()) {
		bh = vertical ? barh() * (unsigned)n : barh();
		if (bh > (unsigned)m->w.height / 2) bh = (unsigned)m->w.height / 2;
	}
	box = (struct wlr_box){ .x = m->w.x, .y = m->w.y + (int)bh, .width = m->w.width, .height = m->w.height - (int)bh };
	for (i = 0; i < n; i++) {
		resize(l[i], box, 0);
		wlr_scene_node_set_enabled(&l[i]->scene->node, l[i] == sel);
	}
	wlr_scene_node_raise_to_top(&sel->scene->node);
	m->tabsel = sel;
	tabs_draw(m, l, n, vertical, sel);
}

void tabbed(Monitor *m) { tablayout(m, 0); }
void stack(Monitor *m) { tablayout(m, 1); }

/* title or urgent change, new colours: redraw without a relayout */
void
tabs_refresh(Monitor *m)
{
	Client *l[MAXTAB];
	int k = tabs_layout(m), n;
	if (!k || !m->tabsel) return;
	n = tablist(m, l);
	m->ntabs = n;
	tabs_draw(m, l, n, k == 2, m->tabsel);
}

/* focusing c on its monitor needs a relayout so that c becomes the shown window */
int
tabs_wants(Monitor *m, Client *c)
{
	return m && tabs_layout(m) && m->tabsel != c && VISIBLEON(c, m) && !c->isfloating;
}

/* the window whose tab is at layout coordinates (lx, ly), or NULL */
Client *
tabs_hit(Monitor *m, double lx, double ly)
{
	Client *l[MAXTAB];
	int k, n, i;
	unsigned int h = barh();
	double x, y;

	if (!m || !(k = tabs_layout(m)) || !h || !m->tabbuf || !m->tabbuf->node.enabled) return NULL;
	n = tablist(m, l);
	x = lx - m->w.x; y = ly - m->w.y;
	if (x < 0 || x >= m->w.width || y < 0) return NULL;
	if (k == 1) { if (y >= h) return NULL; i = (int)(x * n / m->w.width); }
	else { if (y >= (double)h * n) return NULL; i = (int)(y / h); }
	return i >= 0 && i < n ? l[i] : NULL;
}

/* switch tab: delta = +1/-1 relative to the shown window, or idx >= 0 absolute (0-based) */
void
tabs_select(Monitor *m, int delta, int idx)
{
	Client *l[MAXTAB];
	int n, cur = 0, i;
	if (!m) return;
	n = tablist(m, l);
	if (!n) return;
	for (i = 0; i < n; i++) if (l[i] == (m->tabsel ? m->tabsel : focustop(m))) cur = i;
	i = idx >= 0 ? (idx < n ? idx : n - 1) : ((cur + delta) % n + n) % n;
	focusclient(l[i], 1);
	arrange(m);
	printstatus();
}

/* layout per (monitor, tag): the slot of the lowest tag in the view; all = every tag of the monitor */
void
monsetlt(Monitor *m, const Layout *l, int all)
{
	unsigned int ts = m->tagset[m->seltags] & TAGMASK;
	int i;
	if (all) {
		for (i = 0; i < TAGCOUNT; i++) m->tlt[i] = l;
	} else if (ts) {
		m->tlt[__builtin_ctz(ts)] = l;
	}
	m->lt[m->sellt] = l;
}
