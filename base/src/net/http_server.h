#ifndef HOLYC_HTTP_SERVER_H_
#define HOLYC_HTTP_SERVER_H_

/* Minimal HTTP/1.1 SERVER on top of socket.h - the deliberately-deferred
 * other half of http.h (see http.h's own top comment: "No server here...
 * out of scope, not forgotten"). Now built.
 *
 * Scope, stated as plainly as http.h states its own: single-threaded,
 * one request per connection (accept -> read -> parse -> call handler
 * -> write response -> close), no keep-alive, no TLS (plain HTTP only -
 * an "https_server" in name only until a real TLS decision is made
 * separately - pulling in a TLS library is a separate dependency
 * decision this file isn't making on its own), no routing table (the
 * caller's ONE handler callback sees the parsed method/path and decides
 * what to do - a real router is a thing a caller can build ON TOP of
 * this, not this file's job to impose). Concurrency policy is the
 * caller's: http_server_serve_one handles exactly one connection and
 * returns, so a caller wanting concurrent connections runs it in a
 * loop across threads/processes/an event loop - this file doesn't pick
 * for them, same stance http.h's own header comment already took.
 *
 * Request framing: Content-Length is read (chunked TE on a REQUEST body
 * is rare enough in practice - and genuinely more caller-hostile to
 * support correctly - that it's out of scope for this first version,
 * unlike http.h's client side which does handle chunked responses since
 * servers commonly send those). A request with a body but no
 * Content-Length is treated as a zero-length body, not a hang waiting
 * for a close that may never come. */

#include "core/base.h"
#include "core/arena_alloc.h"
#include "core/str.h"
#include "net/socket.h"
#include "net/http.h"

typedef struct {
    String_View method;
    String_View path;
    Http_Header headers[HTTP_MAX_HEADERS];
    size_t header_count;
    u8 *body;      /* arena-owned, body_len bytes, may be NULL if body_len == 0 */
    size_t body_len;
} Http_Server_Request;

typedef struct {
    i32 status_code;       /* e.g. 200, 404 - required */
    const char *reason;    /* NULL -> a small built-in table covers common codes, else "OK" */
    const char *extra_headers; /* optional "Name: value\r\n"-per-line block, NULL for none - same convention http_request's own extra_headers uses */
    const void *body;      /* optional, NULL/0 for an empty body */
    size_t body_len;
} Http_Server_Response;

typedef void (*Http_Server_Handler)(const Http_Server_Request *req, Http_Server_Response *out_resp, void *userdata);

typedef struct {
    i32 listen_fd;
    Http_Server_Handler handler;
    void *userdata;
} Http_Server;

typedef enum {
    HTTP_SERVER_ESUCCESS  =  0,
    HTTP_SERVER_ELISTEN   = -1, /* socket_listen failed */
    HTTP_SERVER_EACCEPT   = -2, /* socket_accept failed */
    HTTP_SERVER_EREAD     = -3, /* connection closed/errored before a full request arrived */
    HTTP_SERVER_EBADREQ   = -4  /* request line didn't parse as "METHOD path HTTP/1.x" */
} Http_Server_Error;

/* binds+listens on `port` (backlog connections queued). Returns
 * HTTP_SERVER_ESUCCESS or HTTP_SERVER_ELISTEN. */
i32 http_server_listen(Http_Server *srv, u16 port, i32 backlog, Http_Server_Handler handler, void *userdata);

/* accepts exactly ONE connection, reads+parses exactly one request,
 * calls srv->handler once, sends the response it filled in, closes the
 * connection. Returns HTTP_SERVER_ESUCCESS on a full successful cycle,
 * or one of the error codes above (the connection, if it was ever
 * accepted, is always closed before returning either way - no fd
 * leak on an error path). `a` is used for the request's body buffer
 * and header value copies - reset/scratch it between calls if serving
 * a loop of many requests, the same way any other per-call arena in
 * this codebase is used. */
i32 http_server_serve_one(Http_Server *srv, Arena *a);

void http_server_close(Http_Server *srv);

const char *http_server_strerror(i32 err);

#endif /* HOLYC_HTTP_SERVER_H_ */

#ifdef HTTP_SERVER_IMPLEMENTATION
#ifndef HTTP_SERVER_IMPLEMENTATION_
#define HTTP_SERVER_IMPLEMENTATION_

static i32 http_server_read_until_double_crlf_(i32 fd, Arena *a, u8 **buf, size_t *len, size_t *cap)
{
    for (;;) {
        i64 n;
        size_t i;
        for (i = 0; *len >= 4 && i + 3 < *len; i++) {
            if ((*buf)[i] == '\r' && (*buf)[i + 1] == '\n' && (*buf)[i + 2] == '\r' && (*buf)[i + 3] == '\n') return 1;
        }
        if (*len >= *cap) {
            size_t new_cap = *cap == 0 ? 4096 : *cap * 2;
            *buf = (u8 *)arena_realloc(a, *buf, *cap, new_cap);
            *cap = new_cap;
        }
        n = socket_recv_some(fd, *buf + *len, *cap - *len);
        if (n <= 0) return 0;
        *len += (size_t)n;
    }
}

/* parses "METHOD path HTTP/1.x\r\n" + headers out of `block` (already
 * known to contain a full "\r\n\r\n"-terminated header section).
 * Returns the byte offset where the body starts on success, (size_t)-1
 * on a malformed request line. Mirrors http.h's own response parser's
 * technique (String_View, line-by-line via sv_chop_by_delim) - not
 * literally shared code (http.h's own helpers are file-local static),
 * same real approach applied to a request line instead of a status
 * line. */
