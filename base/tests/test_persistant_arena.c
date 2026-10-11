#include "test_common.h"
#define PERSISTANT_ARENA_IMPLEMENTATION
#include "alloc/persistant_arena.h"
#include <string.h>
#include <unistd.h>

#define TEST_PA_PATH "/tmp/holyc_test_persistant_arena.bin"

int main(void)
{
    Persistant_Arena pa;
    char *s1, *s2;
    size_t off1, off2;

    print_cstr("test_persistant_arena\n");
    unlink(TEST_PA_PATH);

    /* Session 1: create, allocate, close - the real "process restart"
     * this whole file exists to check survives is simulated by a real
     * close()+reopen() of the same real file, not just re-reading the
     * same still-mapped memory. */
    TEST_CHECK(persistant_arena_open(&pa, TEST_PA_PATH, 4096), "open (create) succeeds");

    s1 = (char *)persistant_arena_alloc(&pa, 32);
    strcpy(s1, "hello persistent world");
    off1 = persistant_arena_offset(&pa, s1);

    s2 = (char *)persistant_arena_alloc(&pa, 32);
    strcpy(s2, "second allocation");
    off2 = persistant_arena_offset(&pa, s2);

    TEST_CHECK(pa.header->count == 64, "count reflects both real allocations before closing");
    persistant_arena_close(&pa);

    /* Session 2: reopen - real bookkeeping AND real content must both
     * have survived the close, not just the file's own raw bytes. */
    TEST_CHECK(persistant_arena_open(&pa, TEST_PA_PATH, 4096), "reopen succeeds");
    TEST_CHECK(pa.header->count == 64, "count persisted across close/reopen");
    TEST_CHECK(pa.header->capacity == 4096, "capacity persisted across close/reopen");
    TEST_CHECK(strcmp((char *)persistant_arena_at(&pa, off1), "hello persistent world") == 0,
               "first string's real content persisted");
    TEST_CHECK(strcmp((char *)persistant_arena_at(&pa, off2), "second allocation") == 0,
               "second string's real content persisted");

    {
        char *s3 = (char *)persistant_arena_alloc(&pa, 16);
        strcpy(s3, "third");
        TEST_CHECK(pa.header->count == 80, "a new allocation after reopen continues from the persisted count");
    }

    TEST_CHECK(persistant_arena_alloc(&pa, 100000) == NULL,
               "over-capacity alloc returns NULL rather than silently growing (would invalidate live pointers)");

    persistant_arena_close(&pa);
    unlink(TEST_PA_PATH);

    print_flush();
    return test_failures_ != 0;
}
