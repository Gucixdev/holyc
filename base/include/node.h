#ifndef HOLYC_NODE_H_
#define HOLYC_NODE_H_

/* Generic, rendering-agnostic node/port/edge graph - the data model
 * behind a Blueprint/Obsidian-canvas/Blender-shader-nodes/draw.io style
 * node-graph editor. Deliberately split from its renderer: this file
 * only knows about nodes, ports and edges and their validity rules;
 * src/ANSII/ansii_node.h draws a Node_Graph onto a terminal cell buffer,
 * a future GUI backend could draw the same graph a different way without
 * this file changing at all.
 *
 * Arena-allocated, growable via arena_da_append (see arena_alloc.h) - no
 * malloc/free, same convention as every other module here. Ids are
 * indices into their owning array, not pointers: pointers into a
 * growable arena_da_append array are invalidated by reallocation on
 * every growth, indices are not. */

#include "arena_alloc.h"

typedef enum {
    NODE_PORT_IN = 0,
    NODE_PORT_OUT = 1
} Node_Port_Dir;

typedef struct {
    i32 node_id;   /* owning node's index into Node_Graph.nodes */
    const char *name;
    Node_Port_Dir dir;
    i32 alive;     /* 0 once removed (see node_graph_remove_node) - kept as
                    * a tombstone, not compacted out, so every OTHER port's
                    * id stays a stable index into Node_Graph.ports */
} Node_Port;

typedef struct {
    i32 id;
    f32 x, y;
    const char *title;
    i32 alive;     /* 0 once removed - same tombstone reasoning as Node_Port */
} Node;

typedef struct {
    i32 from_node_id, from_port_id; /* from_port_id must be an OUT port */
    i32 to_node_id, to_port_id;     /* to_port_id must be an IN port */
    i32 alive;
} Node_Edge;

typedef struct { Node *items; size_t count, capacity; } Node_List_;
typedef struct { Node_Port *items; size_t count, capacity; } Node_Port_List_;
typedef struct { Node_Edge *items; size_t count, capacity; } Node_Edge_List_;

typedef struct {
    Arena *a;
    Node_List_ nodes;
    Node_Port_List_ ports;
    Node_Edge_List_ edges;
} Node_Graph;

void node_graph_init(Node_Graph *g, Arena *a);

/* returns the new node's id (index into g->nodes - stable even after
 * later arena_da_append growth, since it's an index, not a pointer). */
i32 node_graph_add_node(Node_Graph *g, f32 x, f32 y, const char *title);

/* returns the new port's id. node_id must refer to a live node. */
i32 node_graph_add_port(Node_Graph *g, i32 node_id, const char *name, Node_Port_Dir dir);

/* connects an OUT port to an IN port. Returns 1 on success, 0 if either
 * port id is invalid/dead, or if the direction pairing is wrong (an edge
 * must go OUT -> IN; two OUT ports or two IN ports is rejected, not
 * silently allowed - a node graph where data could flow into another
 * output or out of another input isn't a graph anyone can reason about). */
i32 node_graph_connect(Node_Graph *g, i32 from_node_id, i32 from_port_id,
                        i32 to_node_id, i32 to_port_id);

/* marks node_id (and every port on it, and every edge touching any of
 * those ports) dead. Ids already handed out for other still-live nodes/
 * ports/edges remain valid and unaffected - this is a tombstone, not a
 * compaction, so nothing else needs to be told its id moved. */
void node_graph_remove_node(Node_Graph *g, i32 node_id);

i32 node_graph_port_is_alive(const Node_Graph *g, i32 port_id);
i32 node_graph_node_is_alive(const Node_Graph *g, i32 node_id);

#endif /* HOLYC_NODE_H_ */

#ifdef NODE_IMPLEMENTATION
#ifndef NODE_IMPLEMENTATION_
#define NODE_IMPLEMENTATION_

void node_graph_init(Node_Graph *g, Arena *a)
{
    g->a = a;
    g->nodes.items = NULL; g->nodes.count = 0; g->nodes.capacity = 0;
    g->ports.items = NULL; g->ports.count = 0; g->ports.capacity = 0;
    g->edges.items = NULL; g->edges.count = 0; g->edges.capacity = 0;
}

i32 node_graph_add_node(Node_Graph *g, f32 x, f32 y, const char *title)
{
    Node n;
    n.id = (i32)g->nodes.count;
    n.x = x;
    n.y = y;
    n.title = title;
    n.alive = 1;
    arena_da_append(g->a, &g->nodes, n);
    return n.id;
}

i32 node_graph_node_is_alive(const Node_Graph *g, i32 node_id)
{
    if (node_id < 0 || (size_t)node_id >= g->nodes.count) return 0;
    return g->nodes.items[node_id].alive;
}

i32 node_graph_port_is_alive(const Node_Graph *g, i32 port_id)
{
    if (port_id < 0 || (size_t)port_id >= g->ports.count) return 0;
    return g->ports.items[port_id].alive;
}

i32 node_graph_add_port(Node_Graph *g, i32 node_id, const char *name, Node_Port_Dir dir)
{
    Node_Port p;
    if (!node_graph_node_is_alive(g, node_id)) return -1;
    p.node_id = node_id;
    p.name = name;
    p.dir = dir;
    p.alive = 1;
    arena_da_append(g->a, &g->ports, p);
    return (i32)(g->ports.count - 1);
}

i32 node_graph_connect(Node_Graph *g, i32 from_node_id, i32 from_port_id,
                        i32 to_node_id, i32 to_port_id)
{
    Node_Edge e;
    Node_Port *fp, *tp;

    if (!node_graph_port_is_alive(g, from_port_id) || !node_graph_port_is_alive(g, to_port_id)) {
        return 0;
    }
    fp = &g->ports.items[from_port_id];
    tp = &g->ports.items[to_port_id];
    if (fp->node_id != from_node_id || tp->node_id != to_node_id) return 0;
    if (fp->dir != NODE_PORT_OUT || tp->dir != NODE_PORT_IN) return 0;

    e.from_node_id = from_node_id;
    e.from_port_id = from_port_id;
    e.to_node_id = to_node_id;
    e.to_port_id = to_port_id;
    e.alive = 1;
    arena_da_append(g->a, &g->edges, e);
    return 1;
}

void node_graph_remove_node(Node_Graph *g, i32 node_id)
{
    size_t i;

    if (!node_graph_node_is_alive(g, node_id)) return;
    g->nodes.items[node_id].alive = 0;

    for (i = 0; i < g->ports.count; i++) {
        if (g->ports.items[i].node_id == node_id) {
            g->ports.items[i].alive = 0;
        }
    }
    for (i = 0; i < g->edges.count; i++) {
        Node_Edge *e = &g->edges.items[i];
        if (e->from_node_id == node_id || e->to_node_id == node_id) {
            e->alive = 0;
        }
    }
}

#endif /* NODE_IMPLEMENTATION_ */
#endif /* NODE_IMPLEMENTATION */
