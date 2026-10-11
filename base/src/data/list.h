#ifndef HOLYC_LIST_H_
#define HOLYC_LIST_H_

/* Generic intrusive doubly-linked list, arena-allocated nodes - the
 * complement to array.h's contiguous growable array, not a competing
 * implementation of the same job: array.h is the right pick when you
 * need random-access/cache-friendly iteration and don't often insert/
 * remove from the middle; list.h is the right pick when you need O(1)
 * insertion/removal at an arbitrary known position (a node pointer you
 * already hold) and don't need random access at all. void* payload
 * (not a generic/templated element type) matches this project's own
 * established style for payload-agnostic containers (see gc.c's own
 * void*-payload object header). */

#include "alloc/arena_alloc.h"

typedef struct List_Node {
    struct List_Node *prev, *next;
    void *data;
} List_Node;

typedef struct {
    List_Node *head, *tail;
    size_t count;
} List;

void list_init(List *l);

/* both O(1) - the whole reason this exists instead of array.h's own
 * shift-everything-over insert/remove. */
List_Node *list_push_back(Arena *a, List *l, void *data);
List_Node *list_push_front(Arena *a, List *l, void *data);
void list_remove(List *l, List_Node *node);

/* NULL if the list is empty - does not remove the node (the arena owns
 * it regardless; this just detaches it from the list, same as
 * list_remove). Combine with list_remove(l, list_front(l)/list_back(l))
 * for a real pop. */
List_Node *list_front(const List *l);
List_Node *list_back(const List *l);

#endif /* HOLYC_LIST_H_ */

#ifdef LIST_IMPLEMENTATION
#ifndef LIST_IMPLEMENTATION_
#define LIST_IMPLEMENTATION_

void list_init(List *l)
{
    l->head = NULL;
    l->tail = NULL;
    l->count = 0;
}

List_Node *list_push_back(Arena *a, List *l, void *data)
{
    List_Node *n = (List_Node *)arena_alloc(a, sizeof(List_Node));
    n->data = data;
    n->next = NULL;
    n->prev = l->tail;
    if (l->tail) l->tail->next = n; else l->head = n;
    l->tail = n;
    l->count++;
    return n;
}

List_Node *list_push_front(Arena *a, List *l, void *data)
{
    List_Node *n = (List_Node *)arena_alloc(a, sizeof(List_Node));
    n->data = data;
    n->prev = NULL;
    n->next = l->head;
    if (l->head) l->head->prev = n; else l->tail = n;
    l->head = n;
    l->count++;
    return n;
}

void list_remove(List *l, List_Node *node)
{
    if (node->prev) node->prev->next = node->next; else l->head = node->next;
    if (node->next) node->next->prev = node->prev; else l->tail = node->prev;
    l->count--;
}

List_Node *list_front(const List *l) { return l->head; }
List_Node *list_back(const List *l) { return l->tail; }

#endif /* LIST_IMPLEMENTATION_ */
#endif /* LIST_IMPLEMENTATION */
