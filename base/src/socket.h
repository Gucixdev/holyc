#ifndef HOLYC_SOCKET_H_
#define HOLYC_SOCKET_H_

/* Thin, real wrapper over POSIX TCP sockets - not a portability layer
 * (this project is Linux-only, same stance print.h/arena_alloc.h/
 * fgjob.h already take on write(2)/mmap/pthreads), just enough to stop
 * every future networked module from re-deriving the same partial-
 * read/write loop and sockaddr boilerplate by hand.
 *
 * TCP only - no UDP. System headers (sys/socket.h, netinet/in.h,
 * arpa/inet.h, unistd.h, netdb.h) live directly here, this file's own
 * sole real consumer, matching this project's "no shared platform
 * wrapper" convention (see src/core/uuid4.h / src/utils/exe_path.h for
 * the same pattern elsewhere).
 *
 * Every send/recv here loops until the requested byte count is fully
 * handled or a real error/EOF occurs - a raw single send()/recv() call
 * is not guaranteed to move every byte in one call (TCP is a byte
 * stream, not a message protocol), and treating a partial transfer as
 * "done" is a genuine, common bug class this wrapper exists to close
 * off at the one place instead of at every call site. */

#include "base.h"
#include "contracts.h"

/* own local memset_ - own copy per this project's standing "no cross-
 * IMPLEMENTATION-guard dependency" rule, avoiding a <string.h>
 * dependency for one small byte loop (zeroing a sockaddr_in). */
static void *socket_memset_(void *dst, i32 val, size_t n)
{
    u8 *d = (u8 *)dst;
    size_t i;
    for (i = 0; i < n; i++) d[i] = (u8)val;
    return dst;
}

typedef enum {
    SOCKET_ESUCCESS    =  0,
    SOCKET_ERESOLVE    = -1, /* host lookup (getaddrinfo) failed */
    SOCKET_ECREATE     = -2, /* socket() itself failed */
    SOCKET_ECONNECT    = -3, /* connect() failed against every resolved address */
    SOCKET_EBIND       = -4, /* bind() failed */
    SOCKET_ELISTEN     = -5, /* listen() failed */
    SOCKET_EACCEPT     = -6, /* accept() failed */
    SOCKET_EIO         = -7, /* send()/recv() failed mid-transfer */
    SOCKET_ECLOSED     = -8  /* peer closed the connection (recv() returned 0 before out_len bytes) */
} Socket_Error;

/* client: resolves host (IPv4 or IPv6, whichever the resolver returns
 * first) and connects. Returns the connected fd (>= 0) on success, or a
 * negative Socket_Error on failure. */
i32 socket_connect(const char *host, u16 port);

/* server: creates, binds (0.0.0.0), and listens on port. Returns the
 * listening fd (>= 0) on success, or a negative Socket_Error. */
i32 socket_listen(u16 port, i32 backlog);

/* blocks until a client connects; returns the accepted fd (>= 0), or a
 * negative Socket_Error. */
i32 socket_accept(i32 listen_fd);

/* sends exactly len bytes, looping over partial writes. Returns
 * SOCKET_ESUCCESS once all len bytes are sent, or SOCKET_EIO on a real
 * error partway through. */
i32 socket_send(i32 fd, const void *buf, size_t len);

/* receives exactly len bytes into buf, looping over partial reads.
 * Returns SOCKET_ESUCCESS once all len bytes are read, SOCKET_ECLOSED
 * if the peer closed before len bytes arrived, or SOCKET_EIO on a real
 * error. */
i32 socket_recv(i32 fd, void *buf, size_t len);

/* receives UP TO len bytes (a single recv() call, not a full-len loop -
 * for callers that want to read "whatever's available right now", e.g.
 * an HTTP response of unknown length). Returns the number of bytes
 * actually read (0 means the peer closed cleanly, > 0 is real data), or
 * a negative Socket_Error on a real error. */
i64 socket_recv_some(i32 fd, void *buf, size_t len);

void socket_close(i32 fd);

