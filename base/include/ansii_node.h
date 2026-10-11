#ifndef HOLYC_ANSII_NODE_H_
#define HOLYC_ANSII_NODE_H_

/* Terminal renderer for a node.h Node_Graph - the Unreal Blueprint /
 * Obsidian canvas / Blender shader-node / draw.io style node-graph
 * editor, drawn onto a tui.h Tui_Buffer. Deliberately just a renderer:
 * all graph state/validity lives in node.h, this file only turns
 * (x, y, title, ports, edges) into cells.
 *
 * Node box: top-left corner at the node's (x, y) (truncated to i32 cell
 * coordinates), sized to fit its title with a 1-cell border on each side
 * (box width = title length + 2, box height = 3 - top border, one title
 * row, bottom border). Ports are drawn as '<' (IN, left edge) or '>'
 * (OUT, right edge) markers on the node's own row, one per port, in the
 * order they were added - a real per-project-editor layout would need
 * to handle overflow/scrolling for many ports; not attempted here, this
 * is the first pass.
 *
 * Edges are routed orthogonally (horizontal run from the source port,
 * then a vertical run, then a horizontal run into the destination port)
 * using '-' and '|' characters and a '+' at the turn - simple, always
 * terminates, good enough for a terminal; not a real curve renderer. */

#include "node.h"
#include "tui.h"

/* returns the screen (x, y) of port_id's marker cell - the position
 * ansii_node_render itself draws it at, exposed so a caller building
 * more graphs on top (or writing tests) doesn't have to re-derive the
 * same layout math. */
void ansii_node_port_screen_pos(const Node_Graph *g, i32 port_id, i32 *out_x, i32 *out_y);

void ansii_node_render(const Node_Graph *g, Tui_Buffer *buf);

#endif /* HOLYC_ANSII_NODE_H_ */

#ifdef ANSII_NODE_IMPLEMENTATION
#ifndef ANSII_NODE_IMPLEMENTATION_
#define ANSII_NODE_IMPLEMENTATION_

static i32 ansii_node_box_width_(const char *title)
{
    return (i32)arena_strlen(title) + 2;
}

static i32 ansii_node_port_index_on_node_(const Node_Graph *g, i32 port_id)
{
    i32 idx = 0;
    size_t i;
    i32 node_id = g->ports.items[port_id].node_id;

    for (i = 0; i < (size_t)port_id; i++) {
        if (g->ports.items[i].node_id == node_id && g->ports.items[i].alive) idx++;
    }
    return idx;
}

void ansii_node_port_screen_pos(const Node_Graph *g, i32 port_id, i32 *out_x, i32 *out_y)
{
    const Node_Port *p = &g->ports.items[port_id];
    const Node *n = &g->nodes.items[p->node_id];
    i32 row = ansii_node_port_index_on_node_(g, port_id);
    i32 box_w = ansii_node_box_width_(n->title);

    *out_y = (i32)n->y + 1 + row;
    *out_x = (p->dir == NODE_PORT_IN) ? (i32)n->x : (i32)n->x + box_w - 1;
}

static void ansii_node_draw_box_(Tui_Buffer *buf, i32 x, i32 y, i32 w, const char *title)
{
    i32 i;
    size_t tlen = arena_strlen(title);

    tui_buffer_set_cell(buf, x, y, '+', TUI_DEFAULT, TUI_DEFAULT);
    tui_buffer_set_cell(buf, x + w - 1, y, '+', TUI_DEFAULT, TUI_DEFAULT);
    for (i = 1; i < w - 1; i++) {
        tui_buffer_set_cell(buf, x + i, y, '-', TUI_DEFAULT, TUI_DEFAULT);
        tui_buffer_set_cell(buf, x + i, y + 2, '-', TUI_DEFAULT, TUI_DEFAULT);
    }
    tui_buffer_set_cell(buf, x, y + 2, '+', TUI_DEFAULT, TUI_DEFAULT);
    tui_buffer_set_cell(buf, x + w - 1, y + 2, '+', TUI_DEFAULT, TUI_DEFAULT);

    tui_buffer_set_cell(buf, x, y + 1, '|', TUI_DEFAULT, TUI_DEFAULT);
    tui_buffer_set_cell(buf, x + w - 1, y + 1, '|', TUI_DEFAULT, TUI_DEFAULT);
    for (i = 0; i < (i32)tlen; i++) {
        tui_buffer_set_cell(buf, x + 1 + i, y + 1, (u32)(u8)title[i], TUI_DEFAULT, TUI_DEFAULT);
    }
}

