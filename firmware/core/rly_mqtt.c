#include "rly_mqtt.h"

#include <string.h>

#define VARINT_MAX 4

static size_t varint_len(size_t value) {
    size_t n = 1;
    while (value > 127u) {
        value /= 128u;
        n++;
    }
    return n;
}

static size_t varint_write(size_t value, uint8_t *out) {
    size_t n = 0;
    do {
        uint8_t byte = (uint8_t)(value % 128u);
        value /= 128u;
        if (value > 0) {
            byte |= 0x80u;
        }
        out[n++] = byte;
    } while (value > 0 && n < VARINT_MAX);
    return n;
}

static int varint_read(const uint8_t *in, size_t n, size_t *value, size_t *used) {
    size_t multiplier = 1;
    size_t result = 0;
    for (size_t i = 0; i < VARINT_MAX && i < n; i++) {
        uint8_t byte = in[i];
        result += (size_t)(byte & 0x7Fu) * multiplier;
        if ((byte & 0x80u) == 0) {
            *value = result;
            *used = i + 1;
            return 0;
        }
        multiplier *= 128u;
    }
    return -1;
}

static size_t write_u16(uint16_t value, uint8_t *out) {
    out[0] = (uint8_t)(value >> 8);
    out[1] = (uint8_t)(value & 0xFFu);
    return 2;
}

static size_t write_string(const char *s, uint8_t *out) {
    size_t len = strlen(s);
    write_u16((uint16_t)len, out);
    memcpy(out + 2, s, len);
    return 2 + len;
}

size_t rly_mqtt_build_connect(const rly_mqtt_connect_opts *opts, uint8_t *out, size_t cap) {
    if (opts == NULL || out == NULL || opts->client_id == NULL) {
        return 0;
    }
    size_t client_len = strlen(opts->client_id);
    size_t will_topic_len = opts->will_topic ? strlen(opts->will_topic) : 0;
    size_t will_payload_len = opts->will_payload ? strlen(opts->will_payload) : 0;
    int has_will = opts->will_topic != NULL && opts->will_payload != NULL;
    int has_user = opts->username != NULL && opts->username[0] != '\0';
    int has_pass = has_user && opts->password != NULL && opts->password[0] != '\0';
    size_t user_len = has_user ? strlen(opts->username) : 0;
    size_t pass_len = has_pass ? strlen(opts->password) : 0;

    size_t remaining = 10u; /* protocol name + level + flags + keepalive */
    remaining += 2u + client_len;
    if (has_will) {
        remaining += 2u + will_topic_len + 2u + will_payload_len;
    }
    if (has_user) {
        remaining += 2u + user_len;
    }
    if (has_pass) {
        remaining += 2u + pass_len;
    }

    size_t total = 1u + varint_len(remaining) + remaining;
    if (total > cap || total > RLY_MQTT_MAX_PACKET) {
        return 0;
    }

    uint8_t flags = 0x02u; /* clean session */
    if (has_will) {
        flags |= 0x04u; /* will flag */
        flags |= 0x20u; /* will retain: availability must survive restarts */
    }
    if (has_user) {
        flags |= 0x80u;
    }
    if (has_pass) {
        flags |= 0x40u;
    }

    size_t used = 0;
    out[used++] = 0x10u;
    used += varint_write(remaining, out + used);
    used += write_string("MQTT", out + used);
    out[used++] = 0x04u; /* protocol level 3.1.1 */
    out[used++] = flags;
    used += write_u16(opts->keepalive_s, out + used);
    used += write_string(opts->client_id, out + used);
    if (has_will) {
        used += write_string(opts->will_topic, out + used);
        used += write_string(opts->will_payload, out + used);
    }
    if (has_user) {
        used += write_string(opts->username, out + used);
    }
    if (has_pass) {
        used += write_string(opts->password, out + used);
    }
    return used;
}

