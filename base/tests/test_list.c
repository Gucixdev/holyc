#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#define LIST_IMPLEMENTATION
#include "core/arena_alloc.h"
#include "data/list.h"

int main(void)
{
    Arena a = {0};
    List l;
    i32 v1 = 1, v2 = 2, v3 = 3;

    print_cstr("test_list\n");

    list_init(&l);
    TEST_CHECK(l.count == 0 && list_front(&l) == NULL && list_back(&l) == NULL, "an empty list has count 0 and no front/back");

    list_push_back(&a, &l, &v1);
    list_push_back(&a, &l, &v2);
    list_push_back(&a, &l, &v3);
    TEST_CHECK(l.count == 3, "push_back three times gives count 3");
    TEST_CHECK(*(i32 *)list_front(&l)->data == 1, "front is the first pushed value");
    TEST_CHECK(*(i32 *)list_back(&l)->data == 3, "back is the last pushed value");

    {
        List_Node *n = list_front(&l);
        i32 seen[3];
        i32 i = 0;
        while (n) { seen[i++] = *(i32 *)n->data; n = n->next; }
        TEST_CHECK(seen[0] == 1 && seen[1] == 2 && seen[2] == 3, "forward traversal visits values in push order");
    }
    {
        List_Node *n = list_back(&l);
        i32 seen[3];
        i32 i = 0;
        while (n) { seen[i++] = *(i32 *)n->data; n = n->prev; }
        TEST_CHECK(seen[0] == 3 && seen[1] == 2 && seen[2] == 1, "backward traversal visits values in reverse push order");
    }

    {
        List_Node *mid = list_front(&l)->next; /* the node holding v2 */
        list_remove(&l, mid);
        TEST_CHECK(l.count == 2, "removing the middle node drops count to 2");
        TEST_CHECK(list_front(&l)->next == list_back(&l), "the list is now a direct 2-element chain (1 <-> 3)");
        TEST_CHECK(list_back(&l)->prev == list_front(&l), "the back node's prev correctly skips the removed middle node");
    }

    {
        List_Node *front = list_front(&l);
        list_remove(&l, front);
        TEST_CHECK(l.count == 1 && list_front(&l) == list_back(&l), "removing the (new) front leaves a single-node list");
    }

    {
        List_Node *only = list_front(&l);
        list_remove(&l, only);
        TEST_CHECK(l.count == 0 && list_front(&l) == NULL && list_back(&l) == NULL, "removing the last node empties the list cleanly");
    }

    {
        List l2;
        list_init(&l2);
        list_push_front(&a, &l2, &v1);
        list_push_front(&a, &l2, &v2);
        list_push_front(&a, &l2, &v3);
        TEST_CHECK(*(i32 *)list_front(&l2)->data == 3, "push_front three times: the last pushed value is at the front");
        TEST_CHECK(*(i32 *)list_back(&l2)->data == 1, "push_front three times: the first pushed value ends up at the back");
    }

    arena_free(&a);
    print_flush();
    return test_failures_ != 0;
}
