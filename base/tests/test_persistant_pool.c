#include "test_common.h"
#define PERSISTANT_POOL_IMPLEMENTATION
#include "alloc/persistant_pool.h"
#include <string.h>
#include <unistd.h>

/* Real two-PROCESS design, not two calls in one process: mmap() gives
 * no base-address guarantee, and a single process reopening the same
 * fd can coincidentally get the SAME address back, which would hide a
 * real "stored an absolute pointer instead of an offset" bug (see
 * persistant_pool.h's own top comment on exactly this). A genuine
 * self-re-exec (not fork - a fork's child still starts from a COW copy
 * of the parent's own address space/layout, a real, if smaller,
 * confound) via execl gives a truly fresh process image, forcing an
 * independent mapping decision - this is what actually exercises the
 * property this design is supposed to guarantee, not merely compiles
 * against it. */

#define TEST_PP_PATH "/tmp/holyc_test_persistant_pool.bin"

static void write_phase(void)
{
    Persistant_Pool pp;
    void *a, *b, *c;

    unlink(TEST_PP_PATH);
    if (!persistant_pool_open(&pp, TEST_PP_PATH, 32, 4)) {
        print_cstr("  FAIL: write phase open (create) failed\n");
        return;
    }
    a = persistant_pool_alloc(&pp);
    b = persistant_pool_alloc(&pp);
    c = persistant_pool_alloc(&pp);
    strcpy((char *)a, "chunk A content");
    strcpy((char *)b, "chunk B content");
    strcpy((char *)c, "chunk C content");
    /* Free the MIDDLE chunk (B) specifically - the read phase must see
     * exactly one chunk free (B's real slot), reconstructed via a real
     * offset read back from a genuinely different mapping. */
    persistant_pool_dealloc(&pp, b);
    persistant_pool_close(&pp);
}

static int read_phase(void)
{
    Persistant_Pool pp;
    void *reused, *d, *exhausted;
    char *a_content, *c_content;

    print_cstr("test_persistant_pool\n");

    TEST_CHECK(persistant_pool_open(&pp, TEST_PP_PATH, 32, 4), "reopen (from a fresh process image) succeeds");
    TEST_CHECK(pp.header->free_head == 2, "free_head (chunk B, 1-based index 2) persisted correctly");

    a_content = (char *)pp.chunks;
    c_content = (char *)pp.chunks + 2 * (size_t)pp.header->chunk_size;
    TEST_CHECK(strcmp(a_content, "chunk A content") == 0, "chunk A's real content persisted");
    TEST_CHECK(strcmp(c_content, "chunk C content") == 0, "chunk C's real content persisted");

    /* Checked by IDENTITY (which real chunk slot came back), not by
     * content: dealloc legitimately overwrites a freed chunk's first
     * bytes with the free-list link itself (same "no hidden cost,
     * caller writes what it needs" tradeoff pool_alloc.c's own
     * pool_alloc documents) - B's old string is gone the moment it was
     * freed, in the write phase already, before this process even
     * started. If the free list had stored a raw pointer instead of a
     * real offset, this would dereference an address meaningless in
     * THIS mapping - real garbage or a crash, not just wrong content. */
    reused = persistant_pool_alloc(&pp);
    TEST_CHECK(reused == (void *)((char *)pp.chunks + 1 * (size_t)pp.header->chunk_size),
               "reused chunk is B's real slot, by real offset identity, across a fresh mapping");

    d = persistant_pool_alloc(&pp);
    TEST_CHECK(d != NULL, "chunk D (never touched by the write phase) is still available");

    exhausted = persistant_pool_alloc(&pp);
    TEST_CHECK(exhausted == NULL, "the pool is genuinely exhausted now (4 chunks, all allocated)");

    persistant_pool_close(&pp);
    unlink(TEST_PP_PATH);

    print_flush();
    return test_failures_ != 0;
}

int main(int argc, char *argv[])
{
    if (argc >= 2 && strcmp(argv[1], "__read_phase") == 0) {
        return read_phase();
    }

    write_phase();
    execl(argv[0], argv[0], "__read_phase", (char *)NULL);
    /* execl only returns on real failure - a genuine test-harness
     * problem (this binary got moved/deleted mid-run), not something
     * TEST_CHECK can report from a process that failed to become the
     * one that would call it. */
    print_cstr("  FAIL: execl(self, __read_phase) failed - can't complete the two-process test\n");
    return 1;
}
