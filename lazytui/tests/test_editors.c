/* test_editors.c - the three canvas editors: pixel editor (pen/eraser/fill/picker), node editor (drag/pan/connect/delete), gradient
 * picker (add/select/move/remove stops). Draw + synthetic key/mouse events, headless. */
#include "lazytui.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); fails++; } } while (0)
static lt_caps UNI = {true, true, false, true, 256};
static lt_event key(int k) { return (lt_event){LT_EV_KEY, k, 0, 0, 0, NULL}; }
static lt_event mouse(int k, int x, int y) { return (lt_event){LT_EV_MOUSE, k, x, y, 0, NULL}; }

int main(void)
{
	lt_theme th = lt_theme_default();
	lt_canvas *c = lt_canvas_new(40, 20, UNI);

	/* ---- pixel editor ---- */
	lt_pixeleditor *pe = lt_pixeleditor_new(8, 6);
	CHECK(lt_pixeleditor_cols(pe) == 8 && lt_pixeleditor_rows(pe) == 6, "the size given at construction is kept");
	CHECK(lt_pixeleditor_get(pe, 0, 0) == LT_DEFAULT, "a fresh sprite is transparent");
	lt_rect pr = {0, 0, 8, 6};
	lt_pixeleditor_set_color(pe, LT_RGB(200, 30, 30));
	lt_event e = mouse(LT_K_CLICK, 2, 2);
	CHECK(lt_pixeleditor_event(pe, pr, &e) == LT_W_CHANGED, "a click paints a pixel");
	CHECK(lt_pixeleditor_get(pe, 2, 2) == LT_RGB(200, 30, 30), "and it lands in the pen colour");
	e = mouse(LT_K_DRAG, 3, 2);
	lt_pixeleditor_event(pe, pr, &e);
	CHECK(lt_pixeleditor_get(pe, 3, 2) == LT_RGB(200, 30, 30), "dragging continues the stroke");
	e = mouse(LT_K_RELEASE, 3, 2);
	lt_pixeleditor_event(pe, pr, &e);
	e = mouse(LT_K_DRAG, 4, 2);                                                 /* a drag after release (no button down) must not paint */
	lt_pixeleditor_event(pe, pr, &e);
	CHECK(lt_pixeleditor_get(pe, 4, 2) == LT_DEFAULT, "dragging after release does not paint");
	lt_pixeleditor_set_tool(pe, LT_PIXEL_ERASER);
	e = mouse(LT_K_CLICK, 2, 2);
	lt_pixeleditor_event(pe, pr, &e);
	CHECK(lt_pixeleditor_get(pe, 2, 2) == LT_DEFAULT, "the eraser clears a pixel back to transparent");
	lt_pixeleditor_set(pe, 0, 0, LT_RGB(1, 2, 3));
	lt_pixeleditor_set_tool(pe, LT_PIXEL_PICKER);
	e = mouse(LT_K_CLICK, 0, 0);
	lt_pixeleditor_event(pe, pr, &e);
	CHECK(lt_pixeleditor_color(pe) == LT_RGB(1, 2, 3), "the picker copies a pixel's colour into the pen");
	e = mouse(LT_K_CLICK, 5, 5);                                                /* an empty (transparent) cell */
	lt_pixeleditor_event(pe, pr, &e);
	CHECK(lt_pixeleditor_color(pe) == LT_RGB(1, 2, 3), "picking an empty cell leaves the pen colour alone instead of turning it transparent");
	lt_pixeleditor_clear(pe, LT_RGB(9, 9, 9));
	lt_pixeleditor_set_color(pe, LT_RGB(50, 50, 50));
	lt_pixeleditor_set_tool(pe, LT_PIXEL_FILL);
	e = mouse(LT_K_CLICK, 3, 3);
	lt_pixeleditor_event(pe, pr, &e);
	CHECK(lt_pixeleditor_get(pe, 0, 0) == LT_RGB(50, 50, 50) && lt_pixeleditor_get(pe, 7, 5) == LT_RGB(50, 50, 50), "fill floods the whole same-coloured sprite");
	/* fill and picker are one-shot: a click that drifts into a drag (as a real mouse click always does a little) must not re-fire them */
	lt_pixeleditor_clear(pe, LT_RGB(9, 9, 9));
	lt_pixeleditor_set(pe, 7, 5, LT_RGB(1, 1, 1));                              /* a second, disconnected region */
	lt_pixeleditor_set_color(pe, LT_RGB(50, 50, 50));
	e = mouse(LT_K_CLICK, 0, 0);
	lt_pixeleditor_event(pe, pr, &e);
	e = mouse(LT_K_DRAG, 7, 5);                                                 /* drifted onto the disconnected region mid-click */
	lt_pixeleditor_event(pe, pr, &e);
	CHECK(lt_pixeleditor_get(pe, 7, 5) == LT_RGB(1, 1, 1), "a drag during a fill click does not flood a second, unrelated region");
	lt_pixeleditor_set_tool(pe, LT_PIXEL_PICKER);
	lt_pixeleditor_set_color(pe, LT_RGB(50, 50, 50));
	e = mouse(LT_K_CLICK, 0, 0);                                                /* LT_RGB(50,50,50), from the fill just above */
	lt_pixeleditor_event(pe, pr, &e);
	e = mouse(LT_K_DRAG, 7, 5);                                                 /* drifted onto LT_RGB(1,1,1) mid-click */
	lt_pixeleditor_event(pe, pr, &e);
	CHECK(lt_pixeleditor_color(pe) == LT_RGB(50, 50, 50), "a drag during a picker click does not change what colour got picked");
	/* keyboard-only editing: arrows move the cursor, Enter applies */
	lt_pixeleditor_set_tool(pe, LT_PIXEL_PEN);
	lt_pixeleditor_clear(pe, LT_DEFAULT);
	lt_pixeleditor_set_color(pe, LT_RGB(7, 7, 7));
	e = key(LT_K_HOME); lt_pixeleditor_event(pe, pr, &e);                       /* Home pins x to 0; y keeps whatever the earlier mouse clicks left it at */
	int y0 = lt_pixeleditor_cursor_y(pe);
	e = key(LT_K_RIGHT); lt_pixeleditor_event(pe, pr, &e);
	e = key(LT_K_RIGHT); lt_pixeleditor_event(pe, pr, &e);
	e = key(LT_K_DOWN); lt_pixeleditor_event(pe, pr, &e);
	CHECK(lt_pixeleditor_cursor_x(pe) == 2 && lt_pixeleditor_cursor_y(pe) == y0 + 1, "arrows move the keyboard cursor");
	int cx = lt_pixeleditor_cursor_x(pe), cy = lt_pixeleditor_cursor_y(pe);
	e = key(LT_K_ENTER);
	CHECK(lt_pixeleditor_event(pe, pr, &e) == LT_W_CHANGED && lt_pixeleditor_get(pe, cx, cy) == LT_RGB(7, 7, 7), "Enter applies the tool at the cursor");
	lt_pixeleditor_draw(pe, c, pr, &th, true);                                  /* must not crash */

	/* the built-in toolbar: off by default, and shifts the canvas down by one row once on */
	CHECK(!lt_pixeleditor_toolbar(pe), "the toolbar is off by default");
	lt_pixeleditor_set_tool(pe, LT_PIXEL_PEN);
	lt_pixeleditor_set_toolbar(pe, true);
	CHECK(lt_pixeleditor_toolbar(pe), "set_toolbar takes effect");
	lt_rect tbr = {0, 0, 40, 8};                                                /* wide enough for all four buttons, unlike the 8-wide sprite rect above */
	lt_pixeleditor_draw(pe, c, tbr, &th, true);                                 /* this populates the buttons' hit rectangles */
	e = mouse(LT_K_CLICK, tbr.x + 9, tbr.y);                                    /* "[ Pen ]" is 7 wide (0..6); the toolbar row, on the "Eraser" button just after it */
	CHECK(lt_pixeleditor_event(pe, tbr, &e) == LT_W_CHANGED && lt_pixeleditor_tool(pe) == LT_PIXEL_ERASER, "clicking a toolbar button switches the tool");
	lt_pixeleditor_clear(pe, LT_RGB(3, 3, 3));
	e = mouse(LT_K_CLICK, tbr.x, tbr.y + 1);                                    /* row 1 = the sprite's row 0, now that the toolbar owns row 0 */
	lt_pixeleditor_event(pe, tbr, &e);
	CHECK(lt_pixeleditor_get(pe, 0, 0) == LT_DEFAULT, "the eraser (selected via the toolbar) cleared that pixel");

	/* the toolbar docked to a side instead: a column of buttons, one per row, next to the canvas rather than above it */
	CHECK(lt_pixeleditor_toolbar_side(pe) == LT_SIDE_TOP, "LT_SIDE_TOP is the default");
	lt_pixeleditor_set_toolbar_side(pe, LT_SIDE_LEFT);
	CHECK(lt_pixeleditor_toolbar_side(pe) == LT_SIDE_LEFT, "set_toolbar_side takes effect");
	lt_pixeleditor_set_tool(pe, LT_PIXEL_PEN);
	lt_rect sbr = {0, 0, 40, 8};
	lt_pixeleditor_draw(pe, c, sbr, &th, true);                                 /* populates the column's hit rectangles */
	e = mouse(LT_K_CLICK, sbr.x + 1, sbr.y + 1);                                /* the 2nd row of the toolbar column = Eraser */
	CHECK(lt_pixeleditor_event(pe, sbr, &e) == LT_W_CHANGED && lt_pixeleditor_tool(pe) == LT_PIXEL_ERASER, "clicking a row in the side toolbox switches the tool");
	lt_pixeleditor_clear(pe, LT_RGB(3, 3, 3));
	e = mouse(LT_K_CLICK, sbr.x + 10, sbr.y);                                   /* column is 9 wide + 1 gap: the sprite starts at x=10 */
	lt_pixeleditor_event(pe, sbr, &e);
	CHECK(lt_pixeleditor_get(pe, 0, 0) == LT_DEFAULT, "the eraser (selected via the side toolbox) cleared the sprite's (0,0)");
	lt_pixeleditor_draw(pe, c, (lt_rect){0, 0, 3, 8}, &th, true);               /* far too narrow for a 9-wide column: must not crash */

	/* reset: back to a blank white-pen sprite, also bound to 'r' */
	lt_pixeleditor_set_toolbar_side(pe, LT_SIDE_TOP);
	lt_pixeleditor_set_color(pe, LT_RGB(9, 9, 9));
	lt_pixeleditor_set_tool(pe, LT_PIXEL_FILL);
	lt_pixeleditor_reset(pe);
	CHECK(lt_pixeleditor_get(pe, 0, 0) == LT_DEFAULT, "reset clears every pixel back to transparent");
	CHECK(lt_pixeleditor_color(pe) == LT_RGB(255, 255, 255) && lt_pixeleditor_tool(pe) == LT_PIXEL_PEN, "...and restores the default white pen and the pen tool");
	lt_pixeleditor_set(pe, 0, 0, LT_RGB(1, 2, 3));
	lt_pixeleditor_set_color(pe, LT_RGB(9, 9, 9));
	e = key('r');
	CHECK(lt_pixeleditor_event(pe, pr, &e) == LT_W_CHANGED && lt_pixeleditor_get(pe, 0, 0) == LT_DEFAULT && lt_pixeleditor_color(pe) == LT_RGB(255, 255, 255), "'r' resets it too");
	lt_pixeleditor_free(pe);

	/* ---- node editor ---- */
	lt_nodeeditor *ne = lt_nodeeditor_new();
	int n1 = lt_nodeeditor_add(ne, "Start", 2, 2);
	int n2 = lt_nodeeditor_add(ne, "End", 30, 10);
	CHECK(lt_nodeeditor_node_count(ne) == 2, "both nodes were added");
	CHECK(!strcmp(lt_nodeeditor_label(ne, n1), "Start"), "a node's label round-trips");
	int x, y;
	lt_nodeeditor_get_pos(ne, n2, &x, &y);
	CHECK(x == 30 && y == 10, "a node's position round-trips");
	lt_nodeeditor_set_label(ne, n1, "Renamed");
	CHECK(!strcmp(lt_nodeeditor_label(ne, n1), "Renamed"), "set_label takes effect");
	int edge = lt_nodeeditor_connect(ne, n1, n2);
	CHECK(edge >= 0 && lt_nodeeditor_edge_count(ne) == 1, "connecting two real nodes succeeds");
	int from, to;
	CHECK(lt_nodeeditor_edge_at(ne, 0, &from, &to) && from == n1 && to == n2, "the edge's endpoints round-trip");
	CHECK(lt_nodeeditor_connect(ne, n1, n1) == -1, "a node cannot connect to itself");
	CHECK(lt_nodeeditor_connect(ne, n1, 999) == -1, "connecting to a bogus id is refused");

	lt_rect ner = {0, 0, 40, 16};
	lt_nodeeditor_draw(ne, c, ner, &th, true);
	int hit = lt_nodeeditor_node_at(ne, ner, 3, 3);
	CHECK(hit == n1, "a click inside a node's box finds it");
	CHECK(lt_nodeeditor_node_at(ne, ner, 39, 15) != n1, "a click far away does not find that node");

	/* dragging a node moves it */
	e = mouse(LT_K_CLICK, 3, 3);
	lt_nodeeditor_event(ne, ner, &e);
	CHECK(lt_nodeeditor_selected(ne) == n1, "clicking a node selects it");
	e = mouse(LT_K_DRAG, 8, 6);
	lt_nodeeditor_event(ne, ner, &e);
	lt_nodeeditor_get_pos(ne, n1, &x, &y);
	CHECK(x != 2 || y != 2, "dragging a selected node moves it");
	e = mouse(LT_K_RELEASE, 8, 6);
	lt_nodeeditor_event(ne, ner, &e);

	/* dragging empty space pans the view */
	int ox0, oy0;
	lt_nodeeditor_get_pan(ne, &ox0, &oy0);
	e = mouse(LT_K_CLICK, 38, 14);                                              /* empty corner, away from both nodes */
	lt_nodeeditor_event(ne, ner, &e);
	CHECK(lt_nodeeditor_selected(ne) == -1, "clicking empty space deselects");
	e = mouse(LT_K_DRAG, 30, 10);
	lt_nodeeditor_event(ne, ner, &e);
	int ox1, oy1;
	lt_nodeeditor_get_pan(ne, &ox1, &oy1);
	CHECK(ox1 != ox0 || oy1 != oy0, "dragging empty space pans the view");
	e = mouse(LT_K_RELEASE, 30, 10);
	lt_nodeeditor_event(ne, ner, &e);
	lt_nodeeditor_pan(ne, 0, 0);

	/* connect mode: 'c' arms it, clicking the source then the target makes an edge */
	int n3 = lt_nodeeditor_add(ne, "Mid", 15, 6);
	e = key('c');
	lt_nodeeditor_event(ne, ner, &e);
	CHECK(lt_nodeeditor_connecting(ne), "'c' arms connect mode");
	lt_nodeeditor_get_pos(ne, n3, &x, &y);
	e = mouse(LT_K_CLICK, x + 1, y + 1);
	lt_nodeeditor_event(ne, ner, &e);
	lt_nodeeditor_get_pos(ne, n2, &x, &y);
	e = mouse(LT_K_CLICK, x + 1, y + 1);
	lt_wres cr = lt_nodeeditor_event(ne, ner, &e);
	CHECK(cr == LT_W_ACTIVATE && lt_nodeeditor_edge_count(ne) == 2 && !lt_nodeeditor_connecting(ne), "connect mode makes an edge and then turns itself off");
	e = key('c'); lt_nodeeditor_event(ne, ner, &e);
	e = key(LT_K_ESC);
	CHECK(lt_nodeeditor_event(ne, ner, &e) == LT_W_HANDLED && !lt_nodeeditor_connecting(ne), "Esc cancels an armed connect");

	/* delete removes the node and every edge touching it */
	lt_nodeeditor_select(ne, n2);
	e = key('d');
	lt_nodeeditor_event(ne, ner, &e);
	CHECK(lt_nodeeditor_node_count(ne) == 2, "deleting a node removes it");
	CHECK(lt_nodeeditor_edge_count(ne) == 0, "and every edge that touched it");
	CHECK(lt_nodeeditor_node_at(ne, ner, x + 1, y + 1) == -1, "the deleted node no longer hit-tests");
	lt_nodeeditor_draw(ne, c, ner, &th, true);                                  /* must not crash with a dangling edge / mid-connect state */

	/* zoom: +/- change it, 0 resets it (and the pan), wheel zooms around the pointer, positions still round-trip through get_pos */
	CHECK(lt_nodeeditor_zoom(ne) == 1.0, "starts unzoomed");
	e = key('+');
	lt_nodeeditor_event(ne, ner, &e);
	CHECK(lt_nodeeditor_zoom(ne) > 1.0, "'+' zooms in");
	e = key('-');
	lt_nodeeditor_event(ne, ner, &e);
	lt_nodeeditor_event(ne, ner, &e);
	CHECK(lt_nodeeditor_zoom(ne) < 1.0, "'-' zooms back out, past 1.0");
	lt_nodeeditor_set_zoom(ne, 100.0);
	CHECK(lt_nodeeditor_zoom(ne) <= 4.0, "set_zoom clamps to the documented range");
	lt_nodeeditor_pan(ne, 5, 5);
	e = key('0');
	lt_nodeeditor_event(ne, ner, &e);
	int panx, pany;
	lt_nodeeditor_get_pan(ne, &panx, &pany);
	CHECK(lt_nodeeditor_zoom(ne) == 1.0 && panx == 0 && pany == 0, "'0' resets both zoom and pan");
	int n4 = lt_nodeeditor_add(ne, "Fresh", 7, 7);                             /* n1 was dragged earlier in this test; check a node that was never moved */
	lt_nodeeditor_get_pos(ne, n4, &x, &y);
	CHECK(x == 7 && y == 7, "a node's own world position is unaffected by zoom/pan");
	e = mouse(LT_K_WHEEL_UP, ner.x + 5, ner.y + 5);
	CHECK(lt_nodeeditor_event(ne, ner, &e) == LT_W_CHANGED && lt_nodeeditor_zoom(ne) > 1.0, "wheel-up over the canvas zooms in");
	lt_nodeeditor_draw(ne, c, ner, &th, true);                                  /* zoomed: must still draw without crashing */
	lt_nodeeditor_free(ne);

	/* ---- gradient picker ---- */
	lt_gradientpicker *g = lt_gradientpicker_new();
	CHECK(lt_gradientpicker_count(g) == 2, "starts with two stops");
	lt_gradstop s0 = lt_gradientpicker_stop(g, 0), s1 = lt_gradientpicker_stop(g, 1);
	CHECK(s0.pos == 0 && s1.pos == 1, "the default stops sit at the two ends");
	lt_color mid = lt_gradientpicker_sample(g, 0.5);
	CHECK(mid != s0.color && mid != s1.color, "the midpoint is a real blend, not either endpoint");
	CHECK(lt_gradientpicker_sample(g, -1) == s0.color && lt_gradientpicker_sample(g, 2) == s1.color, "sampling outside 0..1 clamps to the endpoints");

	lt_rect gr = {0, 0, 30, 3};
	lt_gradientpicker_draw(g, c, gr, &th, true);
	e = mouse(LT_K_CLICK, gr.x + 15, gr.y);                                     /* the gradient row, away from either endpoint marker */
	lt_wres gres = lt_gradientpicker_event(g, gr, &e);
	CHECK(gres == LT_W_CHANGED && lt_gradientpicker_count(g) == 3, "clicking empty gradient adds a new stop");
	int newsel = lt_gradientpicker_selected(g);
	lt_gradientpicker_set_color(g, LT_RGB(0, 255, 0));
	CHECK(lt_gradientpicker_stop(g, newsel).color == LT_RGB(0, 255, 0), "set_color recolours the selected stop");
	e = mouse(LT_K_DRAG, gr.x + 20, gr.y);
	lt_gradientpicker_event(g, gr, &e);
	CHECK(lt_gradientpicker_stop(g, lt_gradientpicker_selected(g)).pos > 0.5, "dragging a selected stop moves its position");
	e = mouse(LT_K_RELEASE, gr.x + 20, gr.y);
	lt_gradientpicker_event(g, gr, &e);
	lt_gradientpicker_draw(g, c, gr, &th, true);
	e = mouse(LT_K_CLICK, gr.x, gr.y + 1);                                      /* the marker row, at the first stop */
	lt_gradientpicker_event(g, gr, &e);
	CHECK(lt_gradientpicker_selected(g) == 0, "clicking near a marker selects that stop instead of adding one");
	e = key(LT_K_RIGHT);
	double before = lt_gradientpicker_stop(g, 0).pos;
	lt_gradientpicker_event(g, gr, &e);
	CHECK(lt_gradientpicker_stop(g, lt_gradientpicker_selected(g)).pos > before, "Right nudges the selected stop forward");
	int before_n = lt_gradientpicker_count(g);
	e = key(LT_K_DEL);
	lt_gradientpicker_event(g, gr, &e);
	CHECK(lt_gradientpicker_count(g) == before_n - 1, "Delete removes the selected stop");
	while (lt_gradientpicker_count(g) > 2) { e = key(LT_K_DEL); lt_gradientpicker_event(g, gr, &e); }
	e = key(LT_K_DEL);
	lt_gradientpicker_event(g, gr, &e);
	CHECK(lt_gradientpicker_count(g) == 2, "the count never drops below two stops");
	lt_color grad5[5];
	lt_gradientpicker_to_theme_grad(g, grad5, 5);
	CHECK(grad5[0] != LT_DEFAULT && grad5[4] != LT_DEFAULT, "to_theme_grad fills every requested sample");
	lt_gradientpicker_add_stop(g, 0.5, LT_RGB(9, 9, 9));
	e = key('r');
	CHECK(lt_gradientpicker_event(g, gr, &e) == LT_W_CHANGED, "'r' resets the gradient");
	CHECK(lt_gradientpicker_count(g) == 2, "back to two stops");
	lt_gradstop rs0 = lt_gradientpicker_stop(g, 0), rs1 = lt_gradientpicker_stop(g, 1);
	CHECK(rs0.pos == 0 && rs0.color == LT_RGB(0, 0, 0) && rs1.pos == 1 && rs1.color == LT_RGB(255, 255, 255), "...and the original black -> white");
	lt_gradientpicker_free(g);

	/* ---- colour picker ---- */
	lt_colorpicker *cp = lt_colorpicker_new(LT_RGB(255, 0, 0));
	double h, s, v;
	lt_colorpicker_get_hsv(cp, &h, &s, &v);
	CHECK(fabs(h - 0) < 1 && s > 0.99 && v > 0.99, "pure red round-trips to hue 0, full saturation and value");
	CHECK(lt_colorpicker_color(cp) == LT_RGB(255, 0, 0), "and back to the same rgb");
	lt_colorpicker_set_color(cp, LT_RGB(0, 255, 0));
	lt_colorpicker_get_hsv(cp, &h, &s, &v);
	CHECK(fabs(h - 120) < 1, "pure green is hue 120");
	lt_colorpicker_set_color(cp, LT_RGB(128, 128, 128));
	lt_colorpicker_get_hsv(cp, NULL, &s, NULL);
	CHECK(s < 0.01, "a grey has (near) zero saturation");
	lt_colorpicker_set_hsv(cp, 200, 0.5, 0.8);
	lt_rect cpr = {0, 0, 30, 10};
	lt_colorpicker_draw(cp, c, cpr, &th, true);
	e = mouse(LT_K_CLICK, cpr.x + 5, cpr.y + 3);                                  /* inside the SV plane */
	CHECK(lt_colorpicker_event(cp, cpr, &e) == LT_W_CHANGED, "a click on the plane changes s/v");
	lt_colorpicker_get_hsv(cp, &h, NULL, NULL);
	CHECK(fabs(h - 200) < 0.5, "...but leaves the hue alone");
	e = mouse(LT_K_DRAG, cpr.x + 20, cpr.y + 1);
	lt_colorpicker_event(cp, cpr, &e);                                           /* outside the plane, but still "dragging": must not crash, and does nothing since no button is down as far as the widget knows */
	e = mouse(LT_K_RELEASE, cpr.x + 20, cpr.y + 1);
	lt_colorpicker_event(cp, cpr, &e);
	e = mouse(LT_K_CLICK, cpr.x + cpr.w - 2, cpr.y + cpr.h / 2);                    /* the hue bar (the last 3 columns) */
	CHECK(lt_colorpicker_event(cp, cpr, &e) == LT_W_CHANGED, "a click on the hue bar changes the hue");
	lt_colorpicker_get_hsv(cp, &h, NULL, NULL);
	CHECK(fabs(h - 200) > 1, "...and it is a different hue now");
	e = key(LT_K_RIGHT);
	lt_colorpicker_get_hsv(cp, NULL, &s, NULL);
	lt_colorpicker_event(cp, cpr, &e);
	double s2;
	lt_colorpicker_get_hsv(cp, NULL, &s2, NULL);
	CHECK(s2 > s, "Right nudges saturation up from the keyboard");
	e = key('[');
	lt_colorpicker_get_hsv(cp, &h, NULL, NULL);
	lt_colorpicker_event(cp, cpr, &e);
	double h2;
	lt_colorpicker_get_hsv(cp, &h2, NULL, NULL);
	CHECK(h2 != h, "'[' nudges the hue from the keyboard");
	lt_colorpicker_set_hsv(cp, 10, 0.5, NAN);                                   /* NaN must be ignored, not crash / corrupt the state */
	lt_colorpicker_draw(cp, c, (lt_rect){0, 0, 5, 2}, &th, false);              /* too small: must not crash */
	e = key('r');
	CHECK(lt_colorpicker_event(cp, cpr, &e) == LT_W_CHANGED && lt_colorpicker_color(cp) == LT_RGB(255, 0, 0), "'r' resets to the colour given at construction");
	lt_colorpicker_free(cp);

	/* ---- zonebar with a legend: the bar, then one compact horizontal line naming every zone and its percentage ---- */
	lt_zone zones[3] = {{"used", 40, LT_T_ERR}, {"cache", 35, LT_T_WARN}, {"free", 25, LT_T_OK}};
	lt_canvas_clear(c, th.s[LT_T_TEXT]);
	lt_zonebar_ex(c, (lt_rect){0, 0, 40, 2}, &th, zones, 3, true);
	CHECK(lt_canvas_fg(c, 0, 1) != LT_DEFAULT, "the legend line paints a coloured swatch for the first zone");
	char *zd = lt_canvas_dump(c);
	CHECK(strstr(zd, "used") && strstr(zd, "40%") && strstr(zd, "cache") && strstr(zd, "35%") && strstr(zd, "free") && strstr(zd, "25%"), "every zone's label and percentage appear, all on the one legend line");
	free(zd);
	lt_zonebar_ex(c, (lt_rect){0, 0, 40, 1}, &th, zones, 3, true);              /* too short for any legend row: must not crash, just skip it */
	lt_zone zeroz[2] = {{"a", 0, LT_T_OK}, {"b", 0, LT_T_ERR}};
	lt_zonebar_ex(c, (lt_rect){0, 0, 20, 3}, &th, zeroz, 2, true);              /* all-zero weights: must not divide by zero */

	lt_canvas_free(c);
	printf(fails ? "editors: %d failed\n" : "editors: ok\n", fails);
	return fails != 0;
}
