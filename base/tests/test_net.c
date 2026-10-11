#include "test_common.h"
#define NET_IMPLEMENTATION
#include "net.h"

int main(void)
{
    print_cstr("test_net\n");

    {
        u8 ip[4];
        TEST_CHECK(net_ipv4_parse("192.168.1.1", ip) == 1, "parse succeeds on a well-formed IPv4 string");
        TEST_CHECK(ip[0]==192 && ip[1]==168 && ip[2]==1 && ip[3]==1, "parsed octets are correct and in order");
    }
    {
        u8 ip[4];
        TEST_CHECK(net_ipv4_parse("0.0.0.0", ip) == 1 && ip[0]==0 && ip[3]==0, "parse handles all-zero address");
        TEST_CHECK(net_ipv4_parse("255.255.255.255", ip) == 1 && ip[0]==255, "parse handles the max octet value");
    }
    {
        u8 ip[4];
        TEST_CHECK(net_ipv4_parse("256.1.1.1", ip) == 0, "parse rejects an octet over 255");
        TEST_CHECK(net_ipv4_parse("1.2.3", ip) == 0, "parse rejects too few octets");
        TEST_CHECK(net_ipv4_parse("1.2.3.4.5", ip) == 0, "parse rejects trailing garbage after 4 octets");
        TEST_CHECK(net_ipv4_parse("1.2.3.a", ip) == 0, "parse rejects a non-digit octet");
    }
    {
        char out[16];
        u8 ip[4];
        ip[0]=10; ip[1]=0; ip[2]=0; ip[3]=255;
        net_ipv4_format(ip, out);
        TEST_CHECK(out[0]=='1'&&out[1]=='0'&&out[2]=='.'&&out[3]=='0'&&out[4]=='.', "format produces the expected leading digits and dots");
        {
            u8 back[4];
            TEST_CHECK(net_ipv4_parse(out, back) == 1 && back[0]==10 && back[3]==255, "format then parse round-trips exactly");
        }
    }

    TEST_CHECK(net_hton16(0x1234) == 0x3412, "hton16 swaps the byte order");
    TEST_CHECK(net_ntoh16(net_hton16(0xBEEF)) == 0xBEEF, "hton16 then ntoh16 round-trips (both are the same swap)");
    TEST_CHECK(net_hton32(0x12345678u) == 0x78563412u, "hton32 swaps all 4 bytes into reverse order");
    TEST_CHECK(net_ntoh32(net_hton32(0xDEADBEEFu)) == 0xDEADBEEFu, "hton32 then ntoh32 round-trips");

    print_flush();
    return test_failures_ != 0;
}
