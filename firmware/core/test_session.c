/* Host-side tests for the MQTT session state machine with a fake transport. */
#include <stdio.h>
#include <string.h>

#include "../core/rly_session.h"

static int failures;
static int checks;

/* Fake transport: every sent byte is captured and parsed back as MQTT packets. */
typedef struct {
    uint8_t stream[16 * 1024];
    size_t len;
    int sent_packets;
    int discovery_packets;
    int state_packets;
    int availability_online;
    char last_topic[RLY_MQTT_TOPIC_MAX];
    char last_payload[64];
} fake_bus;

static int fake_send(void *ctx, const uint8_t *buf, size_t len) {
    fake_bus *bus = (fake_bus *)ctx;
    if (bus->len + len > sizeof bus->stream) {
        return -1;
    }
    memcpy(bus->stream + bus->len, buf, len);
    bus->len += len;
    bus->sent_packets++;
    return (int)len;
}

static int fake_recv(void *ctx, uint8_t *buf, size_t cap) {
    (void)ctx;
    (void)buf;
    (void)cap;
    return 0;
}

/* Walks everything the session published and classifies it. */
static void analyse(fake_bus *bus) {
    size_t off = 0;
    while (off < bus->len) {
        rly_mqtt_packet pkt;
        size_t consumed = 0;
        if (rly_mqtt_parse(bus->stream + off, bus->len - off, &pkt, &consumed) != RLY_OK) {
            return;
        }
        if (pkt.type == RLY_MQTT_PKT_PUBLISH) {
            size_t n = pkt.payload_len < sizeof bus->last_payload - 1 ? pkt.payload_len
                                                                     : sizeof bus->last_payload - 1;
            memcpy(bus->last_payload, pkt.payload, n);
            bus->last_payload[n] = '\0';
            snprintf(bus->last_topic, sizeof bus->last_topic, "%s", pkt.topic);
            if (strstr(pkt.topic, "/config") != NULL && (pkt.flags & 0x01u) != 0) {
                bus->discovery_packets++;
            } else if (strstr(pkt.topic, "/state") != NULL) {
                bus->state_packets++;
            } else if (strstr(pkt.topic, "/status") != NULL &&
                       strcmp(bus->last_payload, "online") == 0) {
                bus->availability_online++;
            }
        }
        off += consumed;
    }
}

