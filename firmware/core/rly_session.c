#include "rly_session.h"

#include <string.h>

static void copy_str(char *dst, size_t cap, const char *src) {
    if (dst == NULL || cap == 0) {
        return;
    }
    size_t n = src ? strlen(src) : 0;
    if (n >= cap) {
        n = cap - 1;
    }
    if (n > 0) {
        memcpy(dst, src, n);
    }
    dst[n] = '\0';
}

static rly_status send_raw(rly_session *s, const uint8_t *buf, size_t len) {
    if (s == NULL || s->io.send == NULL || buf == NULL || len == 0) {
        return RLY_ERR_ARG;
    }
    int sent = s->io.send(s->io.ctx, buf, len);
    return sent == (int)len ? RLY_OK : RLY_ERR_ARG;
}

/* Single-threaded firmware: keep the wire buffers off the stack so a small STM32
 * stack (often 1-2 KB) does not have to accommodate two 512-byte frames.
 * tx_frame holds the MQTT packet, discovery holds the JSON payload it publishes. */
static uint8_t tx_frame[RLY_MQTT_MAX_PACKET];
static char discovery_json[RLY_HA_DISCOVERY_MAX];

void rly_session_init(rly_session *s, const rly_transport *io, const rly_ha_config *ha,
                      const char *client_id, uint16_t keepalive_s) {
    if (s == NULL) {
        return;
    }
    memset(s, 0, sizeof *s);
    if (io != NULL) {
        s->io = *io;
    }
    if (ha != NULL) {
        s->ha = *ha;
    }
    copy_str(s->client_id, sizeof s->client_id, client_id);
    s->keepalive_s = keepalive_s;
    s->next_packet_id = 1;
}

rly_status rly_session_connect(rly_session *s) {
    if (s == NULL) {
        return RLY_ERR_ARG;
    }
    char availability[RLY_TOPIC_MAX];
    rly_ha_topics(&s->ha, 1, NULL, NULL, NULL, availability, sizeof availability);
    if (availability[0] == '\0') {
        return RLY_ERR_LEN;
    }
    rly_mqtt_connect_opts opts = {s->client_id, availability, "offline", s->keepalive_s,
                                  s->username, s->password};
    size_t n = rly_mqtt_build_connect(&opts, tx_frame, sizeof tx_frame);
    if (n == 0) {
        return RLY_ERR_LEN;
    }
    return send_raw(s, tx_frame, n);
}

static rly_status publish(rly_session *s, const char *topic, const char *payload, int retain) {
    if (topic == NULL || payload == NULL || topic[0] == '\0') {
        return RLY_ERR_ARG;
    }
    size_t n = rly_mqtt_build_publish(topic, (const uint8_t *)payload, strlen(payload), retain, 0, 0,
                                      tx_frame, sizeof tx_frame);
    if (n == 0) {
        return RLY_ERR_LEN;
    }
    return send_raw(s, tx_frame, n);
}

rly_status rly_session_announce(rly_session *s) {
    if (s == NULL) {
        return RLY_ERR_ARG;
    }
    char topic[RLY_TOPIC_MAX];

    for (uint8_t relay = 1; relay <= RLY_RELAY_COUNT; relay++) {
        size_t n = rly_ha_discovery(&s->ha, relay, discovery_json, sizeof discovery_json);
        if (n == 0) {
            return RLY_ERR_LEN;
        }
        rly_ha_topics(&s->ha, relay, topic, NULL, NULL, NULL, sizeof topic);
        if (topic[0] == '\0') {
            return RLY_ERR_LEN;
        }
        rly_status st = publish(s, topic, discovery_json, 1);
        if (st != RLY_OK) {
            return st;
        }
        s->discovery_published++;
    }

    char availability[RLY_TOPIC_MAX];
    rly_ha_topics(&s->ha, 1, NULL, NULL, NULL, availability, sizeof availability);
    return publish(s, availability, "online", 1);
}