size_t rly_mqtt_build_publish(const char *topic, const uint8_t *payload, size_t payload_len,
                              int retain, int qos, uint16_t packet_id, uint8_t *out, size_t cap) {
    if (topic == NULL || out == NULL || (payload == NULL && payload_len > 0) || qos > 1) {
        return 0;
    }
    size_t topic_len = strlen(topic);
    if (qos > 0 && packet_id == 0) {
        return 0;
    }
    size_t remaining = 2u + topic_len + (qos > 0 ? 2u : 0u) + payload_len;
    size_t total = 1u + varint_len(remaining) + remaining;
    if (total > cap || total > RLY_MQTT_MAX_PACKET) {
        return 0;
    }
    size_t used = 0;
    out[used++] = (uint8_t)(0x30u | (retain ? 0x01u : 0u) | ((uint8_t)qos << 1));
    used += varint_write(remaining, out + used);
    used += write_string(topic, out + used);
    if (qos > 0) {
        used += write_u16(packet_id, out + used);
    }
    if (payload_len > 0) {
        memcpy(out + used, payload, payload_len);
        used += payload_len;
    }
    return used;
}

size_t rly_mqtt_build_subscribe(uint16_t packet_id, const char *topic_filter, int qos,
                                uint8_t *out, size_t cap) {
    if (topic_filter == NULL || out == NULL || packet_id == 0 || qos > 1) {
        return 0;
    }
    size_t filter_len = strlen(topic_filter);
    size_t remaining = 2u + 2u + filter_len + 1u;
    size_t total = 1u + varint_len(remaining) + remaining;
    if (total > cap || total > RLY_MQTT_MAX_PACKET) {
        return 0;
    }
    size_t used = 0;
    out[used++] = 0x82u; /* SUBSCRIBE, reserved bits 0b0010 */
    used += varint_write(remaining, out + used);
    used += write_u16(packet_id, out + used);
    used += write_string(topic_filter, out + used);
    out[used++] = (uint8_t)qos;
    return used;
}

size_t rly_mqtt_build_pingreq(uint8_t *out, size_t cap) {
    if (out == NULL || cap < 2) {
        return 0;
    }
    out[0] = 0xC0u;
    out[1] = 0x00u;
    return 2;
}

rly_status rly_mqtt_parse(const uint8_t *buf, size_t n, rly_mqtt_packet *out, size_t *consumed) {
    if (buf == NULL || out == NULL || consumed == NULL) {
        return RLY_ERR_ARG;
    }
    if (n < 2) {
        return RLY_ERR_SHORT;
    }
    size_t remaining = 0;
    size_t varint_used = 0;
    if (varint_read(buf + 1, n - 1, &remaining, &varint_used) != 0) {
        return RLY_ERR_LEN;
    }
    size_t header = 1u + varint_used;
    if (remaining > RLY_MQTT_MAX_PACKET) {
        return RLY_ERR_LEN;
    }
    if (n < header + remaining) {
        return RLY_ERR_SHORT;
    }

    memset(out, 0, sizeof *out);
    out->type = (rly_mqtt_pkt_type)(buf[0] >> 4);
    out->flags = (uint8_t)(buf[0] & 0x0Fu);
    out->payload = buf + header;
    out->payload_len = remaining;

    if (out->type == RLY_MQTT_PKT_PUBLISH) {
        const uint8_t *body = buf + header;
        if (remaining < 2) {
            return RLY_ERR_LEN;
        }
        size_t topic_len = ((size_t)body[0] << 8) | body[1];
        if (topic_len == 0 || topic_len >= RLY_MQTT_TOPIC_MAX || 2 + topic_len > remaining) {
            return RLY_ERR_LEN;
        }
        memcpy(out->topic, body + 2, topic_len);
        out->topic[topic_len] = '\0';
        size_t offset = 2 + topic_len;
        int qos = (out->flags >> 1) & 0x03;
        if (qos > 0) {
            if (offset + 2 > remaining) {
                return RLY_ERR_LEN;
            }
            out->packet_id = (uint16_t)(((uint16_t)body[offset] << 8) | body[offset + 1]);
            offset += 2;
        }
        out->payload = body + offset;
        out->payload_len = remaining - offset;
    }

    *consumed = header + remaining;
    return RLY_OK;
}
