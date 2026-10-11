/* netcat - minimal TCP pipe: `netcat HOST PORT` connects and shuttles
 * stdin/stdout over the socket; `netcat -l PORT` listens for one
 * connection and does the same. MVP: TCP/IPv4 only, one connection,
 * no UDP mode - real scope cut vs full nc(1). */

#define BASE_TYPE_IMPLEMENTATION
#include "base.h"
#include "patches.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netdb.h>

static void netcat_pump(int sock)
{
    fd_set fds;
    char buf[4096];
    i32 stdin_open = 1;

    for (;;) {
        ssize_t n;
        FD_ZERO(&fds);
        if (stdin_open) FD_SET(0, &fds);
        FD_SET(sock, &fds);
        if (select(sock + 1, &fds, NULL, NULL, NULL) < 0) break;

        if (stdin_open && FD_ISSET(0, &fds)) {
            n = read(0, buf, sizeof(buf));
            if (n <= 0) stdin_open = 0; /* EOF on stdin: stop feeding the socket, keep reading its replies */
            else write(sock, buf, (size_t)n);
        }
        if (FD_ISSET(sock, &fds)) {
            n = read(sock, buf, sizeof(buf));
            if (n <= 0) break; /* peer closed: nothing left to read */
            write(1, buf, (size_t)n);
        }
    }
}

int main(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "-l") == 0) {
        int listen_sock, conn;
        struct sockaddr_in addr;
        socklen_t addrlen = sizeof(addr);
        int port = atoi(argv[2]);
        int yes = 1;

        listen_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_sock < 0) { fprintf(stderr, "netcat: socket() failed\n"); return 1; }
        setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons((unsigned short)port);

        if (bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            fprintf(stderr, "netcat: bind failed on port %d\n", port);
            return 1;
        }
        listen(listen_sock, 1);
        conn = accept(listen_sock, (struct sockaddr *)&addr, &addrlen);
        close(listen_sock);
        netcat_pump(conn);
        close(conn);
        return 0;
    }

    if (argc >= 3) {
        struct addrinfo hints, *res;
        int sock;

        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(argv[1], argv[2], &hints, &res) != 0) {
            fprintf(stderr, "netcat: cannot resolve %s\n", argv[1]);
            return 1;
        }
        sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (sock < 0) { fprintf(stderr, "netcat: socket() failed\n"); freeaddrinfo(res); return 1; }
        if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
            fprintf(stderr, "netcat: cannot connect to %s:%s\n", argv[1], argv[2]);
            freeaddrinfo(res);
            return 1;
        }
        freeaddrinfo(res);
        netcat_pump(sock);
        close(sock);
        return 0;
    }

    fprintf(stderr, "netcat: usage: netcat HOST PORT | netcat -l PORT\n");
    return 1;
}
