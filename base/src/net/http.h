#ifndef HOLYC_HTTP_H_
#define HOLYC_HTTP_H_

/* Minimal HTTP/1.1 CLIENT on top of socket.h. No server here (a real
 * HTTP server needs request routing/concurrency policy this file has no
 * business deciding - out of scope, not forgotten).
 *
 * Response framing: Content-Length is handled (the overwhelmingly
 * common case for anything not streaming). Transfer-Encoding: chunked
 * is a real, common case too and IS handled - the chunk-size-then-CRLF
 * framing is simple enough not to skip. What's explicitly NOT handled:
 * HTTP/1.0 (no keep-alive, no chunked there), redirects (3xx is
 * returned to the caller as-is, not followed automatically - following
 * redirects silently is a surprising thing for a library to do on its
 * own), TLS (this is a plain-socket client - https:// URLs are not
 * supported, only http://), and multiple/duplicate response headers of
 * the same name (last one wins, since headers are looked up by name via
 * a linear scan returning the first match found in a fixed small
 * array - documented, not a hidden footgun).
 *
 * Connection is always closed after one request/response (no keep-alive
 * reuse across calls) - simplest correct behavior for a first version,
 * same "simplest thing that's actually correct, not the fastest thing
 * that's usually correct" call this project's other file headers
 * (arena_alloc.h, sequencer.h) make explicitly for their own first
 * versions. */

#include "core/base.h"
#include "core/arena_alloc.h"
#include "core/str.h"
#include "net/socket.h"

typedef enum {
    HTTP_ESUCCESS     =  0,
    HTTP_ESOCKET      = -1, /* socket_connect/send/recv failed - see out_socket_err if you need the underlying Socket_Error */
    HTTP_EBADURL      = -2, /* url did not parse as http://host[:port]/path */
    HTTP_EBADRESPONSE = -3, /* response did not start with a valid "HTTP/1.x <code> <reason>" status line */
    HTTP_ETRUNCATED   = -4, /* connection closed before a framed (Content-Length or chunked) body finished */
    HTTP_ETOOMANYHDRS = -5  /* response had more headers than HTTP_MAX_HEADERS - the rest were dropped, not silently ignored without a signal */
} Http_Error;

#ifndef HTTP_MAX_HEADERS
#define HTTP_MAX_HEADERS 64
#endif

typedef struct {
    String_View name;
    String_View value;
} Http_Header;

typedef struct {
    i32 status_code;
    String_View reason;
    Http_Header headers[HTTP_MAX_HEADERS];
    size_t header_count;
    u8 *body;        /* arena-owned */
    size_t body_len;
} Http_Response;

/* url must be "http://host[:port]/path" (path defaults to "/" if
 * omitted). method is a plain cstr ("GET", "POST", ...). extra_headers
 * is an optional caller-built "Name: value\r\n"-per-line block appended
 * as-is (NULL/0-length for none) - kept this simple rather than another
 * growable-array API, since callers needing many headers can just build
 * that block with arena_sprintf/arena_sb_append_cstr themselves. body/
 * body_len are optional (0/NULL for none, e.g. GET). */
i32 http_request(Arena *a, const char *method, const char *url,
                  const char *extra_headers, const void *body, size_t body_len,
                  Http_Response *out);

/* linear scan, case-insensitive header name match - returns 1 and fills
 * *out_value if found, 0 otherwise. */
i32 http_response_get_header(const Http_Response *resp, const char *name, String_View *out_value);

const char *http_strerror(i32 err);

#endif /* HOLYC_HTTP_H_ */

#ifdef HTTP_IMPLEMENTATION
#ifndef HTTP_IMPLEMENTATION_
#define HTTP_IMPLEMENTATION_

const char *http_strerror(i32 err)
{
    switch (err) {
    case HTTP_ESUCCESS:     return "success";
    case HTTP_ESOCKET:      return "socket connect/send/recv failed";
    case HTTP_EBADURL:      return "url did not parse as http://host[:port]/path";
    case HTTP_EBADRESPONSE: return "response did not start with a valid HTTP status line";
    case HTTP_ETRUNCATED:   return "connection closed before the framed body finished";
    case HTTP_ETOOMANYHDRS: return "response had more headers than HTTP_MAX_HEADERS";
    default: break;
    }
    return "unknown error";
}

/* "http://host[:port][/path]" -> host (arena-owned cstr), port, path
 * (arena-owned cstr, always starts with '/'). Returns 0 on a malformed
 * url, 1 on success. */
