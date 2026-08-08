/* http.c - GET a URL.
 *
 * Milestone 39. HTTP/1.0 with an explicit Host header, which is HTTP/1.1's
 * one indispensable feature grafted onto the version that has no chunked
 * encoding and no keep-alive to implement. The server closes the
 * connection when the body ends, and that is the framing.
 *
 * Which is a real limitation and worth naming: a server that answers 1.0
 * with a chunked 1.1 body would confuse this, and so would one that keeps
 * the connection open. Neither is allowed by the RFC for a 1.0 request,
 * and the alternative - a chunked decoder and a keep-alive state machine
 * - is a lot of parser for an updater that talks to one server.
 *
 * Content-Length is honoured when present, because a download with a
 * known size can report progress and can tell truncation from
 * completion. Without it, the body is whatever arrived before the close.
 */

#include "http.h"
#include "tcp.h"
#include "dns.h"
#include "net.h"
#include "kstring.h"
#include "pit.h"

static int is_digit(char c) { return c >= '0' && c <= '9'; }

/* Splits "http://host:port/path" into its parts. No https - TLS is a
 * different project - and no userinfo, query or fragment handling beyond
 * passing them through as part of the path, which is what a server
 * wants anyway. */
int http_parse_url(const char* url, char* host, uint32_t host_size,
                   uint16_t* port, char* path, uint32_t path_size) {
    *port = 80;

    if (kstrncmp(url, "http://", 7) == 0) url += 7;
    else if (kstrncmp(url, "https://", 8) == 0) return 0;   /* no TLS */

    uint32_t n = 0;
    while (*url && *url != '/' && *url != ':' && n < host_size - 1) {
        host[n++] = *url++;
    }
    host[n] = '\0';
    if (!n) return 0;

    if (*url == ':') {
        url++;
        uint32_t v = 0;
        while (is_digit(*url)) v = v * 10 + (uint32_t)(*url++ - '0');
        if (!v || v > 65535) return 0;
        *port = (uint16_t)v;
    }

    n = 0;
    if (*url != '/' && path_size > 1) path[n++] = '/';
    while (*url && n < path_size - 1) path[n++] = *url++;
    path[n] = '\0';
    if (!n) { path[0] = '/'; path[1] = '\0'; }

    return 1;
}

/* Finds the end of the headers and the start of the body, and reads
 * Content-Length on the way past. Returns the body offset, or 0 if the
 * headers are not complete yet. */
static uint32_t split_headers(const uint8_t* buf, uint32_t len,
                              int* status_out, uint32_t* length_out) {
    *status_out = 0;
    *length_out = 0;

    /* "HTTP/1.x NNN " */
    if (len < 12 || kstrncmp((const char*)buf, "HTTP/", 5) != 0) return 0;
    uint32_t i = 5;
    while (i < len && buf[i] != ' ') i++;
    while (i < len && buf[i] == ' ') i++;
    int status = 0;
    while (i < len && is_digit((char)buf[i])) status = status * 10 + (buf[i++] - '0');
    *status_out = status;

    for (i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            /* Content-Length, case-insensitively, anywhere in the block. */
            for (uint32_t j = 0; j + 15 < i; j++) {
                if (kstrncasecmp((const char*)buf + j, "content-length:", 15) != 0) {
                    continue;
                }
                uint32_t k = j + 15;
                while (k < i && buf[k] == ' ') k++;
                uint32_t v = 0;
                while (k < i && is_digit((char)buf[k])) {
                    v = v * 10 + (uint32_t)(buf[k++] - '0');
                }
                *length_out = v;
                break;
            }
            return i + 4;
        }
    }
    return 0;
}

