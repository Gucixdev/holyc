#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#include "arena_alloc.h"
#define NODE_IMPLEMENTATION
#include "node.h"

int main(void)
{
    Arena a;
    Node_Graph g;
    i32 n0, n1, n2;
    i32 p_out, p_in, p_in2, p_out_bad_dir;

    print_cstr("test_node\n");

    a.begin = a.end = NULL;
    node_graph_init(&g, &a);

    n0 = node_graph_add_node(&g, 0, 0, "Source");
    n1 = node_graph_add_node(&g, 10, 0, "Sink");
    TEST_CHECK(n0 == 0 && n1 == 1, "add_node: ids assigned in creation order");

    p_out = node_graph_add_port(&g, n0, "out", NODE_PORT_OUT);
    p_in = node_graph_add_port(&g, n1, "in", NODE_PORT_IN);
    TEST_CHECK(p_out >= 0 && p_in >= 0, "add_port: succeeds on live nodes");
    TEST_CHECK(node_graph_add_port(&g, 999, "bad", NODE_PORT_IN) == -1,
               "add_port: fails on a nonexistent node id");

    TEST_CHECK(node_graph_connect(&g, n0, p_out, n1, p_in) == 1,
               "connect: valid OUT->IN edge succeeds");
    TEST_CHECK(g.edges.count == 1, "connect: edge actually recorded");

    p_in2 = node_graph_add_port(&g, n1, "in2", NODE_PORT_IN);
    TEST_CHECK(node_graph_connect(&g, n0, p_out, n1, p_in2) == 1,
               "connect: one OUT port can fan out to multiple IN ports");

    p_out_bad_dir = node_graph_add_port(&g, n0, "out2", NODE_PORT_OUT);
    TEST_CHECK(node_graph_connect(&g, n1, p_in, n0, p_out_bad_dir) == 0,
               "connect: IN->OUT (reversed) is rejected");
    TEST_CHECK(node_graph_connect(&g, n0, p_out, n0, p_out_bad_dir) == 0,
               "connect: OUT->OUT is rejected");
    TEST_CHECK(node_graph_connect(&g, n0, p_out, n1, 999) == 0,
               "connect: nonexistent port id is rejected");
    TEST_CHECK(g.edges.count == 2, "connect: rejected attempts did not add edges");

    n2 = node_graph_add_node(&g, 20, 20, "Unrelated");
    TEST_CHECK(node_graph_node_is_alive(&g, n2), "third node starts alive");

    node_graph_remove_node(&g, n1);
    TEST_CHECK(!node_graph_node_is_alive(&g, n1), "remove_node: node marked dead");
    TEST_CHECK(!node_graph_port_is_alive(&g, p_in), "remove_node: its ports marked dead too");
    TEST_CHECK(!g.edges.items[0].alive, "remove_node: edges touching it marked dead");
    TEST_CHECK(!g.edges.items[1].alive, "remove_node: fan-out edge touching it marked dead too");
    TEST_CHECK(node_graph_node_is_alive(&g, n0), "remove_node: unrelated node n0 still alive");
    TEST_CHECK(node_graph_port_is_alive(&g, p_out), "remove_node: unrelated port still alive");
    TEST_CHECK(node_graph_node_is_alive(&g, n2), "remove_node: unrelated node n2 still alive");

    TEST_CHECK(node_graph_connect(&g, n0, p_out, n1, p_in) == 0,
               "connect: cannot connect through a dead port after removal");

    print_flush();
    return test_failures_ != 0;
}
