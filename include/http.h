#ifndef HTTP_H
#define HTTP_H

#include <stdint.h>

/* http - GET a URL, into memory.
 *
 * Milestone 39. HTTP/1.0 with a Host header: the server closes the
 * connection when the body ends, and that close is the framing. See
 * kernel/http.c for what that rules out and why it is enough here.
 *
 * No https. TLS is a certificate store, a chain validator and three
 * ciphersuites - a project of its own - and shipping a client that says
 * "https" while checking nothing would be worse than not having one.
 */

#define HTTP_OK           0
#define HTTP_ERR_URL     -1
#define HTTP_ERR_DNS     -2
#define HTTP_ERR_CONNECT -3
#define HTTP_ERR_SEND    -4
#define HTTP_ERR_HEADERS -5
#define HTTP_ERR_TIMEOUT -6

typedef struct {
    int      status;          /* the HTTP status code, e.g. 200 */
    uint32_t body_len;        /* how much of the body was stored */
    uint32_t content_length;  /* what the server said, or 0 if it did not */
    uint32_t server_ip;
    int      truncated;       /* the body did not fit in the buffer given */
} http_result_t;

int http_get(const char* url, uint8_t* body, uint32_t body_max,
             http_result_t* result);

int http_parse_url(const char* url, char* host, uint32_t host_size,
                   uint16_t* port, char* path, uint32_t path_size);

const char* http_error_string(int err);

#endif /* HTTP_H */
