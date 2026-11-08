/* Tiny HTTP server on the lwIP raw TCP API (port 80).
 *
 * Two connection slots, each buffering one request (up to HTTPD_REQ_MAX bytes). Responses
 * are rendered into one shared buffer and queued with TCP_WRITE_FLAG_COPY; when a response
 * does not fit the send buffer at once, the rest goes out from the sent callback and the
 * shared buffer stays reserved until then (a second request meanwhile gets a short 503).
 *
 * One upload path (firmware update) is streamed instead of buffered: once its headers are
 * in, begin() decides, every body byte goes to data() as it arrives, end() renders the
 * response. abort() is called when the connection drops mid-upload.
 */
#ifndef HTTPD_H
#define HTTPD_H

#include <stddef.h>
#include <stdint.h>

#include "../../core/rly_http.h"

#define HTTPD_REQ_MAX 1536
#define HTTPD_RESP_MAX 6144

/* Renders a response for a parsed request into out; returns its length. */
typedef size_t (*httpd_handler)(const rly_http_request *req, char *out, size_t cap);

typedef struct {
    const char *path;
    /* 1 = accept the upload; 0 = reject, with a complete response written to out */
    int (*begin)(const rly_http_request *req, char *out, size_t cap, size_t *resp_len);
    void (*data)(const uint8_t *data, size_t n);
    size_t (*end)(char *out, size_t cap);
    void (*abort)(void);
} httpd_upload;

int httpd_start(httpd_handler handler);
void httpd_set_upload(const httpd_upload *upload);
unsigned httpd_requests(void);

#endif /* HTTPD_H */
