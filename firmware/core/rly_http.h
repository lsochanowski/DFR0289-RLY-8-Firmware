/* Minimal HTTP/1.0-style request parsing for the web panel.
 *
 * One request per connection ("Connection: close"), whole request buffered (it is small:
 * a settings form is well under 1 KB). Nothing here touches lwIP, so it is host-tested.
 */
#ifndef RLY_HTTP_H
#define RLY_HTTP_H

#include <stddef.h>
#include <stdint.h>

#define RLY_HTTP_PATH_MAX 48
#define RLY_HTTP_QUERY_MAX 64

typedef enum { RLY_HTTP_GET = 1, RLY_HTTP_POST = 2, RLY_HTTP_OTHER = 3 } rly_http_method;

typedef enum {
    RLY_HTTP_INCOMPLETE = 0, /* keep reading */
    RLY_HTTP_READY = 1,      /* headers and full body are in the buffer */
    RLY_HTTP_BAD = -1,       /* malformed */
    RLY_HTTP_TOO_BIG = -2
} rly_http_parse_status;

typedef struct {
    rly_http_method method;
    char path[RLY_HTTP_PATH_MAX];   /* without the query string */
    char query[RLY_HTTP_QUERY_MAX]; /* raw, after '?' */
    const char *auth;               /* value of "Authorization:" (not NUL-terminated) */
    size_t auth_len;
    const char *body; /* points into the request buffer */
    size_t body_len;
    size_t content_length; /* announced body size */
    size_t head_len;       /* bytes up to and including the blank line */
} rly_http_request;

/* Parses buf[0..len). Returns READY once the headers and Content-Length bytes are present. */
rly_http_parse_status rly_http_parse(const char *buf, size_t len, rly_http_request *req);

/* Same, but READY as soon as the headers are complete (streamed uploads). body/body_len
 * then cover only the body bytes already in buf. */
rly_http_parse_status rly_http_parse_head(const char *buf, size_t len, rly_http_request *req);

/* Iterates "a=1&b=2" (application/x-www-form-urlencoded or a query string).
 * *pos starts at 0. Decodes '+' and %XX. Returns 1 while a pair was produced. */
int rly_http_form_next(const char *data, size_t len, size_t *pos, char *key, size_t key_cap,
                       char *value, size_t value_cap);

/* Looks up one field; returns 1 when found. */
int rly_http_form_get(const char *data, size_t len, const char *name, char *value, size_t cap);

/* Checks "Basic base64(user:pass)". */
int rly_http_check_basic(const char *auth, size_t auth_len, const char *user, const char *pass);

/* Appends text to out with HTML escaping (& < > " '). Returns new length, or cap on
 * overflow (sticky: the caller checks used >= cap once at the end). */
size_t rly_html_escape(char *out, size_t cap, size_t used, const char *text);
size_t rly_buf_append(char *out, size_t cap, size_t used, const char *text);
size_t rly_buf_append_uint(char *out, size_t cap, size_t used, uint32_t v);

#endif /* RLY_HTTP_H */
