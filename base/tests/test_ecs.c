#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#define ECS_IMPLEMENTATION
#include "arena_alloc.h"
#include "ecs.h"

typedef struct { f32 x, y; } Position;
ECS_COMPONENT_DEFINE(Position, position)

int main(void)
{
    Arena a = {0};
    Ecs_World w;
    Ecs_Entity e0, e1, e2;
    position_Storage store;
    Position *p;

    print_cstr("test_ecs\n");

    ecs_world_init(&w);
    position_storage_init(&a, &store);

    e0 = ecs_create(&a, &w);
    e1 = ecs_create(&a, &w);
    e2 = ecs_create(&a, &w);
    TEST_CHECK(e0 != e1 && e1 != e2 && e0 != e2, "3 created entities have distinct ids");

    {
        Position pos0, pos1;
        pos0.x = 1.0f; pos0.y = 2.0f;
        pos1.x = 3.0f; pos1.y = 4.0f;
        position_storage_add(&a, &store, e0, pos0);
        position_storage_add(&a, &store, e1, pos1);
    }

    TEST_CHECK(position_storage_has(&store, e0), "e0 has Position after add");
    TEST_CHECK(position_storage_has(&store, e1), "e1 has Position after add");
    TEST_CHECK(!position_storage_has(&store, e2), "e2 does NOT have Position (never added)");

    p = position_storage_get(&store, e0);
    TEST_CHECK(p != 0 && p->x == 1.0f && p->y == 2.0f, "position_storage_get(e0) returns the correct data");

    TEST_CHECK(store.count == 2, "storage dense count is 2 after 2 adds");

    /* remove e0 (swap-with-last: e1's data should move into e0's old dense slot) */
    position_storage_remove(&store, e0);
    TEST_CHECK(!position_storage_has(&store, e0), "e0 no longer has Position after remove");
    TEST_CHECK(position_storage_has(&store, e1), "e1 still has Position after removing e0 (swap-with-last correctness)");
    TEST_CHECK(store.count == 1, "storage dense count is 1 after remove");
    {
        Position *p1 = position_storage_get(&store, e1);
        TEST_CHECK(p1 != 0 && p1->x == 3.0f && p1->y == 4.0f, "e1's data intact after e0's removal (swap-with-last didn't corrupt it)");
    }

    /* dense array iteration */
    {
        size_t i;
        i32 saw_e1 = 0;
        for (i = 0; i < store.count; i++) {
            if (store.dense_entities[i] == e1) saw_e1 = 1;
        }
        TEST_CHECK(saw_e1, "dense array iteration finds e1");
    }

    /* destroy + id recycling */
    ecs_destroy(&a, &w, e2);
    {
        Ecs_Entity e3 = ecs_create(&a, &w);
        TEST_CHECK(e3 == e2, "destroying an entity and creating a new one recycles the freed id");
    }

    /* a fresh entity with a high id grows the sparse array correctly */
    {
        Ecs_Entity big = 0;
        size_t i;
        for (i = 0; i < 40; i++) big = ecs_create(&a, &w);
        TEST_CHECK(!position_storage_has(&store, big), "a never-added high-id entity correctly reports no component (sparse growth didn't leave garbage)");
        {
            Position pb;
            pb.x = 9.0f; pb.y = 9.0f;
            position_storage_add(&a, &store, big, pb);
        }
        TEST_CHECK(position_storage_has(&store, big), "component add works correctly after sparse array growth past 16 entries");
    }

    print_flush();
    return test_failures_ != 0;
}
