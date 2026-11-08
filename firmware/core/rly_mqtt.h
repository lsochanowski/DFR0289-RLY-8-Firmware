/* Minimal MQTT 3.1.1 client packet layer.
 *
 * Scope on purpose: what a relay board needs for Home Assistant.
 *   - CONNECT with a retained will (availability) and clean session
 *   - PUBLISH (QoS 0 or 1), with the retain flag
 *   - SUBSCRIBE (single topic filter)
 *   - PINGREQ keepalive
 *   - parsing of CONNACK and inbound PUBLISH
 *
 * Not implemented (deliberately, see docs/firmware-plan.md): QoS 1 retransmission,
 * DUP handling, TLS, packet fragmentation across reads.
 * Username/password auth is supported (MQTT 3.1.1 §3.1.2.8/9).
 */
#ifndef RLY_MQTT_H
#define RLY_MQTT_H

#include <stddef.h>
#include <stdint.h>

#include "rly_frame.h"

#define RLY_MQTT_MAX_PACKET 512
#define RLY_MQTT_TOPIC_MAX 96

typedef enum {
    RLY_MQTT_PKT_CONNECT = 1,
    RLY_MQTT_PKT_CONNACK = 2,
    RLY_MQTT_PKT_PUBLISH = 3,
    RLY_MQTT_PKT_PUBACK = 4,
    RLY_MQTT_PKT_SUBSCRIBE = 8,
    RLY_MQTT_PKT_SUBACK = 9,
    RLY_MQTT_PKT_PINGREQ = 12,
    RLY_MQTT_PKT_PINGRESP = 13
} rly_mqtt_pkt_type;

typedef struct {
    const char *client_id;
    const char *will_topic;
    const char *will_payload;
    uint16_t keepalive_s;
    const char *username; /* NULL or "" = none */
    const char *password; /* only sent together with a username */
} rly_mqtt_connect_opts;

typedef struct {
    rly_mqtt_pkt_type type;
    uint8_t flags;
    uint16_t packet_id; /* 0 when absent */
    char topic[RLY_MQTT_TOPIC_MAX];
    const uint8_t *payload; /* points into the parsed buffer */
    size_t payload_len;
} rly_mqtt_packet;

/* Each builder returns the packet length, or 0 when it does not fit. */
size_t rly_mqtt_build_connect(const rly_mqtt_connect_opts *opts, uint8_t *out, size_t cap);
size_t rly_mqtt_build_publish(const char *topic, const uint8_t *payload, size_t payload_len,
                              int retain, int qos, uint16_t packet_id, uint8_t *out, size_t cap);
size_t rly_mqtt_build_subscribe(uint16_t packet_id, const char *topic_filter, int qos,
                                uint8_t *out, size_t cap);
size_t rly_mqtt_build_pingreq(uint8_t *out, size_t cap);

/* Parses one packet from a byte stream. */
rly_status rly_mqtt_parse(const uint8_t *buf, size_t n, rly_mqtt_packet *out, size_t *consumed);

#endif /* RLY_MQTT_H */