/* MQTT packets are binary: a NUL inside them must not stop the search, so no strstr. */
static int contains(const uint8_t *hay, size_t hay_len, const char *needle) {
    size_t n = strlen(needle);
    if (n == 0 || hay_len < n) {
        return 0;
    }
    for (size_t i = 0; i + n <= hay_len; i++) {
        if (memcmp(hay + i, needle, n) == 0) {
            return 1;
        }
    }
    return 0;
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

static void expect_str(const char *what, const char *got, const char *want) {
    checks++;
    if (got == NULL || strcmp(got, want) != 0) {
        failures++;
        printf("FAIL %s\n  want: %s\n  got : %s\n", what, want, got ? got : "(null)");
        return;
    }
    printf("ok   %s\n", what);
}

int main(void) {
    fake_bus bus;
    memset(&bus, 0, sizeof bus);

    rly_transport io = {fake_send, fake_recv, &bus};
    rly_ha_config ha;
    rly_ha_defaults(&ha, "rly8test");

    rly_session session;
    rly_session_init(&session, &io, &ha, "rly8", 30);
    expect_str("client id", session.client_id, "rly8");
    expect_int("initial mask", session.relay_mask, 0);
    expect_int("not connected yet", session.connected, 0);

    /* --- CONNECT carries the retained availability will --- */
    expect_int("session connect", rly_session_connect(&session), RLY_OK);
    expect_int("one packet sent", bus.sent_packets, 1);
    expect_int("CONNECT type byte", bus.stream[0], 0x10);
    checks++;
    if (!contains(bus.stream, bus.len, "rly8test/status") ||
        !contains(bus.stream, bus.len, "offline")) {
        failures++;
        printf("FAIL CONNECT will topic/payload missing\n");
    } else {
        printf("ok   CONNECT carries will rly8test/status=offline\n");
    }
    expect_int("will retained flag", bus.stream[9] & 0x20u, 0x20);

    /* --- CONNACK triggers discovery for all 8 relays + states + online --- */
    memset(&bus, 0, sizeof bus);
    static const uint8_t connack[] = {0x20, 0x02, 0x00, 0x00};
    rly_mqtt_packet pkt;
    size_t consumed = 0;
    expect_int("parse connack", rly_mqtt_parse(connack, sizeof connack, &pkt, &consumed), RLY_OK);
    expect_int("handle connack", rly_session_on_packet(&session, &pkt), RLY_OK);
    expect_int("connected", session.connected, 1);
    expect_int("8 discovery configs", session.discovery_published, 8);
    expect_int("8 state messages", session.state_published, 8);
    analyse(&bus);
    expect_int("captured discovery packets", bus.discovery_packets, 8);
    expect_int("captured state packets", bus.state_packets, 8);
    expect_int("captured availability online", bus.availability_online, 1);

    /* --- inbound relay command from Home Assistant --- */
    memset(&bus, 0, sizeof bus);
    unsigned before = session.commands_applied;
    uint8_t cmd[64];
    size_t n = rly_mqtt_build_publish("rly8test/relay3/set", (const uint8_t *)"ON", 2, 0, 0, 0,
                                      cmd, sizeof cmd);
    expect_int("build inbound publish", n > 0, 1);
    expect_int("parse inbound publish", rly_mqtt_parse(cmd, n, &pkt, &consumed), RLY_OK);
    expect_int("handle relay3 ON", rly_session_on_packet(&session, &pkt), RLY_OK);
    expect_int("mask has relay3", (session.relay_mask >> 2) & 1, 1);
    expect_int("command counted", session.commands_applied - before, 1);
    expect_int("state published for relay3", bus.sent_packets, 1);
    analyse(&bus);
    expect_str("state topic", bus.last_topic, "rly8test/relay3/state");
    expect_str("state payload", bus.last_payload, "ON");

    /* relay OFF on the same relay clears the bit */
    n = rly_mqtt_build_publish("rly8test/relay3/set", (const uint8_t *)"OFF", 3, 0, 0, 0, cmd,
                               sizeof cmd);
    rly_mqtt_parse(cmd, n, &pkt, &consumed);
    expect_int("handle relay3 OFF", rly_session_on_packet(&session, &pkt), RLY_OK);
    expect_int("mask cleared relay3", (session.relay_mask >> 2) & 1, 0);

    /* --- foreign topic and bogus payload are ignored --- */
    memset(&bus, 0, sizeof bus);
    before = session.commands_applied;
    n = rly_mqtt_build_publish("other/relay1/set", (const uint8_t *)"ON", 2, 0, 0, 0, cmd,
                               sizeof cmd);
    rly_mqtt_parse(cmd, n, &pkt, &consumed);
    expect_int("foreign topic ignored", rly_session_on_packet(&session, &pkt), RLY_OK);
    n = rly_mqtt_build_publish("rly8test/relay2/set", (const uint8_t *)"maybe", 5, 0, 0, 0, cmd,
                               sizeof cmd);
    rly_mqtt_parse(cmd, n, &pkt, &consumed);
    expect_int("bogus payload ignored", rly_session_on_packet(&session, &pkt), RLY_OK);
    expect_int("nothing applied", session.commands_applied, (long)before);
    expect_int("nothing published", bus.sent_packets, 0);

    /* --- refused CONNACK --- */
    static const uint8_t connack_bad[] = {0x20, 0x02, 0x00, 0x05};
    rly_mqtt_parse(connack_bad, sizeof connack_bad, &pkt, &consumed);
    session.connected = 1;
    expect_int("refused connack rejected", rly_session_on_packet(&session, &pkt), RLY_ERR_ARG);
    expect_int("session marked offline", session.connected, 0);

    /* --- direct API edges --- */
    expect_int("set_relay 0 rejected", rly_session_set_relay(&session, 0, 1), RLY_ERR_ARG);
    expect_int("set_relay 9 rejected", rly_session_set_relay(&session, 9, 1), RLY_ERR_ARG);
    expect_int("null session rejected", rly_session_on_packet(NULL, &pkt), RLY_ERR_ARG);

    /* --- discovery payload content for one relay --- */
    memset(&bus, 0, sizeof bus);
    expect_int("announce again", rly_session_announce(&session), RLY_OK);
    checks++;
    if (contains(bus.stream, bus.len, "homeassistant/switch/rly8test/relay1/config")) {
        printf("ok   discovery topic on the wire\n");
    } else {
        failures++;
        printf("FAIL discovery topic missing on the wire\n");
    }
    checks++;
    if (contains(bus.stream, bus.len, "rly8test_relay8")) {
        printf("ok   discovery mentions HA unique id\n");
    } else {
        failures++;
        printf("FAIL discovery unique id missing\n");
    }

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
