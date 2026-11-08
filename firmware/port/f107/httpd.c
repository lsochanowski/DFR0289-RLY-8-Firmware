#include "httpd.h"

#include <string.h>

#include "lwip/tcp.h"

#define SLOTS 2
#define IDLE_POLLS 20 /* tcp_poll every 500 ms -> 10 s idle timeout */

typedef struct {
    struct tcp_pcb *pcb;
    size_t len;
    const char *tx;   /* remaining response bytes (in resp_buf or a static string) */
    size_t tx_left;
    int owns_resp;    /* holds the shared response buffer */
    int idle;
    char req[HTTPD_REQ_MAX];
} slot;

static slot slots[SLOTS];

/* Connections accepted while both slots are busy. Their data stays queued inside lwIP
 * (the recv callback returns ERR_MEM, lwIP re-delivers it) until a slot frees up. */
#define WAITERS 4
typedef struct {
    struct tcp_pcb *pcb;
    uint8_t polls;
} waiter;
static waiter waiters[WAITERS];

static int is_waiter(void *arg) {
    return arg != NULL && (waiter *)arg >= &waiters[0] && (waiter *)arg < &waiters[WAITERS];
}
static char resp_buf[HTTPD_RESP_MAX];
static int resp_busy;
static httpd_handler handler_fn;
static unsigned request_count;

static const char BUSY[] = "HTTP/1.0 503 Service Unavailable\r\nRetry-After: 1\r\n"
                           "Connection: close\r\nContent-Length: 0\r\n\r\n";
static const char TOO_BIG[] = "HTTP/1.0 413 Payload Too Large\r\nConnection: close\r\n"
                              "Content-Length: 0\r\n\r\n";
static const char BAD[] = "HTTP/1.0 400 Bad Request\r\nConnection: close\r\n"
                          "Content-Length: 0\r\n\r\n";

unsigned httpd_requests(void) { return request_count; }

static void release(slot *s) {
    if (s->owns_resp) {
        resp_busy = 0;
        s->owns_resp = 0;
    }
    s->pcb = NULL;
    s->len = 0;
    s->tx = NULL;
    s->tx_left = 0;
}

static void finish(slot *s) {
    struct tcp_pcb *pcb = s->pcb;
    if (pcb != NULL) {
        tcp_arg(pcb, NULL);
        tcp_recv(pcb, NULL);
        tcp_sent(pcb, NULL);
        tcp_poll(pcb, NULL, 0);
        tcp_err(pcb, NULL);
        if (tcp_close(pcb) != ERR_OK) {
            tcp_abort(pcb);
        }
    }
    release(s);
}

/* Queues as much of the pending response as the send buffer takes. */
static void pump(slot *s) {
    while (s->tx_left > 0) {
        u16_t room = tcp_sndbuf(s->pcb);
        if (room == 0 || tcp_sndqueuelen(s->pcb) >= TCP_SND_QUEUELEN - 1) {
            break;
        }
        u16_t n = (u16_t)(s->tx_left < room ? s->tx_left : room);
        if (n > TCP_MSS) {
            n = TCP_MSS;
        }
        if (tcp_write(s->pcb, s->tx, n, TCP_WRITE_FLAG_COPY) != ERR_OK) {
            break;
        }
        s->tx += n;
        s->tx_left -= n;
    }
    tcp_output(s->pcb);
    if (s->tx_left == 0 && s->owns_resp) {
        resp_busy = 0; /* everything copied into lwIP; buffer free for the next request */
        s->owns_resp = 0;
    }
}

static void respond(slot *s, const char *data, size_t len, int owns) {
    s->tx = data;
    s->tx_left = len;
    s->owns_resp = owns;
    pump(s);
}

static err_t on_sent(void *arg, struct tcp_pcb *pcb, u16_t len) {
    (void)pcb;
    (void)len;
    slot *s = (slot *)arg;
    if (s == NULL || s->pcb == NULL) {
        return ERR_OK;
    }
    s->idle = 0;
    pump(s);
    if (s->tx_left == 0 && s->tx != NULL && tcp_sndqueuelen(s->pcb) == 0) {
        finish(s); /* all acknowledged */
    }
    return ERR_OK;
}

static void handle(slot *s) {
    rly_http_request req;
    rly_http_parse_status ps = rly_http_parse(s->req, s->len, &req);
    if (ps == RLY_HTTP_INCOMPLETE) {
        if (s->len >= sizeof s->req) {
            respond(s, TOO_BIG, sizeof TOO_BIG - 1, 0);
        }
        return;
    }
    if (ps != RLY_HTTP_READY) {
        respond(s, ps == RLY_HTTP_TOO_BIG ? TOO_BIG : BAD,
                ps == RLY_HTTP_TOO_BIG ? sizeof TOO_BIG - 1 : sizeof BAD - 1, 0);
        return;
    }
    if (resp_busy) {
        respond(s, BUSY, sizeof BUSY - 1, 0);
        return;
    }
    resp_busy = 1;
    request_count++;
    size_t n = handler_fn(&req, resp_buf, sizeof resp_buf);
    if (n >= sizeof resp_buf) {
        n = sizeof resp_buf - 1; /* truncated page is better than none */
    }
    respond(s, resp_buf, n, 1);
}

