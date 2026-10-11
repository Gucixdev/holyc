#ifndef HOLYC_TREE_H_
#define HOLYC_TREE_H_

/* Generic N-ary tree, arena-allocated nodes, void* payload - the
 * general-purpose tree this project didn't have one of yet (tree/
 * currently holds only merkletree.h, a specific hash-tree algorithm,
 * not a general container; ast.h, same directory, is this file's own
 * specialization for representing PARSED EXPRESSIONS specifically -
 * this file is the plain, payload-agnostic version underneath it).
 * Children are stored as first-child/next-sibling links (not a
 * dynamic array of child pointers) - the classic technique for an
 * arbitrary-arity tree with no upper bound on children-per-node
 * declared anywhere, at the cost of O(children) sibling-walk instead
 * of O(1) indexed child access; this project's own use cases (walking
 * a whole tree, not random-accessing the Nth child) never need the
 * latter. */

#include "arena_alloc.h"

typedef struct Tree_Node {
    struct Tree_Node *parent;
    struct Tree_Node *first_child;
    struct Tree_Node *next_sibling;
    void *data;
} Tree_Node;

Tree_Node *tree_node_create(Arena *a, void *data);

/* appends `child` after parent's existing children (O(children) to
 * find the current last child - see this file's own header comment on
 * why first-child/next-sibling was chosen anyway). child->parent is
 * set to `parent`. */
void tree_add_child(Tree_Node *parent, Tree_Node *child);

i32 tree_child_count(const Tree_Node *node);

/* pre-order depth-first walk starting at `root` (root visited first,
 * then each subtree left to right) - visit returns 0 to stop the walk
 * early, nonzero to continue. Returns 0 if `visit` ever returned 0,
 * else 1 (the whole tree was visited). */
i32 tree_walk_preorder(Tree_Node *root, i32 (*visit)(Tree_Node *node, void *ctx), void *ctx);

#endif /* HOLYC_TREE_H_ */

#ifdef TREE_IMPLEMENTATION
#ifndef TREE_IMPLEMENTATION_
#define TREE_IMPLEMENTATION_

Tree_Node *tree_node_create(Arena *a, void *data)
{
    Tree_Node *n = (Tree_Node *)arena_alloc(a, sizeof(Tree_Node));
    n->parent = NULL;
    n->first_child = NULL;
    n->next_sibling = NULL;
    n->data = data;
    return n;
}

void tree_add_child(Tree_Node *parent, Tree_Node *child)
{
    child->parent = parent;
    child->next_sibling = NULL;

    if (!parent->first_child) {
        parent->first_child = child;
        return;
    }

    {
        Tree_Node *last = parent->first_child;
        while (last->next_sibling) last = last->next_sibling;
        last->next_sibling = child;
    }
}

i32 tree_child_count(const Tree_Node *node)
{
    i32 n = 0;
    Tree_Node *c = node->first_child;
    while (c) { n++; c = c->next_sibling; }
    return n;
}

static i32 tree_walk_preorder_(Tree_Node *node, i32 (*visit)(Tree_Node *, void *), void *ctx)
{
    Tree_Node *c;
    if (!visit(node, ctx)) return 0;
    for (c = node->first_child; c; c = c->next_sibling) {
        if (!tree_walk_preorder_(c, visit, ctx)) return 0;
    }
    return 1;
}

i32 tree_walk_preorder(Tree_Node *root, i32 (*visit)(Tree_Node *node, void *ctx), void *ctx)
{
    return tree_walk_preorder_(root, visit, ctx);
}

#endif /* TREE_IMPLEMENTATION_ */
#endif /* TREE_IMPLEMENTATION */