int http_get(const char* url, uint8_t* body, uint32_t body_max,
             http_result_t* result) {
    char host[128], path[256];
    uint16_t port;

    result->status = 0;
    result->body_len = 0;
    result->content_length = 0;
    result->truncated = 0;

    if (!http_parse_url(url, host, sizeof(host), &port, path, sizeof(path))) {
        return HTTP_ERR_URL;
    }

    uint32_t ip = 0;
    if (!dns_resolve(host, &ip, 500)) return HTTP_ERR_DNS;
    result->server_ip = ip;

    int conn = tcp_connect_wait(ip, port, 1000);
    if (conn < 0) return HTTP_ERR_CONNECT;

    /* The request. HTTP/1.0 plus Host, and Connection: close said
     * explicitly so a 1.1 server that would otherwise keep the socket
     * open does not. */
    static char request[512];
    char* p = request;
    const char* s;

    for (s = "GET "; *s; ) *p++ = *s++;
    for (s = path; *s; ) *p++ = *s++;
    for (s = " HTTP/1.0\r\nHost: "; *s; ) *p++ = *s++;
    for (s = host; *s; ) *p++ = *s++;
    for (s = "\r\nUser-Agent: Novaris/1.0\r\nConnection: close\r\n\r\n"; *s; ) *p++ = *s++;
    uint32_t request_len = (uint32_t)(p - request);

    if (tcp_send_all(conn, (const uint8_t*)request, request_len, 500) < 0) {
        tcp_close(conn);
        return HTTP_ERR_SEND;
    }

    /* Headers first, into their own buffer, so that `body` holds only the
     * body and a caller writing it to a file writes the file. */
    static uint8_t head[2048];
    uint32_t head_len = 0;
    uint32_t body_start = 0;
    int status = 0;
    uint32_t content_length = 0;

    /* An *idle* deadline, reset every time bytes arrive, rather than a
     * total one. A total timeout is a maximum download size in disguise:
     * thirty seconds at any rate is a limit on the file, not on the
     * server, and an updater that fetches a 48MB initrd would fail
     * against a server that was working perfectly. Thirty seconds of
     * silence is a dead connection; thirty seconds of transfer is a
     * transfer. */
    const uint32_t idle_ticks = 3000;                /* thirty seconds */
    uint32_t deadline = pit_get_ticks() + idle_ticks;

    while (pit_get_ticks() < deadline) {
        net_poll();

        uint8_t chunk[512];
        int n = tcp_recv(conn, chunk, sizeof(chunk));

        /* Read first, then acknowledge - see the note in the body loop
         * below, which is the one that carries the bytes. */
        tcp_tick();
        if (n > 0) {
            deadline = pit_get_ticks() + idle_ticks;
            uint32_t take = (uint32_t)n;
            if (head_len + take > sizeof(head)) take = sizeof(head) - head_len;
            kmemcpy(head + head_len, chunk, take);
            head_len += take;

            /* Anything past the header break in this chunk is body, and
             * has to be kept - it arrived in the same segment. */
            body_start = split_headers(head, head_len, &status, &content_length);
            if (body_start) {
                uint32_t extra = head_len - body_start;
                if (extra > body_max) { extra = body_max; result->truncated = 1; }
                kmemcpy(body, head + body_start, extra);
                result->body_len = extra;
                break;
            }
            if (head_len >= sizeof(head)) { tcp_close(conn); return HTTP_ERR_HEADERS; }
            continue;
        }

        if (tcp_eof(conn) || tcp_state(conn) == TCP_CLOSED) break;
        __asm__ __volatile__("hlt");
    }

    if (!body_start) {
        tcp_close(conn);
        return status ? HTTP_ERR_HEADERS : HTTP_ERR_TIMEOUT;
    }

    result->status = status;
    result->content_length = content_length;

    /* The rest of the body, until the length is met or the server closes. */
    while (pit_get_ticks() < deadline) {
        if (content_length && result->body_len >= content_length) break;

        net_poll();

        int n = tcp_recv(conn, body + result->body_len,
                         body_max - result->body_len);

        /* Acknowledged *after* the read, not before it.
         *
         * The window in an acknowledgement is however much room is left
         * in the receive ring at the moment it is built. Acknowledging
         * before draining therefore reports the ring as the sender's own
         * data left it - full, or nearly - and the sender stops until
         * something tells it otherwise. Draining first means the same
         * acknowledgement carries the window the reader has just opened,
         * and the sender never pauses at all.
         *
         * The cost of the wrong order is one round trip per window rather
         * than a stall, so it does not look like a bug; it looks like a
         * link that is half as fast as it should be. */
        tcp_tick();
        if (n > 0) {
            deadline = pit_get_ticks() + idle_ticks;
            result->body_len += (uint32_t)n;
            if (result->body_len >= body_max) { result->truncated = 1; break; }
            continue;
        }
        if (tcp_eof(conn) || tcp_state(conn) == TCP_CLOSED) break;
        __asm__ __volatile__("hlt");
    }

    tcp_close(conn);
    return HTTP_OK;
}

const char* http_error_string(int err) {
    switch (err) {
        case HTTP_OK:           return "ok";
        case HTTP_ERR_URL:      return "not a URL this can fetch (http:// only)";
        case HTTP_ERR_DNS:      return "could not resolve the host";
        case HTTP_ERR_CONNECT:  return "could not connect";
        case HTTP_ERR_SEND:     return "could not send the request";
        case HTTP_ERR_HEADERS:  return "the reply's headers made no sense";
        case HTTP_ERR_TIMEOUT:  return "no reply in thirty seconds";
        default:                return "unknown error";
    }
}
