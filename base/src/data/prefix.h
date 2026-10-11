#ifndef HOLYC_PREFIX_H_
#define HOLYC_PREFIX_H_

/* Prefix tree (trie) over byte strings. Arena-allocated nodes, 256-way
 * branching (one child slot per byte value - simple and fast, at the
 * cost of 256*sizeof(ptr) per node; fine for a modest key count, a
 * sparse/compressed trie would trade that for a much bigger real gain
 * only past a few thousand keys). */

#include "core/base.h"
#include "core/arena_alloc.h"

typedef struct Prefix_Node_ {
    struct Prefix_Node_ *children[256];
    i32 is_end;  /* 1 if a key ends exactly here */
    void *value; /* caller-owned, valid only when is_end */
} Prefix_Node;

typedef struct {
    Prefix_Node *root;
    Arena *arena;
    size_t count; /* number of complete keys inserted */
} Prefix_Tree;

void prefix_init(Prefix_Tree *t, Arena *arena);

/* inserts key -> value. Overwrites value if key already present
 * (count unchanged in that case). */
void prefix_insert(Prefix_Tree *t, const char *key, size_t key_len, void *value);

/* NULL if key isn't present (as a complete key - a key that's only a
 * prefix of something else doesn't count, see prefix_has_prefix). */
void *prefix_get(const Prefix_Tree *t, const char *key, size_t key_len);

/* 1 if any inserted key starts with this prefix (including the prefix
 * itself, if it was also inserted as a complete key). */
i32 prefix_has_prefix(const Prefix_Tree *t, const char *prefix, size_t prefix_len);

#endif /* HOLYC_PREFIX_H_ */

#ifdef PREFIX_IMPLEMENTATION
#ifndef PREFIX_IMPLEMENTATION_
#define PREFIX_IMPLEMENTATION_

void prefix_init(Prefix_Tree *t, Arena *arena)
{
    t->arena = arena;
    t->count = 0;
    t->root = (Prefix_Node *)arena_alloc(arena, sizeof(Prefix_Node));
    {
        i32 i;
        for (i = 0; i < 256; i++) t->root->children[i] = NULL;
    }
    t->root->is_end = 0;
    t->root->value = NULL;
}

static Prefix_Node *prefix_node_new_(Arena *a)
{
    Prefix_Node *n = (Prefix_Node *)arena_alloc(a, sizeof(Prefix_Node));
    i32 i;
    for (i = 0; i < 256; i++) n->children[i] = NULL;
    n->is_end = 0;
    n->value = NULL;
    return n;
}

void prefix_insert(Prefix_Tree *t, const char *key, size_t key_len, void *value)
{
    Prefix_Node *node = t->root;
    size_t i;
    for (i = 0; i < key_len; i++) {
        u8 b = (u8)key[i];
        if (!node->children[b]) node->children[b] = prefix_node_new_(t->arena);
        node = node->children[b];
    }
    if (!node->is_end) t->count++;
    node->is_end = 1;
    node->value = value;
}

static const Prefix_Node *prefix_walk_(const Prefix_Tree *t, const char *key, size_t key_len)
{
    const Prefix_Node *node = t->root;
    size_t i;
    for (i = 0; i < key_len; i++) {
        u8 b = (u8)key[i];
        if (!node->children[b]) return NULL;
        node = node->children[b];
    }
    return node;
}

void *prefix_get(const Prefix_Tree *t, const char *key, size_t key_len)
{
    const Prefix_Node *node = prefix_walk_(t, key, key_len);
    return (node && node->is_end) ? node->value : NULL;
}

i32 prefix_has_prefix(const Prefix_Tree *t, const char *prefix, size_t prefix_len)
{
    return prefix_walk_(t, prefix, prefix_len) != NULL;
}

#endif /* PREFIX_IMPLEMENTATION_ */
#endif /* PREFIX_IMPLEMENTATION */
