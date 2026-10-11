#include "test_common.h"
#define SOCKET_IMPLEMENTATION
#include "net/socket.h"
#include <pthread.h>
#include <string.h>

#define TEST_PORT 18742

typedef struct {
    i32 listen_fd;
    volatile i32 ready;
} Echo_Ctx;

/* accepts one connection, reads exactly 5 bytes, echoes them back
 * unmodified, then closes - simplest possible round-trip peer. */
static void *echo_server_(void *ctx_)
{
    Echo_Ctx *ctx = (Echo_Ctx *)ctx_;
    i32 client_fd;
    char buf[5];

    ctx->ready = 1;
    client_fd = socket_accept(ctx->listen_fd);
    if (client_fd < 0) return NULL;

    if (socket_recv(client_fd, buf, sizeof(buf)) == SOCKET_ESUCCESS) {
        socket_send(client_fd, buf, sizeof(buf));
    }
    socket_close(client_fd);
    return NULL;
}

/* accepts one connection, sends "hi" (2 bytes) then closes immediately -
 * exercises socket_recv's SOCKET_ECLOSED path when the caller asks for
 * more bytes than the peer ever sends. */
static void *short_server_(void *ctx_)
{
    Echo_Ctx *ctx = (Echo_Ctx *)ctx_;
    i32 client_fd;

    ctx->ready = 1;
    client_fd = socket_accept(ctx->listen_fd);
    if (client_fd < 0) return NULL;

    socket_send(client_fd, "hi", 2);
    socket_close(client_fd);
    return NULL;
}

/* accepts one connection, reads whatever partial byte-at-a-time chunks
 * arrive and reassembles them (real client sends via many tiny
 * socket_send calls below) - exercises the server side seeing a byte
 * stream, not a message boundary. */
static void *reassemble_server_(void *ctx_)
{
    Echo_Ctx *ctx = (Echo_Ctx *)ctx_;
    i32 client_fd;
    char buf[10];

    ctx->ready = 1;
    client_fd = socket_accept(ctx->listen_fd);
    if (client_fd < 0) return NULL;

    if (socket_recv(client_fd, buf, sizeof(buf)) == SOCKET_ESUCCESS) {
        socket_send(client_fd, buf, sizeof(buf));
    }
    socket_close(client_fd);
    return NULL;
}

int main(void)
{
    print_cstr("test_socket\n");

    /* --- basic connect/send/recv round trip --- */
    {
        Echo_Ctx ctx;
        pthread_t th;
        i32 fd;
        char sent[5] = { 'h', 'e', 'l', 'l', 'o' };
        char got[5];

        ctx.ready = 0;
        ctx.listen_fd = socket_listen(TEST_PORT, 1);
        TEST_CHECK(ctx.listen_fd >= 0, "socket_listen succeeds on a free local port");

        pthread_create(&th, NULL, echo_server_, &ctx);
        while (!ctx.ready) { /* spin until the server is listening/accepting */ }

        fd = socket_connect("127.0.0.1", TEST_PORT);
        TEST_CHECK(fd >= 0, "socket_connect succeeds against the listening server");

        TEST_CHECK(socket_send(fd, sent, sizeof(sent)) == SOCKET_ESUCCESS,
                   "socket_send reports success sending 5 bytes");
        TEST_CHECK(socket_recv(fd, got, sizeof(got)) == SOCKET_ESUCCESS,
                   "socket_recv reports success reading the 5-byte echo");
        TEST_CHECK(memcmp(sent, got, sizeof(sent)) == 0,
                   "echoed bytes match exactly what was sent");

        socket_close(fd);
        pthread_join(th, NULL);
    }

    /* --- SOCKET_ECLOSED: peer sends fewer bytes than requested, then closes --- */
    {
        Echo_Ctx ctx;
        pthread_t th;
        i32 fd;
        char buf[10];
        i32 err;

        ctx.ready = 0;
        ctx.listen_fd = socket_listen(TEST_PORT + 1, 1);
        pthread_create(&th, NULL, short_server_, &ctx);
        while (!ctx.ready) { }

        fd = socket_connect("127.0.0.1", TEST_PORT + 1);
        TEST_CHECK(fd >= 0, "socket_connect succeeds (short-server test)");

        err = socket_recv(fd, buf, sizeof(buf));
        TEST_CHECK(err == SOCKET_ECLOSED,
                   "socket_recv reports SOCKET_ECLOSED when the peer closes before enough bytes arrive");

        socket_close(fd);
        pthread_join(th, NULL);
    }

    /* --- partial-write correctness: many tiny sends reassembled correctly --- */
    {
        Echo_Ctx ctx;
        pthread_t th;
        i32 fd;
        char sent[10] = { 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j' };
        char got[10];
        size_t i;

        ctx.ready = 0;
        ctx.listen_fd = socket_listen(TEST_PORT + 2, 1);
        pthread_create(&th, NULL, reassemble_server_, &ctx);
        while (!ctx.ready) { }

        fd = socket_connect("127.0.0.1", TEST_PORT + 2);
        TEST_CHECK(fd >= 0, "socket_connect succeeds (reassembly test)");

        /* one socket_send call per byte - if socket_send didn't loop
         * correctly this would still work (each call IS the full
         * transfer for that call), so this mainly exercises the
         * server's socket_recv looping over many small arrivals into
         * one exact-length read. */
        for (i = 0; i < sizeof(sent); i++) {
            socket_send(fd, sent + i, 1);
        }
        TEST_CHECK(socket_recv(fd, got, sizeof(got)) == SOCKET_ESUCCESS,
                   "socket_recv reassembles many single-byte arrivals into one exact read");
        TEST_CHECK(memcmp(sent, got, sizeof(sent)) == 0,
                   "reassembled bytes match exactly what was sent one byte at a time");

        socket_close(fd);
        pthread_join(th, NULL);
    }

    /* --- error paths that don't need a real peer --- */
    {
        i32 fd = socket_connect("127.0.0.1", 1); /* port 1 - nothing listens there */
        TEST_CHECK(fd < 0, "socket_connect fails cleanly against a port nothing listens on");
    }

    print_flush();
    return test_failures_ != 0;
}