static size_t http_server_parse_request_(String_View block, Http_Server_Request *out, i32 *out_err)
{
    String_View line, rest;
    size_t consumed;

    *out_err = HTTP_SERVER_ESUCCESS;
    rest = block;
    line = sv_chop_by_delim(&rest, '\n');
    if (line.count > 0 && line.data[line.count - 1] == '\r') line.count--;

    out->method = sv_chop_by_delim(&line, ' ');
    out->path = sv_chop_by_delim(&line, ' ');
    line = sv_trim(line); /* whatever's left should be "HTTP/1.x" */

    if (out->method.count == 0 || out->path.count == 0 || !sv_starts_with(line, SV("HTTP/1."))) {
        *out_err = HTTP_SERVER_EBADREQ;
        return (size_t)-1;
    }

    out->header_count = 0;
    for (;;) {
        String_View name, value;
        if (rest.count == 0) break;
        line = sv_chop_by_delim(&rest, '\n');
        if (line.count > 0 && line.data[line.count - 1] == '\r') line.count--;
        if (line.count == 0) break; /* blank line: end of headers */

        if (!sv_try_chop_by_delim(&line, ':', &name)) continue;
        value = sv_trim(line);
        if (out->header_count < HTTP_MAX_HEADERS) {
            out->headers[out->header_count].name = name;
            out->headers[out->header_count].value = value;
            out->header_count++;
        }
    }

    consumed = (size_t)(rest.data - block.data);
    return consumed;
}

static i32 http_server_get_header_(const Http_Server_Request *req, const char *name, String_View *out_value)
{
    String_View name_sv = sv_from_cstr(name);
    size_t i;
    for (i = 0; i < req->header_count; i++) {
        if (sv_eq_ignorecase(req->headers[i].name, name_sv)) {
            *out_value = req->headers[i].value;
            return 1;
        }
    }
    return 0;
}

static const char *http_server_default_reason_(i32 code)
{
    switch (code) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 500: return "Internal Server Error";
    default:  return "OK";
    }
}

i32 http_server_listen(Http_Server *srv, u16 port, i32 backlog, Http_Server_Handler handler, void *userdata)
{
    srv->listen_fd = socket_listen(port, backlog);
    if (srv->listen_fd < 0) return HTTP_SERVER_ELISTEN;
    srv->handler = handler;
    srv->userdata = userdata;
    return HTTP_SERVER_ESUCCESS;
}

i32 http_server_serve_one(Http_Server *srv, Arena *a)
{
    i32 client_fd;
    u8 *buf = NULL;
    size_t len = 0, cap = 0;
    Http_Server_Request req;
    Http_Server_Response resp;
    String_View block;
    size_t body_start;
    i32 err = HTTP_SERVER_ESUCCESS;

    client_fd = socket_accept(srv->listen_fd);
    if (client_fd < 0) return HTTP_SERVER_EACCEPT;

    if (!http_server_read_until_double_crlf_(client_fd, a, &buf, &len, &cap)) {
        socket_close(client_fd);
        return HTTP_SERVER_EREAD;
    }

    block.data = (const char *)buf;
    block.count = len;
    body_start = http_server_parse_request_(block, &req, &err);
    if (body_start == (size_t)-1) {
        socket_close(client_fd);
        return err;
    }

    req.body = NULL;
    req.body_len = 0;
    {
        String_View cl_sv;
        if (http_server_get_header_(&req, "Content-Length", &cl_sv)) {
            size_t content_length = (size_t)sv_to_u64(cl_sv);
            size_t have = len - body_start;
            if (content_length > have) {
                size_t need = content_length - have;
                if (len + need > cap) {
                    size_t new_cap = len + need;
                    buf = (u8 *)arena_realloc(a, buf, cap, new_cap);
                    cap = new_cap;
                }
                {
                    size_t remaining = need;
                    while (remaining > 0) {
                        i64 n = socket_recv_some(client_fd, buf + len, remaining);
                        if (n <= 0) { socket_close(client_fd); return HTTP_SERVER_EREAD; }
                        len += (size_t)n;
                        remaining -= (size_t)n;
                    }
                }
            }
            req.body = buf + body_start;
            req.body_len = content_length;
        }
    }

    resp.status_code = 200;
    resp.reason = NULL;
    resp.extra_headers = NULL;
    resp.body = NULL;
    resp.body_len = 0;
    srv->handler(&req, &resp, srv->userdata);

    {
        const char *reason = resp.reason ? resp.reason : http_server_default_reason_(resp.status_code);
        char head[512];
        i32 head_len = sprintf(head,
            "HTTP/1.1 %d %s\r\nContent-Length: %lu\r\n%s\r\n",
            resp.status_code, reason, (unsigned long)resp.body_len,
            resp.extra_headers ? resp.extra_headers : "");
        socket_send(client_fd, head, (size_t)head_len);
        if (resp.body_len > 0) socket_send(client_fd, resp.body, resp.body_len);
    }

    socket_close(client_fd);
    return HTTP_SERVER_ESUCCESS;
}

void http_server_close(Http_Server *srv)
{
    socket_close(srv->listen_fd);
}

const char *http_server_strerror(i32 err)
{
    switch (err) {
    case HTTP_SERVER_ESUCCESS: return "success";
    case HTTP_SERVER_ELISTEN:  return "listen failed";
    case HTTP_SERVER_EACCEPT:  return "accept failed";
    case HTTP_SERVER_EREAD:    return "connection closed before a full request arrived";
    case HTTP_SERVER_EBADREQ:  return "malformed request line";
    default: return "unknown error";
    }
}

#endif /* HTTP_SERVER_IMPLEMENTATION_ */
#endif /* HTTP_SERVER_IMPLEMENTATION */
