/* Non-blocking MQTT client on top of any byte stream.
 *
 * The point of the abstraction: the connection state machine (CONNECT -> CONNACK ->
 * SUBSCRIBE -> steady state, keepalive, dispatch of inbound commands) is identical no
 * matter whether the bytes come from lwIP sockets, USB CDC or a host test harness. That
 * keeps it testable on a laptop while the Ethernet PHY is still unresolved.
 */
#ifndef RLY_MQTT_CLIENT_H
#define RLY_MQTT_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#include "rly_session.h"

typedef struct {
    int (*open)(void *ctx);                            /* 0 = connected */
    int (*send)(void *ctx, const uint8_t *buf, size_t n); /* 0 = ok */
    int (*recv)(void *ctx, uint8_t *buf, size_t cap);  /* >0 bytes, 0 none, <0 closed */
    void (*close)(void *ctx);
    void *ctx;
} rly_stream;

typedef enum {
    RLY_MC_IDLE = 0,
    RLY_MC_CONNECTING,
    RLY_MC_READY,
    RLY_MC_ERROR
} rly_mc_state;

#define RLY_MC_RX_MAX 512
#define RLY_MC_SUB_ID 1u

typedef struct {
    rly_stream io;
    rly_session session;
    rly_mc_state state;
    uint32_t keepalive_s;
    uint32_t last_tx_ms;
    uint32_t reconnects;
    uint32_t retry_at_ms; /* earliest moment the client may dial again */
    uint32_t backoff_ms;  /* grows from 1 s to 30 s while the link keeps failing */
    size_t rx_len;
    uint8_t rx[RLY_MC_RX_MAX];
} rly_mqtt_client;

void rly_mc_init(rly_mqtt_client *c, const rly_stream *io, const rly_ha_config *ha,
                 const char *client_id, uint32_t keepalive_s);

/* Opens the stream and sends CONNECT. */
rly_status rly_mc_start(rly_mqtt_client *c, uint32_t now_ms);

/* Drains input, dispatches packets, keeps the link alive, and re-dials after a failure.
 * Call regularly with a monotonic millisecond clock. */
rly_status rly_mc_poll(rly_mqtt_client *c, uint32_t now_ms);

#endif /* RLY_MQTT_CLIENT_H */
