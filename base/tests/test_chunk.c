#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#include "core/arena_alloc.h"
#define CHUNK_IMPLEMENTATION
#include "core/chunk.h"

int main(void)
{
    Arena a = {0};
    Chunk c;

    print_cstr("test_chunk\n");

    c = chunk_make(&a, 8);
    TEST_CHECK(c.cap == 8 && c.len == 0 && c.pos == 0, "chunk_make: fresh chunk starts empty with the requested capacity");
    TEST_CHECK(chunk_remaining_write(&c) == 8, "chunk_remaining_write: full capacity available on a fresh chunk");
    TEST_CHECK(chunk_remaining_read(&c) == 0, "chunk_remaining_read: nothing to read on a fresh chunk");
    TEST_CHECK(!chunk_full(&c), "chunk_full: a fresh chunk is not full");
    TEST_CHECK(chunk_exhausted(&c), "chunk_exhausted: a fresh chunk (nothing written) is trivially exhausted");

    {
        size_t written = chunk_write(&c, "hello", 5);
        TEST_CHECK(written == 5, "chunk_write: writes all 5 bytes when there's room");
        TEST_CHECK(c.len == 5, "chunk_write: len advances by the amount actually written");
        TEST_CHECK(chunk_remaining_write(&c) == 3, "chunk_remaining_write: shrinks by what was written");
        TEST_CHECK(!chunk_exhausted(&c), "chunk_exhausted: false once there's unread data");
    }

    {
        size_t written = chunk_write(&c, "WORLD!!", 7);
        TEST_CHECK(written == 3, "chunk_write: a write past capacity is truncated to the remaining room, not rejected outright");
        TEST_CHECK(c.len == 8, "chunk_write: len caps out exactly at capacity");
        TEST_CHECK(chunk_full(&c), "chunk_full: true once len reaches cap");
        TEST_CHECK(chunk_write(&c, "x", 1) == 0, "chunk_write: a full chunk accepts 0 more bytes");
    }

    {
        char out[4];
        size_t read1, read2, read3;

        read1 = chunk_read(&c, out, 4);
        TEST_CHECK(read1 == 4, "chunk_read: reads the requested amount when available");
        TEST_CHECK(out[0]=='h'&&out[1]=='e'&&out[2]=='l'&&out[3]=='l', "chunk_read: bytes come back in write order, from the start");
        TEST_CHECK(c.pos == 4, "chunk_read: pos advances by the amount actually read");

        read2 = chunk_read(&c, out, 4);
        TEST_CHECK(read2 == 4, "chunk_read: second read picks up right where the first left off");
        TEST_CHECK(out[0]=='o'&&out[1]=='W'&&out[2]=='O'&&out[3]=='R', "chunk_read: continues across the boundary between the two earlier writes");
        TEST_CHECK(chunk_exhausted(&c), "chunk_exhausted: true once pos catches up to len");

        read3 = chunk_read(&c, out, 4);
        TEST_CHECK(read3 == 0, "chunk_read: an exhausted chunk yields 0 bytes, does not underrun");
    }

    chunk_reset(&c);
    TEST_CHECK(c.len == 0 && c.pos == 0, "chunk_reset: len and pos both return to 0");
    TEST_CHECK(c.cap == 8, "chunk_reset: capacity (and the backing buffer) is untouched, ready to reuse");
    TEST_CHECK(chunk_write(&c, "reuse!!", 7) == 7, "chunk_reset: the same chunk accepts fresh writes afterward");

    print_flush();
    return test_failures_ != 0;
}
