/* Host-side tests for the MQTT 3.1.1 packet layer. */
#include <stdio.h>
#include <string.h>

#include "../core/rly_mqtt.h"

static int failures;
static int checks;

static void expect_str(const char *what, const char *got, const char *want) {
    checks++;
    if (got == NULL || strcmp(got, want) != 0) {
        failures++;
        printf("FAIL %s\n  want: %s\n  got : %s\n", what, want, got ? got : "(null)");
        return;
    }
    printf("ok   %s\n", what);
}

static void expect_int(const char *what, long got, long want) {
    checks++;
    if (got != want) {
        failures++;
        printf("FAIL %s: want %ld, got %ld\n", what, want, got);
        return;
    }
    printf("ok   %s\n", what);
}

static void expect_bytes(const char *what, const uint8_t *got, size_t got_len,
                         const uint8_t *want, size_t want_len) {
    checks++;
    if (got_len != want_len || memcmp(got, want, want_len) != 0) {
        failures++;
        printf("FAIL %s (%zu vs %zu bytes)\n  want:", what, got_len, want_len);
        for (size_t i = 0; i < want_len; i++) {
            printf(" %02X", want[i]);
        }
        printf("\n  got :");
        for (size_t i = 0; i < got_len; i++) {
            printf(" %02X", got[i]);
        }
        printf("\n");
        return;
    }
    printf("ok   %s\n", what);
}