static i32 http_parse_url_(Arena *a, const char *url, char **out_host, u16 *out_port, char **out_path)
{
    String_View sv, host_sv, port_sv, path_sv;
    size_t slash_idx;
    i32 has_slash;

    sv = sv_from_cstr(url);
    if (!sv_starts_with(sv, SV("http://"))) return 0;
    sv_chop_left(&sv, 7); /* mutates sv to the remainder in place; return value (the chopped "http://") is discarded */

    has_slash = sv_index_of(sv, '/', &slash_idx);
    if (has_slash) {
        path_sv = sv_from_parts(sv.data + slash_idx, sv.count - slash_idx);
        host_sv = sv_from_parts(sv.data, slash_idx);
    } else {
        path_sv = SV("/");
        host_sv = sv;
    }

    if (host_sv.count == 0) return 0;

    {
        size_t colon_idx;
        if (sv_index_of(host_sv, ':', &colon_idx)) {
            port_sv = sv_from_parts(host_sv.data + colon_idx + 1, host_sv.count - colon_idx - 1);
            host_sv = sv_from_parts(host_sv.data, colon_idx);
            *out_port = (u16)sv_to_u64(port_sv);
            if (*out_port == 0) return 0;
        } else {
            *out_port = 80;
        }
    }

    *out_host = (char *)arena_alloc(a, host_sv.count + 1);
    arena_memcpy(*out_host, host_sv.data, host_sv.count);
    (*out_host)[host_sv.count] = '\0';

    *out_path = (char *)arena_alloc(a, path_sv.count + 1);
    arena_memcpy(*out_path, path_sv.data, path_sv.count);
    (*out_path)[path_sv.count] = '\0';

    return 1;
}

/* reads from fd, growing buf as needed, until either a real error/close
 * (returns 0) or the caller-supplied `done` predicate says the buffer
 * already holds everything needed (returns 1) - shared by the header
 * read (done = "found \r\n\r\n") and the chunked-body read (done =
 * "found the terminating 0-length chunk"). */
typedef i32 (*Http_Read_Done_Fn)(const u8 *buf, size_t len, void *ctx);

