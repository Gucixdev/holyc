#ifndef HOLYC_NET_H_
#define HOLYC_NET_H_

/* Network utility plumbing: IPv4 address string <-> bytes, and host/
 * network byte-order swaps - what packet.h/arp.h/dhcp.h/ping.h (raw
 * packet builders, this same directory) need to embed/read an address
 * or a multi-byte header field, distinct from socket.h's job (real
 * TCP connect/send/recv, hostname resolution via getaddrinfo - it
 * never needs to hand-parse an IPv4 string itself) and from http.h's
 * (the HTTP protocol built on top of socket.h). Byte swaps assume a
 * little-endian host - true for every machine this Linux-only project
 * targets (same assumption arena_alloc.h/hardscan.h already make
 * about this being x86_64 Linux, not hedged for a big-endian target
 * that will never run this code). */

#include "base.h"

i32 net_ipv4_parse(const char *s, u8 out[4]);    /* "a.b.c.d" -> 4 bytes; returns 0 on malformed input, out untouched */
void net_ipv4_format(const u8 ip[4], char out[16]); /* 4 bytes -> "a.b.c.d\0", out must hold at least 16 bytes */

u16 net_hton16(u16 host_value);
u16 net_ntoh16(u16 net_value);
u32 net_hton32(u32 host_value);
u32 net_ntoh32(u32 net_value);

#endif /* HOLYC_NET_H_ */

#ifdef NET_IMPLEMENTATION
#ifndef NET_IMPLEMENTATION_
#define NET_IMPLEMENTATION_

static i32 net_parse_octet_(const char *s, i32 *pos, u8 *out)
{
    i32 n = 0;
    i32 digits = 0;

    while (s[*pos] >= '0' && s[*pos] <= '9') {
        n = n * 10 + (s[*pos] - '0');
        (*pos)++;
        digits++;
        if (digits > 3 || n > 255) return 0;
    }
    if (digits == 0) return 0;

    *out = (u8)n;
    return 1;
}

i32 net_ipv4_parse(const char *s, u8 out[4])
{
    i32 pos = 0;
    i32 i;
    u8 octets[4];

    for (i = 0; i < 4; i++) {
        if (!net_parse_octet_(s, &pos, &octets[i])) return 0;
        if (i < 3) {
            if (s[pos] != '.') return 0;
            pos++;
        }
    }
    if (s[pos] != '\0') return 0; /* trailing garbage after the 4th octet */

    out[0] = octets[0]; out[1] = octets[1]; out[2] = octets[2]; out[3] = octets[3];
    return 1;
}

static i32 net_format_octet_(u8 v, char *out)
{
    char tmp[3];
    i32 n = 0;
    i32 i;

    if (v == 0) {
        out[0] = '0';
        return 1;
    }
    while (v > 0) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    for (i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    return n;
}

void net_ipv4_format(const u8 ip[4], char out[16])
{
    i32 pos = 0;
    i32 i;

    for (i = 0; i < 4; i++) {
        pos += net_format_octet_(ip[i], out + pos);
        if (i < 3) out[pos++] = '.';
    }
    out[pos] = '\0';
}

u16 net_hton16(u16 v) { return (u16)((v << 8) | (v >> 8)); }
u16 net_ntoh16(u16 v) { return net_hton16(v); }

u32 net_hton32(u32 v)
{
    return ((v & 0x000000FFu) << 24)
         | ((v & 0x0000FF00u) << 8)
         | ((v & 0x00FF0000u) >> 8)
         | ((v & 0xFF000000u) >> 24);
}
u32 net_ntoh32(u32 v) { return net_hton32(v); }

#endif /* NET_IMPLEMENTATION_ */
#endif /* NET_IMPLEMENTATION */
