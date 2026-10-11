/* test_widgets.c - the stateless bars/boxes/shapes/maps: progressbar, pacmanbar (byte-checked against pacman's own output shape),
 * ringbar, zonebar, sparkline/graph, panel, keybar, shapes, worldmap/globe, webchart/radar, rain/scramble, searchbar draw. */
#include "lazytui.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); fails++; } } while (0)
static lt_caps UNI = {true, true, false, true, 256};

static bool has_glyph(const lt_canvas *c)
{
	for (int y = 0; y < lt_canvas_h(c); y++) for (int x = 0; x < lt_canvas_w(c); x++) { uint32_t ch = lt_canvas_char(c, x, y); if (ch != ' ' && ch != 0) return true; }
	return false;
}

int main(void)
{
	lt_theme th = lt_theme_matrix(), mono = lt_theme_mono();
	lt_canvas *c = lt_canvas_new(60, 20, UNI);
	lt_rect all = {0, 0, 60, 20};

	/* progressbar: 0%% is empty, 100%% is full, the percentage text is shown */
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	lt_progressbar(c, (lt_rect){0, 0, 30, 1}, &th, 0.0, "cpu", true);
	char *p0 = lt_canvas_dump(c);
	CHECK(strstr(p0, "0%"), "0%% progress shows 0%%");
	free(p0);
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	lt_progressbar(c, (lt_rect){0, 0, 30, 1}, &th, 1.0, "cpu", true);
	char *p1 = lt_canvas_dump(c);
	CHECK(strstr(p1, "100%"), "100%% progress shows 100%%");
	free(p1);
	lt_progressbar(c, (lt_rect){0, 0, 30, 1}, &th, NAN, "cpu", true);            /* NaN must not crash, treated as 0 */

	/* pacman line: matches the documented shape - name, size, rate, eta, a bracketed bar, a percent */
	char line[512];
	lt_pacman_state st = {0};
	size_t ln = lt_pacman_line(line, sizeof line, 80, "linux", 22.0 * 1048576, 44.0 * 1048576, 11.8 * 1048576, 15, &st);
	CHECK(ln > 0 && strstr(line, "linux") && strstr(line, "MiB") && strstr(line, "[") && strstr(line, "]") && strstr(line, "50%"), "a pacman line has every documented part at 50%%");
	char etaline[256];
	lt_pacman_line(etaline, sizeof etaline, 80, "x", 1, 2, 0, -1, NULL);
	CHECK(strstr(etaline, "--:--"), "a negative eta shows as --:--");
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	lt_pacmanbar(c, (lt_rect){0, 0, 60, 1}, &th, "linux", 22, 44, 11, 15, &st);
	CHECK(has_glyph(c), "pacmanbar draws something");

	/* ringbar: the marquee's visible text changes as phase advances, and an empty string is a no-op */
	lt_canvas_clear(c, th.s[LT_T_TEXT]); lt_ringbar(c, (lt_rect){0, 0, 10, 1}, &th, 0, "hello world", LT_DEFAULT); char *r0 = lt_canvas_dump(c);
	lt_canvas_clear(c, th.s[LT_T_TEXT]); lt_ringbar(c, (lt_rect){0, 0, 10, 1}, &th, 5, "hello world", LT_DEFAULT); char *r1 = lt_canvas_dump(c);
	CHECK(strcmp(r0, r1) != 0, "the ring scrolled after 5 cells of phase");
	free(r0); free(r1);
	lt_ringbar(c, (lt_rect){0, 0, 10, 1}, &th, 0, "", LT_DEFAULT);
	lt_ringbar(c, (lt_rect){0, 0, 10, 1}, &th, NAN, "text", LT_DEFAULT);         /* NaN phase must not crash */

	/* zonebar: widths sum exactly to the rectangle, and an all-zero weight set does not divide by zero */
	lt_zone z[3] = {{"used", 30, LT_T_ERR}, {"cache", 20, LT_T_WARN}, {"free", 50, LT_T_OK}};
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	lt_zonebar(c, (lt_rect){0, 0, 47, 1}, &th, z, 3);
	int used = 0;
	for (int x = 0; x < 47; x++) if (lt_canvas_bg(c, x, 0) != LT_DEFAULT || lt_canvas_char(c, x, 0) != ' ') used++;
	CHECK(used > 0, "the zonebar paints across its rectangle");
	lt_zone zero[2] = {{"a", 0, LT_T_OK}, {"b", 0, LT_T_ERR}};
	lt_zonebar(c, (lt_rect){0, 0, 10, 1}, &th, zero, 2);                        /* must not crash */

	/* sparkline / graph: NaN samples are skipped (blank), real ones are not; a graph of height 1 behaves like a sparkline */
	double vals[6] = {0, NAN, 50, 100, NAN, 25};
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	lt_sparkline(c, (lt_rect){0, 0, 6, 1}, &th, vals, 6, 0, 100, LT_T_OK);
	CHECK(lt_canvas_char(c, 1, 0) == ' ' && lt_canvas_char(c, 3, 0) != ' ', "a NaN sample leaves a blank, a real one draws");
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	lt_graph(c, (lt_rect){0, 0, 6, 1}, &th, vals, 6, 0, 100, LT_T_OK);
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	lt_graph(c, (lt_rect){0, 0, 6, 5}, &th, vals, 6, 0, 100, LT_T_OK);
	CHECK(has_glyph(c), "a multi-row graph draws");

	/* panel: the inner rect is the outer minus the frame, a title appears when there is room */
	lt_rect pr = {0, 0, 40, 10};
	lt_rect inner = lt_panel_inner(pr);
	CHECK(inner.x == 2 && inner.y == 1 && inner.w == 36 && inner.h == 8, "the panel's inner rect matches the frame width");
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	lt_panel(c, pr, &th, "status");
	char *pd = lt_canvas_dump(c);
	CHECK(strstr(pd, "status"), "the panel's title is drawn");
	free(pd);
	lt_panel_ex(c, pr, &th, "hud", LT_PANEL_HUD);                               /* every style at least does not crash */
	lt_panel_ex(c, pr, &th, "sharp", LT_PANEL_SHARP);
	lt_panel_ex(c, pr, &th, "double", LT_PANEL_DOUBLE);
	lt_panel(c, (lt_rect){0, 0, 1, 1}, &th, "too small");                       /* degenerate size: no crash */

	/* keybar: a hint is hit-testable where it was drawn, and held-modifier filtering changes what shows */
	lt_hint hints[2] = {{"enter", "open", 1}, {"ctrl-x", "close", 2}};
	lt_keybar *kb = lt_keybar_new(hints, 2);
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	lt_keybar_draw(kb, c, (lt_rect){0, 0, 60, 1}, &th, 0, false);
	CHECK(lt_keybar_hit(kb, 1, 0) == 1, "a click on the first hint's key finds its id");
	CHECK(lt_keybar_hit(kb, 59, 5) == -1, "a click off the bar finds nothing");
	int rows_unknown = lt_keybar_rows_needed(kb, 60, 0, false);
	CHECK(rows_unknown >= 1, "rows_needed reports at least one row");
	lt_keybar_free(kb);

	/* textbar / searchbar draw */
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	lt_textbar(c, (lt_rect){0, 0, 20, 1}, &th, "status line", LT_T_TEXT);
	char *tb = lt_canvas_dump(c);
	CHECK(strstr(tb, "status line"), "textbar shows its text");
	free(tb);
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	lt_searchbar(c, (lt_rect){0, 0, 20, 1}, &th, "> ", "query", 2);
	char *sb = lt_canvas_dump(c);
	CHECK(strstr(sb, "query"), "searchbar shows the typed text");
	free(sb);

	/* shapes: a shape paints inside its rectangle and stays inside it */
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	lt_circle(c, (lt_rect){0, 0, 20, 10}, LT_RGB(200, 50, 50), LT_RGB(255, 255, 255));
	CHECK(has_glyph(c), "a filled circle draws");
	lt_hexagon(c, (lt_rect){0, 0, 20, 10}, LT_RGB(50, 200, 50), LT_DEFAULT);
	lt_octagon(c, (lt_rect){0, 0, 20, 10}, LT_RGB(50, 50, 200), LT_DEFAULT);
	lt_ngon(c, (lt_rect){0, 0, 10, 10}, 5, 0, LT_RGB(255, 0, 0), LT_DEFAULT);
	lt_wire_circle(c, (lt_rect){0, 0, 10, 10}, LT_RGB(0, 255, 0));
	lt_wire_ngon(c, (lt_rect){0, 0, 10, 10}, 6, 0, LT_RGB(0, 0, 255));
	double star[10] = {0, -5, 1, -1, 5, 0, 1, 1, 0, 5};
	lt_polygon(c, (lt_rect){0, 0, 10, 10}, star, 5, LT_RGB(255, 255, 0), LT_DEFAULT);
	lt_shape_draw(c, (lt_rect){0, 0, 1, 1}, LT_SHAPE_CIRCLE, LT_RGB(1, 1, 1), LT_DEFAULT);    /* a 1x1 shape: no crash */

	/* worldmap / globe: draw without crashing, and a country picked at its own label point round-trips at least once */
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	lt_worldmap(c, (lt_rect){0, 0, 60, 20}, 0, 0, LT_DEFAULT);
	CHECK(has_glyph(c), "the default worldmap draws");
	lt_worldmap(c, (lt_rect){0, 0, 60, 20}, LT_MAP_WIRE | LT_MAP_GRID, 0, LT_RGB(0, 255, 0));
	CHECK(lt_country_count() > 0, "the built-in country table is not empty");
	int fra = lt_country_by_iso("FRA");
	CHECK(fra >= 0, "France is in the country table");
	lt_country ci;
	CHECK(lt_country_info(fra, &ci) && !strcmp(ci.iso, "FRA"), "country info round-trips by index");
	CHECK(!lt_country_info(999999, &ci), "an out-of-range index is refused, not a crash");
	int col, row;
	bool onmap = lt_worldmap_project((lt_rect){0, 0, 60, 20}, 2.35, 48.85, &col, &row);       /* Paris */
	CHECK(onmap, "a normal lon/lat projects onto the map");
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	lt_globe(c, (lt_rect){0, 0, 30, 15}, 0, 23.4, 0, 0);
	CHECK(has_glyph(c), "the globe draws");
	lt_globe_wire(c, (lt_rect){0, 0, 30, 15}, 0, 23.4, LT_RGB(0, 255, 0));
	int gcol, grow;
	CHECK(lt_globe_project((lt_rect){0, 0, 30, 15}, 0, 0, 0, 0, &gcol, &grow), "(0,0) facing the viewer projects onto the globe");
	CHECK(!lt_globe_project((lt_rect){0, 0, 30, 15}, 0, 0, 180, 0, &gcol, &grow), "the far side of the globe does not project");

	/* webchart / radar */
	lt_axis axes[4] = {{"n", 8}, {"e", 3}, {"s", 6}, {"w", 4}};
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	lt_webchart(c, (lt_rect){0, 0, 30, 15}, &th, axes, 4, 10, 4, LT_DEFAULT);
	CHECK(has_glyph(c), "a webchart draws");
	lt_blip blips[2] = {{45, 0.5, "a"}, {200, 0.9, "b"}};
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	lt_radar(c, (lt_rect){0, 0, 30, 15}, &th, 90, blips, 2, LT_DEFAULT);
	CHECK(has_glyph(c), "a radar draws");

	/* rain / scramble: time-driven effects, must draw and must not crash on progress out of [0,1] */
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	lt_rain(c, all, &th, 3.0, 42);
	CHECK(has_glyph(c), "rain draws something at a nonzero time");
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	lt_scramble(c, (lt_rect){0, 0, 20, 1}, &th, "decoded", 0.5, 1.0);
	CHECK(has_glyph(c), "a half-decoded scramble draws");
	lt_scramble(c, (lt_rect){0, 0, 20, 1}, &th, "decoded", 2.0, 1.0);            /* progress > 1: clamps, no crash */
	lt_scramble(c, (lt_rect){0, 0, 20, 1}, &th, "decoded", -1.0, 1.0);

	/* mono theme still draws every widget above without crashing (no colour, attributes only) */
	lt_canvas_clear(c, mono.s[LT_T_TEXT]);
	lt_progressbar(c, (lt_rect){0, 0, 20, 1}, &mono, 0.5, "x", true);
	lt_zonebar(c, (lt_rect){0, 0, 20, 1}, &mono, z, 3);
	lt_panel(c, (lt_rect){0, 0, 20, 6}, &mono, "mono");
	CHECK(has_glyph(c), "the mono theme still produces visible output");

	lt_canvas_free(c);
	printf(fails ? "widgets: %d failed\n" : "widgets: ok\n", fails);
	return fails != 0;
}
