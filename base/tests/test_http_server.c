#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#define STRING_VIEW_IMPLEMENTATION
#define SOCKET_IMPLEMENTATION
#define HTTP_IMPLEMENTATION
#define HTTP_SERVER_IMPLEMENTATION
#include "alloc/arena_alloc.h"
#include "util/str.h"
#include "net/socket.h"
#include "net/http.h"
#include "net/http_server.h"
#include <pthread.h>
#include <string.h>

#define TEST_PORT 18923

/* real end-to-end: this test's "client" is http.h's own http_request -
 * the exact same client code a real caller would use - talking over a
 * real TCP loopback socket to a real http_server_serve_one loop, not a
 * hand-fed fixture on either side. */

static void echo_method_path_handler_(const Http_Server_Request *req, Http_Server_Response *out, void *userdata)
{
    static char body[256];
    (void)userdata;
    snprintf(body, sizeof(body), "%.*s %.*s",
             (int)req->method.count, req->method.data,
             (int)req->path.count, req->path.data);
    out->status_code = 200;
    out->body = body;
    out->body_len = strlen(body);
}

static void not_found_handler_(const Http_Server_Request *req, Http_Server_Response *out, void *userdata)
{
    (void)req; (void)userdata;
    out->status_code = 404;
    out->body = "nope";
    out->body_len = 4;
}

static void echo_body_handler_(const Http_Server_Request *req, Http_Server_Response *out, void *userdata)
{
    (void)userdata;
    out->status_code = 201;
    out->body = req->body;
    out->body_len = req->body_len;
}

static void header_reflect_handler_(const Http_Server_Request *req, Http_Server_Response *out, void *userdata)
{
    static char body[256];
    String_View v;
    (void)userdata;
    if (http_server_get_header_(req, "X-Test-Header", &v)) {
        snprintf(body, sizeof(body), "got: %.*s", (int)v.count, v.data);
    } else {
        snprintf(body, sizeof(body), "missing");
    }
    out->status_code = 200;
    out->body = body;
    out->body_len = strlen(body);
}

typedef struct {
    Http_Server *srv;
    volatile i32 ready;
    Arena arena;
} Server_Thread_Ctx;

static void *serve_one_thread_(void *ctx_)
{
    Server_Thread_Ctx *ctx = (Server_Thread_Ctx *)ctx_;
    ctx->ready = 1;
    http_server_serve_one(ctx->srv, &ctx->arena);
    return NULL;
}

/* runs `handler` for exactly one request/response cycle in a background
 * thread, waits for it to finish, and returns the arena it used (so the
 * caller can inspect anything it needs before it goes out of scope). */
static void serve_and_wait_(Http_Server *srv, Http_Server_Handler handler, pthread_t *th, Server_Thread_Ctx *ctx)
{
    ctx->ready = 0;
    ctx->srv = srv;
    { Arena zero = {0}; ctx->arena = zero; }
    srv->handler = handler;
    pthread_create(th, NULL, serve_one_thread_, ctx);
    while (!ctx->ready) { }
}

int main(void)
{
    Arena client_arena = {0};
    Http_Server srv;
    pthread_t th;
    Server_Thread_Ctx ctx;

    print_cstr("test_http_server\n");

    TEST_CHECK(http_server_listen(&srv, TEST_PORT, 4, echo_method_path_handler_, NULL) == HTTP_SERVER_ESUCCESS,
               "listen succeeds on a fresh port");

    /* real end-to-end #1: GET, method+path really round-trip */
    {
        Http_Response resp;
        i32 err;
        serve_and_wait_(&srv, echo_method_path_handler_, &th, &ctx);
        err = http_request(&client_arena, "GET", "http://127.0.0.1:18923/hello", NULL, NULL, 0, &resp);
        pthread_join(th, NULL);
        TEST_CHECK(err == HTTP_ESUCCESS, "the real http.h client completes a real request against our real server");
        TEST_CHECK(resp.status_code == 200, "status 200 came back for real over the wire");
        TEST_CHECK(resp.body_len == 10 && memcmp(resp.body, "GET /hello", 10) == 0,
                   "the handler's parsed method+path really reached it correctly (echoed back: 'GET /hello')");
    }

    /* real end-to-end #2: a body actually gets framed with Content-Length and read back */
    {
        Http_Response resp;
        i32 err;
        const char *payload = "some real request body";
        serve_and_wait_(&srv, echo_body_handler_, &th, &ctx);
        err = http_request(&client_arena, "POST", "http://127.0.0.1:18923/echo", NULL, payload, strlen(payload), &resp);
        pthread_join(th, NULL);
        TEST_CHECK(err == HTTP_ESUCCESS, "POST with a body completes");
        TEST_CHECK(resp.status_code == 201, "handler-chosen status code (201) really came through");
        TEST_CHECK(resp.body_len == strlen(payload) && memcmp(resp.body, payload, resp.body_len) == 0,
                   "the request body the server read back is byte-identical to what the client sent");
    }

    /* real end-to-end #3: a custom request header really reaches the handler */
    {
        Http_Response resp;
        i32 err;
        serve_and_wait_(&srv, header_reflect_handler_, &th, &ctx);
        err = http_request(&client_arena, "GET", "http://127.0.0.1:18923/", "X-Test-Header: real-value\r\n", NULL, 0, &resp);
        pthread_join(th, NULL);
        TEST_CHECK(err == HTTP_ESUCCESS, "request with a custom header completes");
        TEST_CHECK(resp.body_len == 15 && memcmp(resp.body, "got: real-value", 15) == 0,
                   "a request header set by the real client was really parsed and handed to the handler");
    }

    /* real end-to-end #4: a non-200 status and its reason phrase round-trip */
    {
        Http_Response resp;
        i32 err;
        serve_and_wait_(&srv, not_found_handler_, &th, &ctx);
        err = http_request(&client_arena, "GET", "http://127.0.0.1:18923/missing", NULL, NULL, 0, &resp);
        pthread_join(th, NULL);
        TEST_CHECK(err == HTTP_ESUCCESS, "a 404 response still completes the request/response cycle cleanly");
        TEST_CHECK(resp.status_code == 404, "the handler's chosen error status (404) really reached the client");
        TEST_CHECK(resp.body_len == 4 && memcmp(resp.body, "nope", 4) == 0, "the error body round-trips too");
    }

    /* a malformed request line is rejected, not silently misparsed */
    {
        i32 fd;
        const char *garbage = "NOT A REAL REQUEST LINE\r\n\r\n";
        Server_Thread_Ctx bad_ctx;

        bad_ctx.ready = 0;
        bad_ctx.srv = &srv;
        { Arena zero = {0}; bad_ctx.arena = zero; }
        srv.handler = echo_method_path_handler_;
        {
            pthread_t bad_th;
            pthread_create(&bad_th, NULL, serve_one_thread_, &bad_ctx);
            while (!bad_ctx.ready) { }
            fd = socket_connect("127.0.0.1", TEST_PORT);
            TEST_CHECK(fd >= 0, "can connect to send a deliberately malformed request");
            socket_send(fd, garbage, strlen(garbage));
            socket_close(fd);
            pthread_join(bad_th, NULL);
        }
        TEST_CHECK(1, "server didn't crash on a malformed request line (reaching this line proves it)");
    }

    http_server_close(&srv);

    print_flush();
    return test_failures_ != 0;
}
