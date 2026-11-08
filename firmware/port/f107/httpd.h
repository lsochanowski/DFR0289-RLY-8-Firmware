/* Tiny HTTP server on the lwIP raw TCP API (port 80).
 *
 * Two connection slots, each buffering one request (up to HTTPD_REQ_MAX bytes). Responses
 * are rendered into one shared buffer and queued with TCP_WRITE_FLAG_COPY; when a response
 * does not fit the send buffer at once, the rest goes out from the sent callback and the
 * shared buffer stays reserved until then (a second request meanwhile gets a short 503).
 */
#ifndef HTTPD_H
#define HTTPD_H

#include <stddef.h>

#include "../../core/rly_http.h"

#define HTTPD_REQ_MAX 1536
#define HTTPD_RESP_MAX 6144

/* Renders a response for a parsed request into out; returns its length. */
typedef size_t (*httpd_handler)(const rly_http_request *req, char *out, size_t cap);

int httpd_start(httpd_handler handler);
unsigned httpd_requests(void);

#endif /* HTTPD_H */
