#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#include "alloc/arena_alloc.h"
#define STREAM_IMPLEMENTATION
#include "util/stream.h"

int main(void)
{
    Arena a = {0};
    Stream st;

    print_cstr("test_stream\n");

    stream_init(&st, 4); /* deliberately tiny chunks to force multi-chunk behavior */
    TEST_CHECK(stream_is_empty(&st), "a freshly initialized stream is empty");

    stream_write(&a, &st, "hello world", 11); /* spans at least 3 chunks of size 4 */
    TEST_CHECK(!stream_is_empty(&st), "a stream with written data is not empty");

    {
        char out[20];
        size_t got = stream_read(&st, out, 11);
        TEST_CHECK(got == 11, "read across multiple chunk boundaries returns all written bytes");
        TEST_CHECK(out[0]=='h'&&out[4]=='o'&&out[5]==' '&&out[6]=='w'&&out[10]=='d',
                   "bytes read back in the exact order they were written, seamless across chunks");
    }
    TEST_CHECK(stream_is_empty(&st), "the stream is empty again once everything written has been read");

    {
        char out[4];
        size_t got = stream_read(&st, out, 4);
        TEST_CHECK(got == 0, "reading an empty stream returns 0, does not underrun");
    }

    stream_write(&a, &st, "more", 4);
    stream_write(&a, &st, "data", 4);
    {
        char out[8];
        size_t got = stream_read(&st, out, 8);
        TEST_CHECK(got == 8 && out[0]=='m' && out[7]=='a', "writing again after full drain still works (append at the existing tail)");
    }

    print_flush();
    return test_failures_ != 0;
}