static slot *grab_slot(struct tcp_pcb *pcb);

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    if (is_waiter(arg)) {
        waiter *w = (waiter *)arg;
        if (p == NULL || err != ERR_OK) {
            w->pcb = NULL;
            tcp_arg(pcb, NULL);
            if (p != NULL) {
                pbuf_free(p);
            }
            if (tcp_close(pcb) != ERR_OK) {
                tcp_abort(pcb);
                return ERR_ABRT;
            }
            return ERR_OK;
        }
        slot *free_slot = grab_slot(pcb);
        if (free_slot == NULL) {
            return ERR_MEM; /* keep the data in lwIP, try again later */
        }
        w->pcb = NULL;
        arg = free_slot;
    }
    slot *s = (slot *)arg;
    if (p == NULL || err != ERR_OK || s == NULL) {
        if (p != NULL) {
            pbuf_free(p);
        }
        if (s != NULL) {
            finish(s);
        } else {
            tcp_close(pcb);
        }
        return ERR_OK;
    }
    tcp_recved(pcb, p->tot_len);
    s->idle = 0;
    if (s->tx == NULL) { /* still reading the request */
        size_t room = sizeof s->req - s->len;
        size_t n = p->tot_len < room ? p->tot_len : room;
        pbuf_copy_partial(p, s->req + s->len, (u16_t)n, 0);
        s->len += n;
        handle(s);
    }
    pbuf_free(p);
    return ERR_OK;
}

static err_t on_poll(void *arg, struct tcp_pcb *pcb) {
    if (is_waiter(arg)) {
        waiter *w = (waiter *)arg;
        if (++w->polls > IDLE_POLLS) {
            w->pcb = NULL;
            tcp_arg(pcb, NULL);
            tcp_abort(pcb);
            return ERR_ABRT;
        }
        return ERR_OK;
    }
    slot *s = (slot *)arg;
    if (s == NULL) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    if (s->tx_left > 0) {
        pump(s);
    }
    int someone_waits = 0;
    for (int i = 0; i < WAITERS; i++) {
        someone_waits |= waiters[i].pcb != NULL;
    }
    /* A browser's speculative connection that never sent a byte must not block a queued
     * real request: drop it after one poll period. */
    if (++s->idle > IDLE_POLLS || (someone_waits && s->len == 0 && s->tx == NULL && s->idle >= 2)) {
        release(s);
        tcp_arg(pcb, NULL);
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    return ERR_OK;
}

static void on_err(void *arg, err_t err) {
    (void)err;
    if (is_waiter(arg)) {
        ((waiter *)arg)->pcb = NULL;
        return;
    }
    slot *s = (slot *)arg;
    if (s != NULL) {
        release(s); /* pcb is already freed by lwIP */
    }
}

static slot *grab_slot(struct tcp_pcb *pcb) {
    for (int i = 0; i < SLOTS; i++) {
        if (slots[i].pcb == NULL) {
            slot *s = &slots[i];
            memset(s, 0, offsetof(slot, req));
            s->pcb = pcb;
            tcp_arg(pcb, s);
            return s;
        }
    }
    return NULL;
}

static err_t on_accept(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)arg;
    if (err != ERR_OK || pcb == NULL) {
        return ERR_VAL;
    }
    tcp_setprio(pcb, TCP_PRIO_MIN);
    tcp_recv(pcb, on_recv);
    tcp_sent(pcb, on_sent);
    tcp_err(pcb, on_err);
    tcp_poll(pcb, on_poll, 1);
    if (grab_slot(pcb) != NULL) {
        return ERR_OK;
    }
    for (int i = 0; i < WAITERS; i++) {
        if (waiters[i].pcb == NULL) {
            waiters[i].pcb = pcb;
            waiters[i].polls = 0;
            tcp_arg(pcb, &waiters[i]);
            return ERR_OK;
        }
    }
    tcp_abort(pcb);
    return ERR_ABRT;
}

int httpd_start(httpd_handler handler) {
    handler_fn = handler;
    struct tcp_pcb *pcb = tcp_new();
    if (pcb == NULL) {
        return -1;
    }
    if (tcp_bind(pcb, IP_ADDR_ANY, 80) != ERR_OK) {
        tcp_abort(pcb);
        return -1;
    }
    struct tcp_pcb *listen = tcp_listen(pcb);
    if (listen == NULL) {
        return -1;
    }
    tcp_accept(listen, on_accept);
    return 0;
}