static void ansii_node_draw_edge_(Tui_Buffer *buf, i32 x0, i32 y0, i32 x1, i32 y1)
{
    i32 x, mid_x;

    if (y0 == y1) {
        i32 lo = (x0 < x1) ? x0 : x1;
        i32 hi = (x0 < x1) ? x1 : x0;
        for (x = lo; x <= hi; x++) tui_buffer_set_cell(buf, x, y0, '-', TUI_DEFAULT, TUI_DEFAULT);
        return;
    }

    mid_x = x0 + (x1 - x0) / 2;
    {
        i32 lo = (x0 < mid_x) ? x0 : mid_x;
        i32 hi = (x0 < mid_x) ? mid_x : x0;
        for (x = lo; x <= hi; x++) tui_buffer_set_cell(buf, x, y0, '-', TUI_DEFAULT, TUI_DEFAULT);
    }
    {
        i32 y, lo_y = (y0 < y1) ? y0 : y1, hi_y = (y0 < y1) ? y1 : y0;
        for (y = lo_y; y <= hi_y; y++) tui_buffer_set_cell(buf, mid_x, y, '|', TUI_DEFAULT, TUI_DEFAULT);
    }
    {
        i32 lo = (mid_x < x1) ? mid_x : x1;
        i32 hi = (mid_x < x1) ? x1 : mid_x;
        for (x = lo; x <= hi; x++) tui_buffer_set_cell(buf, x, y1, '-', TUI_DEFAULT, TUI_DEFAULT);
    }
    tui_buffer_set_cell(buf, mid_x, y0, '+', TUI_DEFAULT, TUI_DEFAULT);
    tui_buffer_set_cell(buf, mid_x, y1, '+', TUI_DEFAULT, TUI_DEFAULT);
}

void ansii_node_render(const Node_Graph *g, Tui_Buffer *buf)
{
    size_t i;

    for (i = 0; i < g->nodes.count; i++) {
        const Node *n = &g->nodes.items[i];
        if (!n->alive) continue;
        ansii_node_draw_box_(buf, (i32)n->x, (i32)n->y, ansii_node_box_width_(n->title), n->title);
    }

    /* edges before ports: an edge's endpoints land exactly on its two
     * ports' own cells, so ports must be drawn AFTER (on top of) edges
     * or their '<'/'>' marker would get overwritten by the edge's '-'/
     * '|'/'+' at that same cell. */
    for (i = 0; i < g->edges.count; i++) {
        const Node_Edge *e = &g->edges.items[i];
        i32 x0, y0, x1, y1;
        if (!e->alive) continue;
        ansii_node_port_screen_pos(g, e->from_port_id, &x0, &y0);
        ansii_node_port_screen_pos(g, e->to_port_id, &x1, &y1);
        ansii_node_draw_edge_(buf, x0, y0, x1, y1);
    }

    for (i = 0; i < g->ports.count; i++) {
        const Node_Port *p = &g->ports.items[i];
        i32 px, py;
        if (!p->alive) continue;
        ansii_node_port_screen_pos(g, (i32)i, &px, &py);
        tui_buffer_set_cell(buf, px, py, (p->dir == NODE_PORT_IN) ? '<' : '>', TUI_DEFAULT, TUI_DEFAULT);
    }
}

#endif /* ANSII_NODE_IMPLEMENTATION_ */
#endif /* ANSII_NODE_IMPLEMENTATION */
