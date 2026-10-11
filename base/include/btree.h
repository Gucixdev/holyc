#ifndef HOLYC_BTREE_H_
#define HOLYC_BTREE_H_

/* Minimal behavior tree - the AI decision-making structure (NOT a binary
 * search tree, despite the name/folder). Standard node set: Sequence
 * (runs children in order, fails as soon as one fails), Selector (runs
 * children in order, succeeds as soon as one succeeds), and Leaf (a
 * caller-supplied function pointer doing the actual work).
 *
 * The defining, non-negotiable property of a real behavior tree is that
 * BT_RUNNING is genuinely resumable across ticks: a composite that
 * returns RUNNING because child i is mid-execution remembers i
 * (running_child) so the NEXT tick resumes there directly instead of
 * re-evaluating already-succeeded siblings from scratch. running_child
 * resets to 0 whenever the composite fully resolves (SUCCESS or
 * FAILURE), so the following fresh run starts over correctly. */

#include "base.h"
#include "arena_alloc.h"

typedef enum {
    BT_SUCCESS,
    BT_FAILURE,
    BT_RUNNING
} Bt_Status;

typedef Bt_Status (*Bt_Leaf_Fn)(void *ctx);

typedef enum {
    BT_NODE_LEAF,
    BT_NODE_SEQUENCE,
    BT_NODE_SELECTOR
} Bt_Node_Type;

typedef struct Bt_Node {
    Bt_Node_Type type;
    Bt_Leaf_Fn leaf_fn;          /* BT_NODE_LEAF only */
    struct Bt_Node **children;   /* BT_NODE_SEQUENCE / BT_NODE_SELECTOR only, arena-owned copy */
    i32 child_count;
    i32 running_child;           /* resume index for composites, see file header */
} Bt_Node;

Bt_Node *bt_leaf(Arena *a, Bt_Leaf_Fn fn);
Bt_Node *bt_sequence(Arena *a, Bt_Node **children, i32 count);
Bt_Node *bt_selector(Arena *a, Bt_Node **children, i32 count);
Bt_Status bt_tick(Bt_Node *node, void *ctx);

#endif /* HOLYC_BTREE_H_ */

#ifdef BTREE_IMPLEMENTATION
#ifndef BTREE_IMPLEMENTATION_
#define BTREE_IMPLEMENTATION_

static Bt_Node *bt_node_new_(Arena *a, Bt_Node_Type type)
{
    Bt_Node *n = (Bt_Node *)arena_alloc(a, sizeof(Bt_Node));
    n->type = type;
    n->leaf_fn = 0;
    n->children = 0;
    n->child_count = 0;
    n->running_child = 0;
    return n;
}

Bt_Node *bt_leaf(Arena *a, Bt_Leaf_Fn fn)
{
    Bt_Node *n = bt_node_new_(a, BT_NODE_LEAF);
    n->leaf_fn = fn;
    return n;
}

static Bt_Node *bt_composite_new_(Arena *a, Bt_Node_Type type, Bt_Node **children, i32 count)
{
    Bt_Node *n = bt_node_new_(a, type);
    n->children = (Bt_Node **)arena_memdup(a, children, (size_t)count * sizeof(Bt_Node *));
    n->child_count = count;
    return n;
}

Bt_Node *bt_sequence(Arena *a, Bt_Node **children, i32 count)
{
    return bt_composite_new_(a, BT_NODE_SEQUENCE, children, count);
}

Bt_Node *bt_selector(Arena *a, Bt_Node **children, i32 count)
{
    return bt_composite_new_(a, BT_NODE_SELECTOR, children, count);
}

Bt_Status bt_tick(Bt_Node *node, void *ctx)
{
    if (node->type == BT_NODE_LEAF) {
        return node->leaf_fn(ctx);
    }

    if (node->type == BT_NODE_SEQUENCE) {
        i32 i;
        Bt_Status s;
        for (i = node->running_child; i < node->child_count; i++) {
            s = bt_tick(node->children[i], ctx);
            if (s == BT_RUNNING) {
                node->running_child = i;
                return BT_RUNNING;
            }
            if (s == BT_FAILURE) {
                node->running_child = 0;
                return BT_FAILURE;
            }
        }
        node->running_child = 0;
        return BT_SUCCESS;
    }

    /* BT_NODE_SELECTOR */
    {
        i32 i;
        Bt_Status s;
        for (i = node->running_child; i < node->child_count; i++) {
            s = bt_tick(node->children[i], ctx);
            if (s == BT_RUNNING) {
                node->running_child = i;
                return BT_RUNNING;
            }
            if (s == BT_SUCCESS) {
                node->running_child = 0;
                return BT_SUCCESS;
            }
        }
        node->running_child = 0;
        return BT_FAILURE;
    }
}

#endif /* BTREE_IMPLEMENTATION_ */
#endif /* BTREE_IMPLEMENTATION */
