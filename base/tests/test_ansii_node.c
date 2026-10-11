#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#include "arena_alloc.h"
#define NODE_IMPLEMENTATION
#include "node.h"
#define TUI_IMPLEMENTATION
#include "tui.h"
#define ANSII_NODE_IMPLEMENTATION
#include "ansii_node.h"

int main(void)
{
    Arena a;
    Node_Graph g;
    Tui_Buffer buf;
    i32 n0, n1, p_out, p_in;
    i32 x0, y0, x1, y1;

    print_cstr("test_ansii_node\n");

    a.begin = a.end = NULL;
    node_graph_init(&g, &a);
    n0 = node_graph_add_node(&g, 0, 0, "A");
    n1 = node_graph_add_node(&g, 10, 0, "B");
    p_out = node_graph_add_port(&g, n0, "out", NODE_PORT_OUT);
    p_in = node_graph_add_port(&g, n1, "in", NODE_PORT_IN);
    node_graph_connect(&g, n0, p_out, n1, p_in);

    tui_buffer_init(&buf, 40, 10);
    tui_buffer_clear(&buf, TUI_DEFAULT, TUI_DEFAULT);
    ansii_node_render(&g, &buf);

    TEST_CHECK(buf.cells[0].ch == '+', "render: node A's box top-left corner drawn");
    TEST_CHECK(buf.cells[1 * 40 + 1].ch == 'A', "render: node A's title character drawn");
    TEST_CHECK(buf.cells[10].ch == '+', "render: node B's box top-left corner drawn");
    TEST_CHECK(buf.cells[1 * 40 + 11].ch == 'B', "render: node B's title character drawn");

    ansii_node_port_screen_pos(&g, p_out, &x0, &y0);
    ansii_node_port_screen_pos(&g, p_in, &x1, &y1);
    TEST_CHECK(buf.cells[y0 * 40 + x0].ch == '>', "render: OUT port marker '>' drawn on top of the edge");
    TEST_CHECK(buf.cells[y1 * 40 + x1].ch == '<', "render: IN port marker '<' drawn on top of the edge");

    {
        i32 mid_x = x0 + (x1 - x0) / 2;
        i32 has_line = 0;
        i32 y;
        for (y = (y0 < y1 ? y0 : y1); y <= (y0 < y1 ? y1 : y0); y++) {
            if (buf.cells[y * 40 + mid_x].ch == '|' || buf.cells[y * 40 + mid_x].ch == '+') {
                has_line = 1;
            }
        }
        TEST_CHECK(has_line || y0 == y1, "render: an edge line connects the two ports");
    }

    {
        Node_Graph empty_g;
        Tui_Buffer empty_buf;
        node_graph_init(&empty_g, &a);
        tui_buffer_init(&empty_buf, 5, 5);
        tui_buffer_clear(&empty_buf, TUI_DEFAULT, TUI_DEFAULT);
        ansii_node_render(&empty_g, &empty_buf);
        TEST_CHECK(empty_buf.cells[0].ch == ' ', "render: empty graph draws nothing, no crash");
        tui_buffer_free(&empty_buf);
    }

    tui_buffer_free(&buf);
    print_flush();
    return test_failures_ != 0;
}
