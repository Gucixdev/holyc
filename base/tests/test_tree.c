#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#define TREE_IMPLEMENTATION
#include "arena_alloc.h"
#include "tree.h"

static i32 collect_(Tree_Node *node, void *ctx)
{
    i32 **out = (i32 **)ctx;
    **out = *(i32 *)node->data;
    (*out)++;
    return 1;
}

static i32 stop_at_second_(Tree_Node *node, void *ctx)
{
    i32 *count = (i32 *)ctx;
    (void)node;
    (*count)++;
    return *count < 2;
}

int main(void)
{
    Arena a = {0};
    Tree_Node *root, *child_a, *child_b, *grandchild;
    i32 root_v = 1, a_v = 2, b_v = 3, gc_v = 4;

    print_cstr("test_tree\n");

    root = tree_node_create(&a, &root_v);
    TEST_CHECK(tree_child_count(root) == 0, "a freshly created node has zero children");

    child_a = tree_node_create(&a, &a_v);
    child_b = tree_node_create(&a, &b_v);
    tree_add_child(root, child_a);
    tree_add_child(root, child_b);
    TEST_CHECK(tree_child_count(root) == 2, "adding two children brings the count to 2");
    TEST_CHECK(child_a->parent == root && child_b->parent == root, "both children's parent pointer is set correctly");
    TEST_CHECK(root->first_child == child_a, "first_child is the first-added child");
    TEST_CHECK(child_a->next_sibling == child_b, "next_sibling chains to the second-added child");

    grandchild = tree_node_create(&a, &gc_v);
    tree_add_child(child_a, grandchild);
    TEST_CHECK(tree_child_count(child_a) == 1, "child_a now has one child of its own");
    TEST_CHECK(grandchild->parent == child_a, "the grandchild's parent is child_a, not root");

    {
        i32 seen[4];
        i32 *cursor = seen;
        i32 ok;
        tree_walk_preorder(root, collect_, &cursor);
        ok = (cursor - seen) == 4;
        TEST_CHECK(ok, "pre-order walk visits all 4 nodes (root + 2 children + 1 grandchild)");
        TEST_CHECK(seen[0] == 1, "pre-order walk visits the root first");
        TEST_CHECK(seen[1] == 2, "pre-order walk visits child_a (and its subtree) before child_b");
        TEST_CHECK(seen[2] == 4, "pre-order walk descends into child_a's own child (the grandchild) before moving to child_b");
        TEST_CHECK(seen[3] == 3, "pre-order walk visits child_b last");
    }

    {
        i32 count = 0;
        i32 all_visited = tree_walk_preorder(root, stop_at_second_, &count);
        TEST_CHECK(!all_visited, "a visit callback returning 0 stops the walk early - tree_walk_preorder reports incomplete");
        TEST_CHECK(count == 2, "the walk stopped right after the second node was visited, not before or later");
    }

    arena_free(&a);
    print_flush();
    return test_failures_ != 0;
}
