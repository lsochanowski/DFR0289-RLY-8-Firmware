/* MQTT session logic for the RLY-8 replacement firmware.
 *
 * Transport-agnostic: the caller supplies send/recv hooks, so the whole state machine
 * (discovery, state publishing, inbound relay commands) is testable on a host with a
 * fake transport, before any STM32 or lwIP exists.
 */
#ifndef RLY_SESSION_H
#define RLY_SESSION_H

#include <stddef.h>
#include <stdint.h>

#include "rly_frame.h"
#include "rly_ha.h"
#include "rly_mqtt.h"

#define RLY_CLIENT_ID_MAX 32

typedef struct {
    int (*send)(void *ctx, const uint8_t *buf, size_t len);
    int (*recv)(void *ctx, uint8_t *buf, size_t cap); /* >0 bytes, 0 nothing yet, <0 error */
    void *ctx;
} rly_transport;

typedef struct {
    rly_transport io;
    rly_ha_config ha;
    char client_id[RLY_CLIENT_ID_MAX];
    uint16_t keepalive_s;
    const char *username; /* optional broker credentials (owned by the caller) */
    const char *password;
    uint8_t relay_mask;  /* desired/known relay state, bit0 = relay 1 */
    int connected;
    uint16_t next_packet_id;
    /* counters, useful in diagnostics and tests */
    unsigned discovery_published;
    unsigned state_published;
    unsigned commands_applied;
} rly_session;

void rly_session_init(rly_session *s, const rly_transport *io, const rly_ha_config *ha,
                      const char *client_id, uint16_t keepalive_s);

/* Sends CONNECT (with retained will on <node>/status). */
rly_status rly_session_connect(rly_session *s);

/* Publishes the 8 retained discovery configs and the availability topic. */
rly_status rly_session_announce(rly_session *s);

/* Publishes retained state for every relay. */
rly_status rly_session_publish_states(rly_session *s);

/* Sets one relay locally and publishes its state. */
rly_status rly_session_set_relay(rly_session *s, uint8_t relay, int on);

/* Handles one inbound packet; returns RLY_OK when consumed. */
rly_status rly_session_on_packet(rly_session *s, const rly_mqtt_packet *pkt);

#endif /* RLY_SESSION_H */
