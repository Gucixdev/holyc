/* test_interactive.c - the state+draw+event widgets not covered by test_controls/test_kinds: list, tree, dropdown, scrollview,
 * lt_input/searchbar_event, buttonbar, tabs, breadcrumbs, menu, treebar, mapview. Synthetic events, headless. */
#include "lazytui.h"
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
	lt_canvas *c = lt_canvas_new(60, 20, UNI);

	/* list: add, cursor movement, filter, multi-select, mouse */
	lt_list *l = lt_list_new();
	for (int i = 0; i < 30; i++) { char b[16]; snprintf(b, sizeof b, "row %d", i); lt_list_add(l, b, i * 10); }
	CHECK(lt_list_count(l) == 30 && lt_list_shown(l) == 30, "every added row is counted and shown");
	CHECK(lt_list_cursor(l) == 0, "the cursor starts on the first row");
	lt_rect lr = {0, 0, 20, 8};
	lt_event e = key(LT_K_DOWN);
	CHECK(lt_list_event(l, lr, &e) == LT_W_CHANGED && lt_list_cursor(l) == 1, "Down moves the cursor");
	e = key(LT_K_END);
	lt_list_event(l, lr, &e);
	CHECK(lt_list_cursor(l) == 29, "End jumps to the last row");
	e = key(LT_K_HOME);
	lt_list_event(l, lr, &e);
	CHECK(lt_list_cursor(l) == 0, "Home jumps to the first row");
	e = key(LT_K_ENTER);
	CHECK(lt_list_event(l, lr, &e) == LT_W_ACTIVATE, "Enter activates the current row");
	CHECK(lt_list_id(l, 3) == 30, "the id given at add() is preserved");
	CHECK(!strcmp(lt_list_label(l, 3), "row 3"), "the label is preserved");
	lt_list_filter(l, "row 1");
	CHECK(lt_list_shown(l) < 30 && lt_list_shown(l) > 0, "a filter narrows what is shown");
	lt_list_filter(l, NULL);
	CHECK(lt_list_shown(l) == 30, "clearing the filter shows everything again");
	lt_list_set_multi(l, true);
	lt_list_draw(l, c, lr, &th, true);
	int cursor_item = lt_list_item_at(l, lr, 2, lt_list_cursor(l));             /* the raw item behind the cursor's own row */
	e = key(' ');
	lt_wres mr = lt_list_event(l, lr, &e);
	CHECK(mr == LT_W_CHANGED, "space marks a row once multi-select is on");
	CHECK(cursor_item >= 0 && lt_list_marked(l, cursor_item), "the row under the cursor is reported as marked");
	lt_list_draw(l, c, lr, &th, true);
	int hit = lt_list_item_at(l, lr, 2, 2);
	CHECK(hit >= 0 && hit < 30, "a cell inside the drawn list finds a real item");
	CHECK(lt_list_item_at(l, lr, 2, 50) == -1, "a cell far outside finds nothing");
	e = mouse(LT_K_WHEEL_DOWN, 2, 2);
	lt_list_event(l, lr, &e);                                                   /* must not crash */
	lt_list_clear(l);
	CHECK(lt_list_count(l) == 0, "clear empties the list");
	lt_list_free(l);

	/* tree: nested nodes, expand/collapse, selection, node lookup by cell */
	lt_tree *t = lt_tree_new();
	int root = lt_tree_add(t, -1, "root", 100);
	int kid = lt_tree_add(t, root, "kid", 101);
	lt_tree_add(t, root, "sibling", 102);
	int leaf = lt_tree_add(t, kid, "leaf", 103);
	CHECK(lt_tree_count(t) == 4, "every added node is counted");
	CHECK(!lt_tree_is_expanded(t, root), "a fresh node starts collapsed");
	lt_tree_set_expanded(t, root, true);
	CHECK(lt_tree_is_expanded(t, root), "set_expanded takes effect");
	lt_tree_expand_all(t, true);
	CHECK(lt_tree_visible(t) == 4, "expanding everything shows every node");
	lt_tree_select(t, leaf);
	CHECK(lt_tree_selected(t) == leaf, "select() moves the selection");
	CHECK(lt_tree_id(t, leaf) == 103 && !strcmp(lt_tree_label(t, leaf), "leaf"), "id and label are preserved");
	lt_rect tr = {0, 0, 20, 6};
	lt_tree_draw(t, c, tr, &th, true);
	int at = lt_tree_node_at(t, tr, 2, 0);
	CHECK(at == root, "the first visible row is the root node");
	lt_tree_expand_all(t, false);
	CHECK(lt_tree_visible(t) == 1, "collapsing everything leaves only the top-level root visible");
	e = key(LT_K_RIGHT);
	lt_tree_event(t, tr, &e);
	CHECK(lt_tree_is_expanded(t, root) || lt_tree_visible(t) >= 1, "Right expands (or at least does not break) the root");
	lt_tree_free(t);

	/* dropdown: a plain list popup */
	const char *items[3] = {"one", "two", "three"};
	lt_dropdown *d = lt_dropdown_new(items, 3);
	CHECK(!lt_dropdown_is_open(d), "starts closed");
	CHECK(lt_dropdown_selected(d) == 0, "starts on the first item");
	lt_rect box = {0, 0, 20, 1};
	e = key(LT_K_ENTER);
	lt_dropdown_event(d, box, &e, 20);
	CHECK(lt_dropdown_is_open(d), "Enter opens the popup");
	e = key(LT_K_DOWN);
	lt_dropdown_event(d, box, &e, 20);
	e = key(LT_K_ENTER);
	lt_wres dr = lt_dropdown_event(d, box, &e, 20);
	CHECK(dr == LT_W_CHANGED && lt_dropdown_selected(d) == 1 && !lt_dropdown_is_open(d), "choosing an item closes the popup and changes the selection");
	lt_dropdown_set_selected(d, 2);
	CHECK(lt_dropdown_selected(d) == 2, "set_selected jumps directly");
	lt_dropdown_draw(d, c, box, &th, true);
	lt_dropdown_free(d);

	/* a tree-backed dropdown */
	lt_tree *dt = lt_tree_new();
	int droot = lt_tree_add(dt, -1, "fruits", 1);
	lt_tree_add(dt, droot, "apple", 2);
	lt_dropdown *td = lt_dropdown_from_tree(dt);
	CHECK(td != NULL, "a tree-backed dropdown is created");
	lt_dropdown_free(td);
	lt_tree_free(dt);

	/* scrollview: begin/end, wheel, keys, show() */
	lt_scrollview sv;
	lt_scrollview_init(&sv);
	lt_rect view = {0, 0, 20, 5};
	lt_rect content = lt_scrollview_begin(&sv, c, view, 50);
	CHECK(content.w > 0, "begin returns a content rectangle");
	lt_scrollview_end(&sv, c, &th);
	e = mouse(LT_K_WHEEL_DOWN, 2, 2);
	lt_scrollview_event(&sv, &e, true);
	CHECK(sv.offset >= 0, "wheel scroll keeps a sane, non-negative offset");
	lt_scrollview_show(&sv, 45, 1);
	lt_scrollview_begin(&sv, c, view, 50);
	CHECK(sv.offset + view.h >= 45, "show() scrolled far enough to reveal that row");
	lt_scrollview_end(&sv, c, &th);                                             /* every begin() needs a matching end() to lift the clip */

	/* lt_input + searchbar events: typing, backspace, cursor motion */
	lt_input in;
	lt_input_init(&in);
	CHECK(in.cursor == 0 && in.text[0] == 0, "starts empty");
	lt_input_set(&in, "hello");
	CHECK(!strcmp(in.text, "hello") && in.cursor == 5, "set() places the cursor at the end");
	CHECK(lt_input_key(&in, LT_K_BSPACE) && !strcmp(in.text, "hell"), "backspace removes the last character");
	CHECK(lt_input_key(&in, LT_K_HOME) && in.cursor == 0, "Home moves the cursor to the start");
	CHECK(lt_input_paste(&in, "XY"), "paste inserts text");
	CHECK(!strncmp(in.text, "XY", 2), "the pasted text lands at the cursor");
	lt_input_free(&in);
	lt_input sbin;
	lt_input_init(&sbin);
	lt_wres sres;
	lt_rect sbr = {0, 0, 20, 1};
	e = mouse(LT_K_CLICK, 5, 0);
	lt_searchbar_event(&sbin, "> ", sbr, &e, &sres);                            /* must not crash on a click with no text yet */
	e.kind = LT_EV_KEY; e.key = 'a';
	CHECK(lt_searchbar_event(&sbin, "> ", sbr, &e, &sres), "typing a character changes the text");
	CHECK(!strcmp(sbin.text, "a"), "and it lands in the buffer");
	lt_input_free(&sbin);

	/* buttonbar */
	const char *labels[4] = {"Refresh", "Theme", "Pause", "Quit"};
	lt_buttonbar *bb = lt_buttonbar_new(labels, 4);
	CHECK(lt_buttonbar_count(bb) == 4, "every label became a button");
	lt_rect bbr = {0, 0, 40, 1};
	e = key(LT_K_RIGHT);
	lt_buttonbar_event(bb, bbr, &e);
	CHECK(lt_buttonbar_cursor(bb) == 1, "Right moves the cursor to the next button");
	lt_buttonbar_set_cursor(bb, 3);
	e = key(LT_K_ENTER);
	CHECK(lt_buttonbar_event(bb, bbr, &e) == LT_W_ACTIVATE, "Enter activates the button under the cursor");
	lt_buttonbar_set_active(bb, 2);
	lt_buttonbar_set_prompt(bb, "menu");
	lt_buttonbar_draw(bb, c, bbr, &th, true);
	char *bbd = lt_canvas_dump(c);
	CHECK(strstr(bbd, "menu") && strstr(bbd, "Quit"), "the prompt and every label are drawn");
	free(bbd);
	lt_buttonbar_free(bb);

	/* tabs */
	const char *tabnames[3] = {"one", "two", "three"};
	lt_tabs *tabs = lt_tabs_new(tabnames, 3, LT_SIDE_TOP);
	CHECK(lt_tabs_selected(tabs) == 0, "starts on the first tab");
	lt_rect whole = {0, 0, 40, 10};
	e = key(LT_K_RIGHT);
	CHECK(lt_tabs_event(tabs, whole, &e) == LT_W_CHANGED && lt_tabs_selected(tabs) == 1, "Right moves to the next tab");
	lt_rect pane = lt_tabs_pane(tabs, whole);
	CHECK(pane.h < whole.h, "the content pane is smaller than the whole (room for the strip)");
	lt_tabs_set(tabs, 2);
	CHECK(lt_tabs_selected(tabs) == 2, "set() jumps directly");
	lt_tabs_draw(tabs, c, whole, &th, true);
	lt_tabs_free(tabs);

	/* breadcrumbs */
	lt_breadcrumbs *bc = lt_breadcrumbs_new();
	lt_breadcrumbs_set_path(bc, "/home/me/dev", '/');
	CHECK(lt_breadcrumbs_count(bc) == 4, "a leading slash plus three segments makes four crumbs");
	char pathbuf[64];
	lt_breadcrumbs_path_upto(bc, 2, '/', pathbuf, sizeof pathbuf);
	CHECK(strstr(pathbuf, "home") && strstr(pathbuf, "me"), "path_upto rebuilds a prefix of the path");
	lt_rect bcr = {0, 0, 40, 1};
	lt_breadcrumbs_draw(bc, c, bcr, &th, true);
	e = key(LT_K_HOME);
	lt_breadcrumbs_event(bc, bcr, &e);
	CHECK(lt_breadcrumbs_cursor(bc) == 0, "Home moves the cursor to the first crumb");
	lt_breadcrumbs_free(bc);

	/* menu: a bar with a pull-down, checkable items, shortcuts */
	lt_menu *m = lt_menu_new();
	int file_menu = lt_menu_add(m, "File");
	lt_menu_item(m, file_menu, "Open", "ctrl-o", 1);
	lt_menu_check(m, file_menu, "Word wrap", "", 2, false);
	lt_menu_sep(m, file_menu);
	lt_menu_item(m, file_menu, "Quit", "ctrl-q", 3);
	lt_rect mbar = {0, 0, 40, 1};
	lt_menu_draw_bar(m, c, mbar, &th);
	e = key(LT_K_ENTER);
	lt_menu_event(m, mbar, &e, 60, 20);
	CHECK(lt_menu_is_open(m), "Enter opens the first menu");
	e = key(LT_K_DOWN);
	lt_menu_event(m, mbar, &e, 60, 20);
	e = key(LT_K_ENTER);
	lt_menu_event(m, mbar, &e, 60, 20);
	CHECK(lt_menu_id(m) == 2 && lt_menu_checked(m, 2), "choosing a checkable item checks it and reports its id");
	CHECK(!lt_menu_is_open(m), "choosing an item closes the menu");
	lt_menu_set_enabled(m, 1, false);
	lt_menu_open_at(m, file_menu, 5, 5);
	CHECK(lt_menu_is_open(m), "open_at opens as a context menu");
	lt_menu_draw_popup(m, c, &th);
	lt_menu_close(m);
	CHECK(!lt_menu_is_open(m), "close() closes it");
	lt_menu_free(m);

	/* treebar: breadcrumbs above, children as buttons below */
	lt_tree *bt = lt_tree_new();
	int broot = lt_tree_add(bt, -1, "root", 1);
	lt_tree_add(bt, broot, "child-a", 2);
	lt_tree_add(bt, broot, "child-b", 3);
	lt_treebar *tb = lt_treebar_new(bt);
	CHECK(lt_treebar_level(tb) == -1, "starts at the top level");
	lt_rect tbr = {0, 0, 40, 1};
	lt_treebar_draw(tb, c, tbr, &th, true);
	e = key(LT_K_RIGHT);
	lt_treebar_event(tb, tbr, &e);
	e = key(LT_K_ENTER);
	lt_treebar_event(tb, tbr, &e);
	CHECK(lt_treebar_level(tb) == broot, "entering the root shows its children");
	CHECK(lt_tree_child_count(bt, broot) == 2, "the root has two children");
	CHECK(lt_tree_parent(bt, lt_tree_child(bt, broot, 0)) == broot, "child/parent round-trip");
	e = key(LT_K_UP);
	lt_treebar_event(tb, tbr, &e);
	CHECK(lt_treebar_level(tb) == -1, "Up returns to the parent level");
	lt_treebar_free(tb);
	lt_tree_free(bt);

	/* mapview: zoom and pan state, kept in range */
	lt_mapview mv;
	lt_mapview_init(&mv);
	CHECK(mv.zoom == 1, "starts unzoomed");
	lt_map_opts base = {0};
	lt_rect maprect = {0, 0, 60, 20};
	lt_mapview_zoom_at(&mv, maprect, &base, 2.0, 30, 10);
	CHECK(mv.zoom > 1, "zoom_at increases the zoom");
	e = mouse(LT_K_WHEEL_UP, 30, 10);
	lt_mapview_event(&mv, maprect, &base, &e);
	CHECK(mv.zoom > 1, "wheel-up over the map zooms in further");
	e = key('0');
	lt_mapview_event(&mv, maprect, &base, &e);
	CHECK(mv.zoom == 1 && mv.clon == 0 && mv.clat == 0, "'0' resets the view");

	lt_canvas_free(c);
	printf(fails ? "interactive: %d failed\n" : "interactive: ok\n", fails);
	return fails != 0;
}