const char *socket_strerror(i32 err);

#endif /* HOLYC_SOCKET_H_ */

#ifdef SOCKET_IMPLEMENTATION
#ifndef SOCKET_IMPLEMENTATION_
#define SOCKET_IMPLEMENTATION_

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>

const char *socket_strerror(i32 err)
{
    switch (err) {
    case SOCKET_ESUCCESS: return "success";
    case SOCKET_ERESOLVE: return "host resolution failed";
    case SOCKET_ECREATE:  return "socket() failed";
    case SOCKET_ECONNECT: return "connect() failed against every resolved address";
    case SOCKET_EBIND:    return "bind() failed";
    case SOCKET_ELISTEN:  return "listen() failed";
    case SOCKET_EACCEPT:  return "accept() failed";
    case SOCKET_EIO:      return "send()/recv() failed mid-transfer";
    case SOCKET_ECLOSED:  return "peer closed the connection early";
    default: break;
    }
    return "unknown error";
}

i32 socket_connect(const char *host, u16 port)
{
    struct addrinfo hints, *res, *rp;
    char port_str[8];
    i32 fd;
    i32 n;

    socket_memset_(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    n = 0;
    {
        u16 p = port;
        char tmp[8];
        i32 tn = 0;
        if (p == 0) {
            tmp[tn++] = '0';
        } else {
            while (p > 0) { tmp[tn++] = (char)('0' + (p % 10)); p /= 10; }
        }
        while (tn > 0) port_str[n++] = tmp[--tn];
        port_str[n] = '\0';
    }

    if (getaddrinfo(host, port_str, &hints, &res) != 0) return SOCKET_ERESOLVE;

    fd = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) return SOCKET_ECONNECT;
    return fd;
}

i32 socket_listen(u16 port, i32 backlog)
{
    i32 fd;
    i32 yes = 1;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return SOCKET_ECREATE;

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    socket_memset_(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return SOCKET_EBIND;
    }

    if (listen(fd, backlog) < 0) {
        close(fd);
        return SOCKET_ELISTEN;
    }

    return fd;
}

i32 socket_accept(i32 listen_fd)
{
    i32 fd = accept(listen_fd, NULL, NULL);
    if (fd < 0) return SOCKET_EACCEPT;
    return fd;
}

i32 socket_send(i32 fd, const void *buf, size_t len)
{
    const u8 *p = (const u8 *)buf;
    size_t sent = 0;

    /* clang static analyzer flagged send/recv/close being handed an fd
     * with no validation at all - not a memory-safety bug (POSIX
     * handles a bad fd by returning -1/EBADF, not UB), but a real,
     * cheap-to-close gap: a caller bug (double-close, use of an fd
     * from a failed accept/connect that wasn't checked) would
     * otherwise silently hit the kernel instead of failing loud here. */
    HOLYC_REQUIRES(fd >= 0);

    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return SOCKET_EIO;
        }
        sent += (size_t)n;
    }
    return SOCKET_ESUCCESS;
}

i32 socket_recv(i32 fd, void *buf, size_t len)
{
    u8 *p = (u8 *)buf;
    size_t got = 0;

    HOLYC_REQUIRES(fd >= 0);

    while (got < len) {
        ssize_t n = recv(fd, p + got, len - got, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return SOCKET_EIO;
        }
        if (n == 0) return SOCKET_ECLOSED;
        got += (size_t)n;
    }
    return SOCKET_ESUCCESS;
}

i64 socket_recv_some(i32 fd, void *buf, size_t len)
{
    ssize_t n;
    HOLYC_REQUIRES(fd >= 0);
    do {
        n = recv(fd, buf, len, 0);
    } while (n < 0 && errno == EINTR);
    if (n < 0) return SOCKET_EIO;
    return (i64)n;
}

void socket_close(i32 fd)
{
    HOLYC_REQUIRES(fd >= 0);
    close(fd);
}

#endif /* SOCKET_IMPLEMENTATION_ */
#endif /* SOCKET_IMPLEMENTATION */
