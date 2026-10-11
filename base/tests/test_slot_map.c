#include "test_common.h"
#define SLOT_MAP_IMPLEMENTATION
#include "data/slot_map.h"

int main(void)
{
    i32 items[8];
    u8 occupied[8];
    Slot_Map b;

    print_cstr("test_bucket\n");

    slot_map_init(&b, items, occupied, 8, sizeof(i32));
    TEST_CHECK(b.count == 0, "starts empty");

    {
        i32 v = 100;
        size_t idx;
        TEST_CHECK(slot_map_insert(&b, &v, &idx) == 1, "insert into an empty bucket succeeds");
        TEST_CHECK(idx == 0, "first insert lands in slot 0");
        TEST_CHECK(*(i32 *)slot_map_get(&b, 0) == 100, "the value is really there");
    }

    {
        i32 v = 200;
        size_t idx;
        slot_map_insert(&b, &v, &idx);
        TEST_CHECK(idx == 1, "second insert lands in slot 1, not reusing slot 0");
    }

    /* STABLE indices: removing an earlier slot must not move a later one */
    {
        i32 v = 300;
        size_t idx2;
        slot_map_insert(&b, &v, &idx2); /* slot 2, value 300 */

        TEST_CHECK(slot_map_remove(&b, 0) == 1, "removing slot 0 succeeds");
        TEST_CHECK(slot_map_get(&b, 0) == NULL, "slot 0 reads back NULL now - it's empty");
        TEST_CHECK(*(i32 *)slot_map_get(&b, 1) == 200, "slot 1's value is untouched by removing slot 0");
        TEST_CHECK(*(i32 *)slot_map_get(&b, 2) == 300, "slot 2's value is untouched too - no swap-with-last happened");
        TEST_CHECK(idx2 == 2, "slot 2 really was index 2");
    }

    TEST_CHECK(b.count == 2, "count reflects 2 occupied slots (1 and 2) after removing slot 0");

    /* the freed slot 0 gets reused on the next insert */
    {
        i32 v = 400;
        size_t idx;
        slot_map_insert(&b, &v, &idx);
        TEST_CHECK(idx == 0, "a later insert reuses the freed slot 0 rather than skipping past it");
        TEST_CHECK(*(i32 *)slot_map_get(&b, 0) == 400, "the reused slot holds the new value");
    }

    /* double-remove is a safe no-op, not a crash or negative count */
    TEST_CHECK(slot_map_remove(&b, 0) == 1, "removing an occupied slot succeeds");
    TEST_CHECK(slot_map_remove(&b, 0) == 0, "removing the same slot again reports failure, doesn't double-decrement count");

    /* out of range is safe */
    TEST_CHECK(slot_map_get(&b, 999) == NULL, "out-of-range get returns NULL, not garbage/crash");
    TEST_CHECK(slot_map_remove(&b, 999) == 0, "out-of-range remove is a safe no-op");
    TEST_CHECK(slot_map_is_occupied(&b, 999) == 0, "out-of-range is_occupied reports false, not garbage");

    /* fill to capacity, then overflow */
    {
        Slot_Map full;
        i32 fitems[4];
        u8 focc[4];
        i32 i, v;
        size_t idx;
        i32 inserted = 0;

        slot_map_init(&full, fitems, focc, 4, sizeof(i32));
        for (i = 0; i < 4; i++) {
            v = i;
            if (slot_map_insert(&full, &v, &idx)) inserted++;
        }
        TEST_CHECK(inserted == 4, "fills exactly to capacity");
        v = 99;
        TEST_CHECK(slot_map_insert(&full, &v, &idx) == 0, "inserting past capacity fails cleanly, doesn't overflow the buffer");
        TEST_CHECK(full.count == 4, "count stays at capacity after a failed overflow insert");
    }

    /* clear */
    slot_map_clear(&b);
    TEST_CHECK(b.count == 0, "clear resets count to 0");
    TEST_CHECK(slot_map_get(&b, 1) == NULL, "clear really frees every slot, not just the count field");

    print_flush();
    return test_failures_ != 0;
}
