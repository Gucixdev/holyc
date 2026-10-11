#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#define STRING_VIEW_IMPLEMENTATION
#define SOCKET_IMPLEMENTATION
#define HTTP_IMPLEMENTATION
#include "arena_alloc.h"
#include "str.h"
#include "socket.h"
#include "http.h"
#include <pthread.h>

#define TEST_PORT 18842

typedef struct {
    i32 listen_fd;
    volatile i32 ready;
    const char *raw_response; /* bytes to send back verbatim after reading the request */
    size_t raw_response_len;
} Fake_Server_Ctx;

/* accepts one connection, reads (and discards) whatever request arrives,
 * writes back a fixed raw response, closes. A hand-fed fake server is
 * simpler and more precise here than a real HTTP server would be - it
 * lets each test dictate the EXACT bytes the client has to parse,
 * including the tricky chunked/no-Content-Length cases. */
static void *fake_server_(void *ctx_)
{
    Fake_Server_Ctx *ctx = (Fake_Server_Ctx *)ctx_;
    i32 client_fd;
    char req_buf[4096];

    ctx->ready = 1;
    client_fd = socket_accept(ctx->listen_fd);
    if (client_fd < 0) return NULL;

    /* drain whatever the client sent (best-effort, not framed - we don't
     * need to understand the request, just get it off the wire before
     * replying) */
    {
        i64 n;
        do { n = socket_recv_some(client_fd, req_buf, sizeof(req_buf)); } while (n == (i64)sizeof(req_buf));
    }

    socket_send(client_fd, ctx->raw_response, ctx->raw_response_len);
    socket_close(client_fd);
    return NULL;
}

static void run_fake_server_(u16 port, const char *raw, Fake_Server_Ctx *ctx, pthread_t *th)
{
    ctx->ready = 0;
    ctx->raw_response = raw;
    ctx->raw_response_len = arena_strlen(raw);
    ctx->listen_fd = socket_listen(port, 1);
    pthread_create(th, NULL, fake_server_, ctx);
    while (!ctx->ready) { }
}

int main(void)
{
    Arena a = { 0 };

    print_cstr("test_http\n");

    /* --- Content-Length framed response --- */
    {
        Fake_Server_Ctx ctx;
        pthread_t th;
        Http_Response resp;
        i32 err;
        char url[64];
        String_View content_type = SV_NULL;

        run_fake_server_(TEST_PORT,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 5\r\n"
            "\r\n"
            "howdy",
            &ctx, &th);

        arena_sprintf(&a, ""); /* touch arena early so arena_sprintf below reuses the same region cleanly */
        {
            char *u = arena_sprintf(&a, "http://127.0.0.1:%d/hello", (i32)TEST_PORT);
            size_t i;
            for (i = 0; i < sizeof(url) - 1 && u[i]; i++) url[i] = u[i];
            url[i] = '\0';
        }

        err = http_request(&a, "GET", url, 0, 0, 0, &resp);
        TEST_CHECK(err == HTTP_ESUCCESS, "http_request succeeds against a Content-Length framed response");
        TEST_CHECK(resp.status_code == 200, "status code parsed as 200");
        TEST_CHECK(sv_eq(resp.reason, SV("OK")), "reason phrase parsed as OK");
        TEST_CHECK(resp.body_len == 5 && memcmp(resp.body, "howdy", 5) == 0,
                   "body bytes match the Content-Length framed body exactly");
        TEST_CHECK(http_response_get_header(&resp, "content-type", &content_type),
                   "header lookup is case-insensitive (asked for content-type, header was Content-Type)");
        TEST_CHECK(sv_eq(content_type, SV("text/plain")), "Content-Type value parsed correctly");

        pthread_join(th, NULL);
    }

    /* --- chunked transfer encoding --- */
    {
        Fake_Server_Ctx ctx;
        pthread_t th;
        Http_Response resp;
        i32 err;
        char url[64];

        run_fake_server_(TEST_PORT + 1,
            "HTTP/1.1 200 OK\r\n"
            "Transfer-Encoding: chunked\r\n"
            "\r\n"
            "4\r\n"
            "Wiki\r\n"
            "5\r\n"
            "pedia\r\n"
            "0\r\n"
            "\r\n",
            &ctx, &th);

        {
            char *u = arena_sprintf(&a, "http://127.0.0.1:%d/chunked", (i32)(TEST_PORT + 1));
            size_t i;
            for (i = 0; i < sizeof(url) - 1 && u[i]; i++) url[i] = u[i];
            url[i] = '\0';
        }

        err = http_request(&a, "GET", url, 0, 0, 0, &resp);
        TEST_CHECK(err == HTTP_ESUCCESS, "http_request succeeds against a chunked-encoded response");
        TEST_CHECK(resp.body_len == 9 && memcmp(resp.body, "Wikipedia", 9) == 0,
                   "chunked body decodes to the concatenation of every chunk's data, in order");

        pthread_join(th, NULL);
    }

    /* --- malformed status line rejected cleanly --- */
    {
        Fake_Server_Ctx ctx;
        pthread_t th;
        Http_Response resp;
        i32 err;
        char url[64];

        run_fake_server_(TEST_PORT + 2, "NOT AN HTTP RESPONSE AT ALL\r\n\r\n", &ctx, &th);

        {
            char *u = arena_sprintf(&a, "http://127.0.0.1:%d/bad", (i32)(TEST_PORT + 2));
            size_t i;
            for (i = 0; i < sizeof(url) - 1 && u[i]; i++) url[i] = u[i];
            url[i] = '\0';
        }

        err = http_request(&a, "GET", url, 0, 0, 0, &resp);
        TEST_CHECK(err == HTTP_EBADRESPONSE, "a garbage (non-HTTP) response is rejected as HTTP_EBADRESPONSE, not crashed on");

        pthread_join(th, NULL);
    }

    /* --- bad url rejected without touching the network at all --- */
    {
        Http_Response resp;
        i32 err = http_request(&a, "GET", "not-a-url-at-all", 0, 0, 0, &resp);
        TEST_CHECK(err == HTTP_EBADURL, "a non-http:// url is rejected as HTTP_EBADURL");
    }

    arena_free(&a);
    print_flush();
    return test_failures_ != 0;
}
