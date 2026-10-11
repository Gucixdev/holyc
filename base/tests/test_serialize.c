#include "test_common.h"
#define SERIALIZE_IMPLEMENTATION
#include "codec/serialize.h"

int main(void)
{
    u8 buf[64];
    Serialize_Writer w;
    Serialize_Reader r;

    print_cstr("test_serialize\n");

    serialize_writer_init(&w, buf, sizeof(buf));
    TEST_CHECK(serialize_write_u8(&w, 0x42), "write_u8 succeeds with room in the buffer");
    TEST_CHECK(serialize_write_u16(&w, 0xBEEF), "write_u16 succeeds");
    TEST_CHECK(serialize_write_u32(&w, 0xDEADBEEFu), "write_u32 succeeds");
    TEST_CHECK(serialize_write_u64(&w, 0x1122334455667788UL), "write_u64 succeeds");
    TEST_CHECK(serialize_write_f32(&w, 3.5f), "write_f32 succeeds");
    TEST_CHECK(serialize_write_f64(&w, 2.25), "write_f64 succeeds");
    TEST_CHECK(serialize_write_bytes(&w, "hi", 2), "write_bytes succeeds");
    TEST_CHECK(w.len == 1+2+4+8+4+8+2, "writer's len advances by exactly the total bytes written");

    serialize_reader_init(&r, buf, w.len);
    {
        u8 v8; u16 v16; u32 v32; u64 v64; f32 vf32; f64 vf64; char vb[2];

        TEST_CHECK(serialize_read_u8(&r, &v8) && v8 == 0x42, "read_u8 round-trips exactly what was written");
        TEST_CHECK(serialize_read_u16(&r, &v16) && v16 == 0xBEEF, "read_u16 round-trips exactly what was written");
        TEST_CHECK(serialize_read_u32(&r, &v32) && v32 == 0xDEADBEEFu, "read_u32 round-trips exactly what was written");
        TEST_CHECK(serialize_read_u64(&r, &v64) && v64 == 0x1122334455667788UL, "read_u64 round-trips exactly what was written");
        TEST_CHECK(serialize_read_f32(&r, &vf32) && vf32 == 3.5f, "read_f32 round-trips exactly what was written");
        TEST_CHECK(serialize_read_f64(&r, &vf64) && vf64 == 2.25, "read_f64 round-trips exactly what was written");
        TEST_CHECK(serialize_read_bytes(&r, vb, 2) && vb[0]=='h' && vb[1]=='i', "read_bytes round-trips a raw byte run");
    }

    {
        Serialize_Writer tiny;
        u8 tiny_buf[1];
        serialize_writer_init(&tiny, tiny_buf, 1);
        TEST_CHECK(!serialize_write_u32(&tiny, 1), "a write that would overrun the buffer fails cleanly");
        TEST_CHECK(tiny.len == 0, "a failed write does not partially advance len");
    }

    {
        Serialize_Reader empty;
        u32 out;
        serialize_reader_init(&empty, buf, 0);
        TEST_CHECK(!serialize_read_u32(&empty, &out), "reading past the end of the buffer fails cleanly");
    }

    print_flush();
    return test_failures_ != 0;
}
