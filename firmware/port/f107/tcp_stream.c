#include "tcp_stream.h"

#include <string.h>

#include "lwip/timeouts.h"

static err_t on_connected(void *arg, struct tcp_pcb *tpcb, err_t err) {
    (void)tpcb;
    lwip_tcp_stream *s = (lwip_tcp_stream *)arg;
    if (err == ERR_OK) {
        s->connected = 1;
    } else {
        s->failed = 1;
    }
    return ERR_OK;
}

static err_t on_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    lwip_tcp_stream *s = (lwip_tcp_stream *)arg;
    if (err != ERR_OK) {
        s->failed = 1;
    }
    if (p == NULL) {
        /* Remote closed the connection. */
        s->connected = 0;
        s->failed = 1;
        return ERR_OK;
    }
    if (s->rx_len + p->tot_len > sizeof s->rx) {
        /* Not enough room: drop the frame and tell lwIP we consumed it, so the connection
         * does not stall. The MQTT client will resynchronise on the next packet. */
        tcp_recved(tpcb, p->tot_len);
        pbuf_free(p);
        s->failed = 1;
        return ERR_OK;
    }
    pbuf_copy_partial(p, s->rx + s->rx_len, p->tot_len, 0);
    s->rx_len += p->tot_len;
    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static void on_error(void *arg, err_t err) {
    (void)err;
    lwip_tcp_stream *s = (lwip_tcp_stream *)arg;
    s->connected = 0;
    s->failed = 1;
    s->pcb = NULL; /* lwIP already freed it */
}

static int stream_open(void *ctx) {
    lwip_tcp_stream *s = (lwip_tcp_stream *)ctx;
    if (s == NULL) {
        return -1;
    }
    s->rx_len = 0;
    s->connected = 0;
    s->failed = 0;

    s->pcb = tcp_new();
    if (s->pcb == NULL) {
        return -1;
    }
    tcp_arg(s->pcb, s);
    tcp_recv(s->pcb, on_recv);
    tcp_err(s->pcb, on_error);

    if (tcp_connect(s->pcb, &s->broker, s->port, on_connected) != ERR_OK) {
        tcp_abort(s->pcb);
        s->pcb = NULL;
        return -1;
    }

    /* Pump until the handshake completes or the deadline passes. */
    uint32_t waited = 0;
    while (!s->connected && !s->failed && waited < s->connect_timeout_ms) {
        if (s->pump != NULL) {
            s->pump();
        }
        sys_check_timeouts();
        for (volatile uint32_t spin = 0; spin < 7000u; spin++) {
        }
        waited++;
    }
    if (!s->connected) {
        if (s->pcb != NULL) {
            tcp_abort(s->pcb);
            s->pcb = NULL;
        }
        return -1;
    }
    return 0;
}

static int stream_send(void *ctx, const uint8_t *buf, size_t n) {
    lwip_tcp_stream *s = (lwip_tcp_stream *)ctx;
    if (s == NULL || s->pcb == NULL || !s->connected || buf == NULL || n == 0) {
        return -1;
    }
    err_t err = tcp_write(s->pcb, buf, (u16_t)n, TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK) {
        return -1;
    }
    if (tcp_output(s->pcb) != ERR_OK) {
        return -1;
    }
    return 0;
}

static int stream_recv(void *ctx, uint8_t *buf, size_t cap) {
    lwip_tcp_stream *s = (lwip_tcp_stream *)ctx;
    if (s == NULL || buf == NULL || cap == 0) {
        return -1;
    }
    if (s->failed && s->rx_len == 0) {
        return -1;
    }
    if (s->rx_len == 0) {
        return 0;
    }
    size_t n = s->rx_len < cap ? s->rx_len : cap;
    memcpy(buf, s->rx, n);
    if (n < s->rx_len) {
        memmove(s->rx, s->rx + n, s->rx_len - n);
    }
    s->rx_len -= n;
    return (int)n;
}

static void stream_close(void *ctx) {
    lwip_tcp_stream *s = (lwip_tcp_stream *)ctx;
    if (s == NULL || s->pcb == NULL) {
        return;
    }
    tcp_arg(s->pcb, NULL);
    tcp_recv(s->pcb, NULL);
    tcp_err(s->pcb, NULL);
    if (tcp_close(s->pcb) != ERR_OK) {
        tcp_abort(s->pcb);
    }
    s->pcb = NULL;
    s->connected = 0;
}

void lwip_tcp_stream_init(lwip_tcp_stream *s, const ip_addr_t *broker, uint16_t port,
                          void (*pump)(void)) {
    if (s == NULL) {
        return;
    }
    memset(s, 0, sizeof *s);
    if (broker != NULL) {
        s->broker = *broker;
    }
    s->port = port;
    s->pump = pump;
    s->connect_timeout_ms = 2000; /* ~2 s of pumped polling */
}

rly_stream lwip_tcp_stream_interface(lwip_tcp_stream *s) {
    rly_stream io;
    io.open = stream_open;
    io.send = stream_send;
    io.recv = stream_recv;
    io.close = stream_close;
    io.ctx = s;
    return io;
}
