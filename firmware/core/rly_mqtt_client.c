#include "rly_mqtt_client.h"

#include <string.h>

static uint8_t tx_buf[RLY_MQTT_MAX_PACKET];

/* Retry policy: 1 s, 2 s, 4 s ... capped at 30 s while the broker stays unreachable.
 * Reset to the minimum as soon as a CONNACK is accepted. */
#define RLY_MC_BACKOFF_MIN_MS 1000u
#define RLY_MC_BACKOFF_MAX_MS 30000u

static void mark_error(rly_mqtt_client *c, uint32_t now_ms) {
    if (c->state != RLY_MC_ERROR) {
        c->retry_at_ms = now_ms + c->backoff_ms;
        c->backoff_ms = c->backoff_ms >= RLY_MC_BACKOFF_MAX_MS ? RLY_MC_BACKOFF_MAX_MS
                                                               : c->backoff_ms * 2u;
    }
    c->state = RLY_MC_ERROR;
    if (c->io.close != NULL) {
        c->io.close(c->io.ctx);
    }
}

static rly_status stream_send(rly_mqtt_client *c, const uint8_t *buf, size_t n, uint32_t now_ms) {
    if (c->io.send == NULL || n == 0) {
        return RLY_ERR_ARG;
    }
    if (c->io.send(c->io.ctx, buf, n) != 0) {
        return RLY_ERR_ARG;
    }
    c->last_tx_ms = now_ms;
    return RLY_OK;
}

void rly_mc_init(rly_mqtt_client *c, const rly_stream *io, const rly_ha_config *ha,
                 const char *client_id, uint32_t keepalive_s) {
    if (c == NULL) {
        return;
    }
    memset(c, 0, sizeof *c);
    if (io != NULL) {
        c->io = *io;
    }
    rly_transport transport = {0};
    /* The session publishes through the same stream: reuse its send hook shape. */
    transport.ctx = c;
    rly_session_init(&c->session, &transport, ha, client_id, (uint16_t)keepalive_s);
    c->keepalive_s = keepalive_s;
    c->state = RLY_MC_IDLE;
    c->backoff_ms = RLY_MC_BACKOFF_MIN_MS;
    c->retry_at_ms = 0;
}

/* Session publishes go through the client's stream. */
static int session_send(void *ctx, const uint8_t *buf, size_t len) {
    rly_mqtt_client *c = (rly_mqtt_client *)ctx;
    if (c == NULL || c->io.send == NULL) {
        return -1;
    }
    return c->io.send(c->io.ctx, buf, len) == 0 ? (int)len : -1;
}

rly_status rly_mc_start(rly_mqtt_client *c, uint32_t now_ms) {
    if (c == NULL || c->io.open == NULL) {
        return RLY_ERR_ARG;
    }
    /* Wire the session's transport to this stream before anything is published. */
    rly_transport transport = {session_send, NULL, c};
    c->session.io = transport;
    c->session.connected = 0;
    c->rx_len = 0;

    if (c->io.open(c->io.ctx) != 0) {
        mark_error(c, now_ms);
        return RLY_ERR_ARG;
    }
    rly_status st = rly_session_connect(&c->session);
    if (st != RLY_OK) {
        mark_error(c, now_ms);
        return st;
    }
    c->state = RLY_MC_CONNECTING;
    c->last_tx_ms = now_ms;
    return RLY_OK;
}

static size_t consume(rly_mqtt_client *c, size_t n) {
    if (n >= c->rx_len) {
        c->rx_len = 0;
        return 0;
    }
    memmove(c->rx, c->rx + n, c->rx_len - n);
    c->rx_len -= n;
    return c->rx_len;
}

static rly_status on_connack(rly_mqtt_client *c, uint32_t now_ms) {
    /* Subscribe to the command topics: "<node>/relay+/set". */
    char filter[RLY_MQTT_TOPIC_MAX];
    size_t used = 0;
    const char *suffix = "/relay+/set";
    size_t suffix_len = strlen(suffix);
    const char *node = c->session.ha.node_id;
    size_t node_len = strlen(node);
    if (node_len + suffix_len + 1u >= sizeof filter) {
        return RLY_ERR_LEN;
    }
    memcpy(filter, node, node_len);
    used = node_len;
    memcpy(filter + used, suffix, suffix_len);
    used += suffix_len;
    filter[used] = '\0';

    size_t n = rly_mqtt_build_subscribe(RLY_MC_SUB_ID, filter, 0, tx_buf, sizeof tx_buf);
    if (n == 0) {
        return RLY_ERR_LEN;
    }
    return stream_send(c, tx_buf, n, now_ms);
}

rly_status rly_mc_poll(rly_mqtt_client *c, uint32_t now_ms) {
    if (c == NULL) {
        return RLY_ERR_ARG;
    }
    if (c->state == RLY_MC_IDLE || c->state == RLY_MC_ERROR) {
        return RLY_OK;
    }

    /* 1. Drain whatever the transport has. */
    if (c->io.recv != NULL) {
        for (;;) {
            if (c->rx_len >= sizeof c->rx) {
                break;
            }
            int got = c->io.recv(c->io.ctx, c->rx + c->rx_len, sizeof c->rx - c->rx_len);
            if (got == 0) {
                break;
            }
            if (got < 0) {
                mark_error(c, now_ms);
                return RLY_OK;
            }
            c->rx_len += (size_t)got;
        }
    }

    /* 2. Handle every complete packet. */
    for (;;) {
        rly_mqtt_packet pkt;
        size_t consumed = 0;
        if (rly_mqtt_parse(c->rx, c->rx_len, &pkt, &consumed) != RLY_OK) {
            break;
        }
        int accepted = (pkt.type == RLY_MQTT_PKT_CONNACK && pkt.payload_len >= 2 &&
                        pkt.payload[1] == 0);
        if (accepted) {
            rly_status st = on_connack(c, now_ms);
            if (st != RLY_OK) {
                mark_error(c, now_ms);
                return st;
            }
        }
        if (rly_session_on_packet(&c->session, &pkt) != RLY_OK) {
            mark_error(c, now_ms);
            return RLY_ERR_ARG;
        }
        if (accepted) {
            c->state = RLY_MC_READY;
        }
        consume(c, consumed);
    }

    /* 3. Keepalive. */
    if (c->state == RLY_MC_READY && c->keepalive_s > 0u) {
        uint32_t interval = c->keepalive_s * 1000u;
        if (now_ms - c->last_tx_ms >= interval) {
            size_t n = rly_mqtt_build_pingreq(tx_buf, sizeof tx_buf);
            if (n == 0) {
                return RLY_ERR_LEN;
            }
            return stream_send(c, tx_buf, n, now_ms);
        }
    }
    return RLY_OK;
}