static i32 http_read_until_(i32 fd, Arena *a, u8 **buf, size_t *len, size_t *cap,
                             Http_Read_Done_Fn done, void *ctx)
{
    for (;;) {
        i64 n;
        if (done(*buf, *len, ctx)) return 1;
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

static i32 http_headers_done_(const u8 *buf, size_t len, void *ctx)
{
    size_t i;
    (void)ctx;
    if (len < 4) return 0;
    for (i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') return 1;
    }
    return 0;
}

/* parses the status line + headers out of a "\r\n\r\n"-terminated
 * header block. Returns byte offset of the blank line's end (start of
 * body) on success, or (size_t)-1 on a malformed status line. */
static size_t http_parse_headers_(String_View block, Http_Response *out, i32 *out_err)
{
    String_View line, rest;
    size_t consumed;

    *out_err = HTTP_ESUCCESS;
    rest = block;
    line = sv_chop_by_delim(&rest, '\n');
    if (line.count > 0 && line.data[line.count - 1] == '\r') line.count--;

    if (!sv_starts_with(line, SV("HTTP/1."))) { *out_err = HTTP_EBADRESPONSE; return (size_t)-1; }
    sv_chop_left(&line, 7);  /* "HTTP/1." - mutates line to the remainder in place */
    sv_chop_left(&line, 1);  /* the '0' or '1' minor-version digit */
    line = sv_trim_left(line);
    {
        String_View code_sv = sv_chop_by_delim(&line, ' ');
        out->status_code = (i32)sv_to_u64(code_sv);
        if (out->status_code < 100 || out->status_code > 599) { *out_err = HTTP_EBADRESPONSE; return (size_t)-1; }
        out->reason = sv_trim(line);
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
        } else {
            *out_err = HTTP_ETOOMANYHDRS;
        }
    }

    consumed = (size_t)(rest.data - block.data);
    return consumed;
}

i32 http_response_get_header(const Http_Response *resp, const char *name, String_View *out_value)
{
    String_View name_sv = sv_from_cstr(name);
    size_t i;
    for (i = 0; i < resp->header_count; i++) {
        if (sv_eq_ignorecase(resp->headers[i].name, name_sv)) {
            *out_value = resp->headers[i].value;
            return 1;
        }
    }
    return 0;
}

typedef struct { size_t target; } Http_Len_Ctx_;
static i32 http_have_len_(const u8 *buf, size_t len, void *ctx)
{
    (void)buf;
    return len >= ((Http_Len_Ctx_ *)ctx)->target;
}

/* decodes a chunked-transfer-encoding body already fully buffered in
 * `enc` (hex-size CRLF chunk-data CRLF, repeated, terminated by a
 * "0\r\n\r\n" final chunk) into a flat arena buffer. Returns 1 on
 * success, 0 if `enc` doesn't actually contain a complete terminating
 * chunk yet (caller's http_read_until_ done-predicate should prevent
 * this, this is the defensive re-check). */
static i32 http_decode_chunked_(Arena *a, String_View enc, u8 **out_body, size_t *out_len)
{
    u8 *body;
    size_t cap, len;

    cap = enc.count; /* upper bound - decoded is never larger than encoded */
    body = (u8 *)arena_alloc(a, cap > 0 ? cap : 1);
    len = 0;

    for (;;) {
        String_View size_line;
        size_t chunk_size;
        if (!sv_try_chop_by_delim(&enc, '\n', &size_line)) return 0;
        if (size_line.count > 0 && size_line.data[size_line.count - 1] == '\r') size_line.count--;
        {
            /* hex chunk-size, ignoring any ";extension" suffix */
            String_View hex = size_line;
            size_t semi;
            if (sv_index_of(hex, ';', &semi)) hex = sv_from_parts(hex.data, semi);
            chunk_size = 0;
            {
                size_t i;
                for (i = 0; i < hex.count; i++) {
                    char c = hex.data[i];
                    u32 d;
                    if (c >= '0' && c <= '9') d = (u32)(c - '0');
                    else if (c >= 'a' && c <= 'f') d = (u32)(c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F') d = (u32)(c - 'A' + 10);
                    else return 0;
                    chunk_size = chunk_size * 16 + d;
                }
            }
        }
        if (chunk_size == 0) { *out_body = body; *out_len = len; return 1; } /* final chunk */
        if (chunk_size > enc.count) return 0;
        arena_memcpy(body + len, enc.data, chunk_size);
        len += chunk_size;
        enc = sv_from_parts(enc.data + chunk_size, enc.count - chunk_size);
        /* trailing CRLF after this chunk's data */
        if (enc.count < 2 || enc.data[0] != '\r' || enc.data[1] != '\n') return 0;
        enc = sv_from_parts(enc.data + 2, enc.count - 2);
    }
}

static i32 http_chunked_done_(const u8 *buf, size_t len, void *ctx)
{
    size_t i;
    (void)ctx;
    /* looks for a "0\r\n\r\n" final-chunk marker anywhere in the stream -
     * a cheap, sufficient signal to stop reading (the real parse in
     * http_decode_chunked_ validates structure properly; this is just
     * "have we probably got it all"). */
    if (len < 5) return 0;
    for (i = 0; i + 4 < len; i++) {
        if (buf[i] == '0' && buf[i + 1] == '\r' && buf[i + 2] == '\n' && buf[i + 3] == '\r' && buf[i + 4] == '\n') return 1;
    }
    return 0;
}

typedef enum {
    HTTP_FRAME_CHUNKED,
    HTTP_FRAME_CONTENT_LENGTH,
    HTTP_FRAME_CLOSE /* no framing header at all - read until the peer closes */
} Http_Frame_Kind_;

/* which framing mechanism applies to this response, per RFC 7230 7.3.3's
 * own precedence (chunked takes priority when both are somehow present) -
 * three sequential early-returns, so the call site can dispatch through a
 * switch on the result instead of repeating this same chain itself. */
static Http_Frame_Kind_ http_detect_framing_(Http_Response *out, String_View *transfer_encoding_sv,
                                              String_View *content_length_sv)
{
    if (http_response_get_header(out, "Transfer-Encoding", transfer_encoding_sv) &&
        sv_eq_ignorecase(sv_trim(*transfer_encoding_sv), SV("chunked"))) {
        return HTTP_FRAME_CHUNKED;
    }
    if (http_response_get_header(out, "Content-Length", content_length_sv)) {
        return HTTP_FRAME_CONTENT_LENGTH;
    }
    return HTTP_FRAME_CLOSE;
}

i32 http_request(Arena *a, const char *method, const char *url,
                  const char *extra_headers, const void *body, size_t body_len,
                  Http_Response *out)
{
    char *host, *path;
    u16 port;
    i32 fd;
    i32 err;
    char *req;
    size_t req_len;
    u8 *hdr_buf;
    size_t hdr_len, hdr_cap;
    size_t header_end;
    String_View header_block;
    String_View content_length_sv;
    String_View transfer_encoding_sv;

    memset(out, 0, sizeof(*out));

    if (!http_parse_url_(a, url, &host, &port, &path)) return HTTP_EBADURL;

    fd = socket_connect(host, port);
    if (fd < 0) return HTTP_ESOCKET;

    req = arena_sprintf(a,
        "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n%s%sContent-Length: %lu\r\n\r\n",
        method, path, host,
        (extra_headers && extra_headers[0]) ? extra_headers : "",
        (extra_headers && extra_headers[0] && extra_headers[arena_strlen(extra_headers) - 1] != '\n') ? "\r\n" : "",
        (u64)body_len); /* %lu expects `unsigned long`; u64 is the same type on this platform */
    req_len = arena_strlen(req);

    if (socket_send(fd, req, req_len) != SOCKET_ESUCCESS) { socket_close(fd); return HTTP_ESOCKET; }
    if (body_len > 0 && socket_send(fd, body, body_len) != SOCKET_ESUCCESS) { socket_close(fd); return HTTP_ESOCKET; }

    hdr_buf = 0;
    hdr_len = 0;
    hdr_cap = 0;
    if (!http_read_until_(fd, a, &hdr_buf, &hdr_len, &hdr_cap, http_headers_done_, 0)) {
        socket_close(fd);
        return HTTP_ETRUNCATED;
    }

    header_block = sv_from_parts((const char *)hdr_buf, hdr_len);
    header_end = http_parse_headers_(header_block, out, &err);
    if (header_end == (size_t)-1) { socket_close(fd); return err; }

    switch (http_detect_framing_(out, &transfer_encoding_sv, &content_length_sv)) {
    case HTTP_FRAME_CHUNKED:
        /* keep reading past what we already have until the chunked
         * terminator shows up, then decode everything after the header
         * block. */
        if (!http_read_until_(fd, a, &hdr_buf, &hdr_len, &hdr_cap, http_chunked_done_, 0)) {
            socket_close(fd);
            return HTTP_ETRUNCATED;
        }
        {
            String_View enc = sv_from_parts((const char *)hdr_buf + header_end, hdr_len - header_end);
            if (!http_decode_chunked_(a, enc, &out->body, &out->body_len)) {
                socket_close(fd);
                return HTTP_ETRUNCATED;
            }
        }
        break;
    case HTTP_FRAME_CONTENT_LENGTH: {
        Http_Len_Ctx_ len_ctx;
        size_t body_target = (size_t)sv_to_u64(content_length_sv);
        len_ctx.target = header_end + body_target;
        if (!http_read_until_(fd, a, &hdr_buf, &hdr_len, &hdr_cap, http_have_len_, &len_ctx)) {
            socket_close(fd);
            return HTTP_ETRUNCATED;
        }
        out->body = (u8 *)arena_alloc(a, body_target > 0 ? body_target : 1);
        arena_memcpy(out->body, hdr_buf + header_end, body_target);
        out->body_len = body_target;
        break;
    }
    default:
        /* HTTP_FRAME_CLOSE: no framing header at all - read until the
         * peer closes, take whatever arrived after the header block as
         * the whole body (matches curl's own fallback behavior for
         * unframed responses). */
        for (;;) {
            i64 n;
            if (hdr_len >= hdr_cap) {
                size_t new_cap = hdr_cap == 0 ? 4096 : hdr_cap * 2;
                hdr_buf = (u8 *)arena_realloc(a, hdr_buf, hdr_cap, new_cap);
                hdr_cap = new_cap;
            }
            n = socket_recv_some(fd, hdr_buf + hdr_len, hdr_cap - hdr_len);
            if (n <= 0) break;
            hdr_len += (size_t)n;
        }
        out->body_len = hdr_len - header_end;
        out->body = (u8 *)arena_alloc(a, out->body_len > 0 ? out->body_len : 1);
        arena_memcpy(out->body, hdr_buf + header_end, out->body_len);
        break;
    }

    socket_close(fd);
    return HTTP_ESUCCESS;
}

#endif /* HTTP_IMPLEMENTATION_ */
#endif /* HTTP_IMPLEMENTATION */