int main(void) {
    uint8_t buf[RLY_MQTT_MAX_PACKET];
    rly_mqtt_packet pkt;
    size_t consumed = 0;

    /* --- CONNECT with a retained will --- */
    rly_mqtt_connect_opts opts = {"rly8", "rly8/status", "offline", 30, NULL, NULL};
    static const uint8_t connect_expected[] = {
        0x10, 0x26, 0x00, 0x04, 'M', 'Q', 'T', 'T', 0x04, 0x26, 0x00, 0x1E,
        0x00, 0x04, 'r', 'l', 'y', '8',
        0x00, 0x0B, 'r', 'l', 'y', '8', '/', 's', 't', 'a', 't', 'u', 's',
        0x00, 0x07, 'o', 'f', 'f', 'l', 'i', 'n', 'e'};
    size_t n = rly_mqtt_build_connect(&opts, buf, sizeof buf);
    expect_bytes("CONNECT with will", buf, n, connect_expected, sizeof connect_expected);
    expect_int("CONNECT flags: clean + will + will retain", buf[9], 0x26);
    expect_int("CONNECT keepalive hi/lo", (buf[10] << 8) | buf[11], 30);

    /* CONNECT without a will drops the will fields from the payload. */
    rly_mqtt_connect_opts no_will = {"rly8", NULL, NULL, 60, NULL, NULL};
    n = rly_mqtt_build_connect(&no_will, buf, sizeof buf);
    expect_int("CONNECT without will length", (long)n, 18);
    expect_int("CONNECT without will flags", buf[9], 0x02);

    /* --- PINGREQ is two bytes --- */
    /* --- CONNECT with username + password (flags 0xC2) --- */
    rly_mqtt_connect_opts auth = {"c", NULL, NULL, 10, "u", "p"};
    static const uint8_t auth_expected[] = {0x10, 0x13, 0x00, 0x04, 'M',  'Q',  'T',
                                            'T',  0x04, 0xC2, 0x00, 0x0A, 0x00, 0x01,
                                            'c',  0x00, 0x01, 'u',  0x00, 0x01, 'p'};
    n = rly_mqtt_build_connect(&auth, buf, sizeof buf);
    expect_bytes("CONNECT user+pass", buf, n, auth_expected, sizeof auth_expected);
    /* password without username is not sent (MQTT 3.1.1 forbids it) */
    rly_mqtt_connect_opts pass_only = {"c", NULL, NULL, 10, "", "p"};
    n = rly_mqtt_build_connect(&pass_only, buf, sizeof buf);
    expect_int("CONNECT pass-only flags", buf[9], 0x02);

    static const uint8_t pingreq_expected[] = {0xC0, 0x00};
    n = rly_mqtt_build_pingreq(buf, sizeof buf);
    expect_bytes("PINGREQ", buf, n, pingreq_expected, sizeof pingreq_expected);

    /* --- PUBLISH round-trip (QoS 0, retained) --- */
    n = rly_mqtt_build_publish("rly8test/relay1/state", (const uint8_t *)"ON", 2, 1, 0, 0, buf,
                               sizeof buf);
    expect_int("PUBLISH length", (long)n, 27);
    expect_int("PUBLISH header is retain+qos0", buf[0], 0x31);
    expect_int("PUBLISH remaining length", buf[1], 25);
    expect_int("parse publish", rly_mqtt_parse(buf, n, &pkt, &consumed), RLY_OK);
    expect_int("publish type", pkt.type, RLY_MQTT_PKT_PUBLISH);
    expect_int("publish retain flag", pkt.flags & 0x01u, 1);
    expect_str("publish topic", pkt.topic, "rly8test/relay1/state");
    expect_int("publish payload len", (long)pkt.payload_len, 2);
    expect_int("publish payload", pkt.payload[0] == 'O' && pkt.payload[1] == 'N', 1);
    expect_int("publish consumed", (long)consumed, 27);

    /* --- PUBLISH QoS 1 keeps the packet id --- */
    n = rly_mqtt_build_publish("rly8test/relay3/set", (const uint8_t *)"OFF", 3, 0, 1, 5, buf,
                               sizeof buf);
    expect_int("parse publish qos1", rly_mqtt_parse(buf, n, &pkt, &consumed), RLY_OK);
    expect_int("qos1 packet id", pkt.packet_id, 5);
    expect_str("qos1 topic", pkt.topic, "rly8test/relay3/set");
    expect_int("qos1 payload len", (long)pkt.payload_len, 3);

    /* --- SUBSCRIBE --- */
    n = rly_mqtt_build_subscribe(7, "rly8test/relay+/set", 0, buf, sizeof buf);
    expect_int("SUBSCRIBE length", (long)n, 26);
    expect_int("SUBSCRIBE header", buf[0], 0x82);
    expect_int("SUBSCRIBE remaining length", buf[1], 24);
    expect_int("parse subscribe", rly_mqtt_parse(buf, n, &pkt, &consumed), RLY_OK);
    expect_int("subscribe type", pkt.type, RLY_MQTT_PKT_SUBSCRIBE);
    expect_int("subscribe consumed", (long)consumed, 26);

    /* --- CONNACK --- */
    static const uint8_t connack[] = {0x20, 0x02, 0x00, 0x00};
    expect_int("parse connack", rly_mqtt_parse(connack, sizeof connack, &pkt, &consumed), RLY_OK);
    expect_int("connack type", pkt.type, RLY_MQTT_PKT_CONNACK);
    expect_int("connack accepted", pkt.payload[1], 0);
    expect_int("connack consumed", (long)consumed, 4);

    /* --- multi-byte remaining length (payload > 127 bytes) --- */
    uint8_t big[300];
    memset(big, 'x', sizeof big);
    n = rly_mqtt_build_publish("rly8test/relay1/state", big, sizeof big, 1, 0, 0, buf, sizeof buf);
    expect_int("large publish built", n > 0, 1);
    expect_int("large publish uses 2-byte varint", buf[1] & 0x80u, 0x80);
    expect_int("parse large publish", rly_mqtt_parse(buf, n, &pkt, &consumed), RLY_OK);
    expect_int("large payload len", (long)pkt.payload_len, (long)sizeof big);
    expect_int("large consumed", (long)consumed, (long)n);

    /* --- rejections --- */
    expect_int("short packet rejected", rly_mqtt_parse(connack, 1, &pkt, &consumed), RLY_ERR_SHORT);
    expect_int("truncated publish rejected", rly_mqtt_parse(buf, n - 1, &pkt, &consumed),
               RLY_ERR_SHORT);
    expect_int("null arg rejected", rly_mqtt_parse(buf, n, NULL, &consumed), RLY_ERR_ARG);
    expect_int("qos1 without packet id refused",
               (long)rly_mqtt_build_publish("t", (const uint8_t *)"x", 1, 0, 1, 0, buf, sizeof buf),
               0);
    expect_int("subscribe without packet id refused",
               (long)rly_mqtt_build_subscribe(0, "a/b", 0, buf, sizeof buf), 0);
    expect_int("publish into tiny buffer refused",
               (long)rly_mqtt_build_publish("rly8test/relay1/state", (const uint8_t *)"ON", 2, 1, 0,
                                            0, buf, 5),
               0);

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
