/* Host-side tests for the MQTT client state machine with a fake byte stream. */
#include <stdio.h>
#include <string.h>

#include "../core/rly_mqtt_client.h"

static int failures;
static int checks;

typedef struct {
    uint8_t rx[1024];
    size_t rx_len;
    size_t rx_pos;
    uint8_t tx[4096];
    size_t tx_len;
    int opens;
    int closes;
} fake_stream;

static int f_open(void *ctx) {
    fake_stream *f = (fake_stream *)ctx;
    f->opens++;
    return 0;
}

static int f_send(void *ctx, const uint8_t *buf, size_t n) {
    fake_stream *f = (fake_stream *)ctx;
    if (f->tx_len + n > sizeof f->tx) {
        return -1;
    }
    memcpy(f->tx + f->tx_len, buf, n);
    f->tx_len += n;
    return 0;
}

static int f_recv(void *ctx, uint8_t *buf, size_t cap) {
    fake_stream *f = (fake_stream *)ctx;
    if (f->rx_pos >= f->rx_len) {
        return 0;
    }
    size_t avail = f->rx_len - f->rx_pos;
    size_t n = avail < cap ? avail : cap;
    memcpy(buf, f->rx + f->rx_pos, n);
    f->rx_pos += n;
    return (int)n;
}

static void f_close(void *ctx) {
    fake_stream *f = (fake_stream *)ctx;
    f->closes++;
}

static void feed(fake_stream *f, const uint8_t *buf, size_t n) {
    memcpy(f->rx + f->rx_len, buf, n);
    f->rx_len += n;
}

/* Binary-safe substring search: MQTT packets contain NUL bytes. */
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

int main(void) {
    fake_stream fake;
    memset(&fake, 0, sizeof fake);

    rly_stream io = {f_open, f_send, f_recv, f_close, &fake};
    rly_ha_config ha;
    rly_ha_defaults(&ha, "rly8test");

    rly_mqtt_client client;
    rly_mc_init(&client, &io, &ha, "rly8", 10);
    expect_int("initial state idle", client.state, RLY_MC_IDLE);

    /* --- start: stream opened, CONNECT with the retained will --- */
    expect_int("start", rly_mc_start(&client, 0), RLY_OK);
    expect_int("stream opened", fake.opens, 1);
    expect_int("state connecting", client.state, RLY_MC_CONNECTING);
    expect_int("CONNECT carries will topic", contains(fake.tx, fake.tx_len, "rly8test/status"), 1);
    expect_int("CONNECT carries will payload", contains(fake.tx, fake.tx_len, "offline"), 1);
    expect_int("CONNECT carries client id", contains(fake.tx, fake.tx_len, "rly8"), 1);

    /* --- CONNACK accepted: subscribe, announce, publish states --- */
    size_t before = fake.tx_len;
    static const uint8_t connack_ok[] = {0x20, 0x02, 0x00, 0x00};
    feed(&fake, connack_ok, sizeof connack_ok);
    expect_int("poll after connack", rly_mc_poll(&client, 5), RLY_OK);
    expect_int("state ready", client.state, RLY_MC_READY);
    expect_int("SUBSCRIBE sent", contains(fake.tx + before, fake.tx_len - before,
                                          "rly8test/relay+/set"), 1);
    expect_int("discovery published for 8 relays", client.session.discovery_published, 8);
    expect_int("availability online sent",
               contains(fake.tx + before, fake.tx_len - before, "online"), 1);

    /* --- inbound relay command --- */
    before = fake.tx_len;
    uint8_t cmd[64];
    size_t n = rly_mqtt_build_publish("rly8test/relay3/set", (const uint8_t *)"ON", 2, 0, 0, 0,
                                      cmd, sizeof cmd);
    feed(&fake, cmd, n);
    expect_int("poll after command", rly_mc_poll(&client, 10), RLY_OK);
    expect_int("relay 3 switched on", (client.session.relay_mask >> 2) & 1, 1);
    expect_int("command counted", (long)client.session.commands_applied, 1);
    expect_int("state echoed", contains(fake.tx + before, fake.tx_len - before,
                                        "rly8test/relay3/state"), 1);

    /* --- keepalive fires once the interval passes --- */
    before = fake.tx_len;
    expect_int("poll before keepalive", rly_mc_poll(&client, 5000), RLY_OK);
    expect_int("no ping yet", fake.tx_len == before, 1);
    expect_int("poll after keepalive", rly_mc_poll(&client, 10u * 1000u + 20u), RLY_OK);
    expect_int("ping sent", fake.tx_len > before, 1);
    expect_int("ping bytes", fake.tx[fake.tx_len - 2] == 0xC0 && fake.tx[fake.tx_len - 1] == 0x00,
               1);

    /* --- broker refusal puts the client into the error state --- */
    fake_stream fake2;
    memset(&fake2, 0, sizeof fake2);
    rly_stream io2 = {f_open, f_send, f_recv, f_close, &fake2};
    rly_mqtt_client client2;
    rly_mc_init(&client2, &io2, &ha, "rly8", 10);
    expect_int("start second client", rly_mc_start(&client2, 0), RLY_OK);
    static const uint8_t connack_bad[] = {0x20, 0x02, 0x00, 0x05};
    feed(&fake2, connack_bad, sizeof connack_bad);
    expect_int("poll refused connack", rly_mc_poll(&client2, 5), RLY_ERR_ARG);
    expect_int("state error", client2.state, RLY_MC_ERROR);
    expect_int("no discovery after refusal", client2.session.discovery_published, 0);

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