rly_status rly_session_publish_states(rly_session *s) {
    if (s == NULL) {
        return RLY_ERR_ARG;
    }
    char state_topic[RLY_TOPIC_MAX];
    for (uint8_t relay = 1; relay <= RLY_RELAY_COUNT; relay++) {
        rly_ha_topics(&s->ha, relay, NULL, NULL, state_topic, NULL, sizeof state_topic);
        if (state_topic[0] == '\0') {
            return RLY_ERR_LEN;
        }
        int on = (s->relay_mask >> (relay - 1)) & 1;
        rly_status st = publish(s, state_topic, rly_ha_state_payload(on), 1);
        if (st != RLY_OK) {
            return st;
        }
        s->state_published++;
    }
    return RLY_OK;
}

rly_status rly_session_set_relay(rly_session *s, uint8_t relay, int on) {
    if (s == NULL || relay < 1 || relay > RLY_RELAY_COUNT) {
        return RLY_ERR_ARG;
    }
    uint8_t bit = (uint8_t)(1u << (relay - 1));
    if (on) {
        s->relay_mask |= bit;
    } else {
        s->relay_mask &= (uint8_t)~bit;
    }
    char state_topic[RLY_TOPIC_MAX];
    rly_ha_topics(&s->ha, relay, NULL, NULL, state_topic, NULL, sizeof state_topic);
    if (state_topic[0] == '\0') {
        return RLY_ERR_LEN;
    }
    rly_status st = publish(s, state_topic, rly_ha_state_payload(on != 0), 1);
    if (st != RLY_OK) {
        return st;
    }
    s->state_published++;
    return RLY_OK;
}

/* Matches "<node_id>/relay<N>/set". */
static int parse_relay_command_topic(const rly_ha_config *ha, const char *topic, uint8_t *relay) {
    if (ha == NULL || topic == NULL || relay == NULL) {
        return 0;
    }
    size_t node_len = strlen(ha->node_id);
    if (strncmp(topic, ha->node_id, node_len) != 0) {
        return 0;
    }
    const char *rest = topic + node_len;
    if (strncmp(rest, "/relay", 6) != 0) {
        return 0;
    }
    char digit = rest[6];
    if (digit < '1' || digit > '8') {
        return 0;
    }
    if (strcmp(rest + 7, "/set") != 0) {
        return 0;
    }
    *relay = (uint8_t)(digit - '0');
    return 1;
}

rly_status rly_session_on_packet(rly_session *s, const rly_mqtt_packet *pkt) {
    if (s == NULL || pkt == NULL) {
        return RLY_ERR_ARG;
    }
    switch (pkt->type) {
    case RLY_MQTT_PKT_CONNACK: {
        if (pkt->payload_len < 2) {
            return RLY_ERR_LEN;
        }
        if (pkt->payload[1] != 0) {
            s->connected = 0;
            return RLY_ERR_ARG; /* broker refused the connection */
        }
        s->connected = 1;
        rly_status st = rly_session_announce(s);
        if (st != RLY_OK) {
            return st;
        }
        return rly_session_publish_states(s);
    }
    case RLY_MQTT_PKT_PUBLISH: {
        uint8_t relay = 0;
        if (!parse_relay_command_topic(&s->ha, pkt->topic, &relay)) {
            return RLY_OK; /* not ours: ignore */
        }
        int on;
        if (pkt->payload_len == 2 && memcmp(pkt->payload, "ON", 2) == 0) {
            on = 1;
        } else if (pkt->payload_len == 3 && memcmp(pkt->payload, "OFF", 3) == 0) {
            on = 0;
        } else {
            return RLY_OK; /* unrecognised payload: ignore rather than guess */
        }
        rly_status st = rly_session_set_relay(s, relay, on);
        if (st != RLY_OK) {
            return st;
        }
        s->commands_applied++;
        return RLY_OK;
    }
    case RLY_MQTT_PKT_SUBACK:
    case RLY_MQTT_PKT_PINGRESP:
    case RLY_MQTT_PKT_PUBACK:
        return RLY_OK;
    default:
        return RLY_OK;
    }
}
