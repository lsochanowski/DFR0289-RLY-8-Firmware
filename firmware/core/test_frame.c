/* Host-side tests for the frame codec. Vectors come from the DFR0289 datasheet. */
#include <stdio.h>
#include <string.h>

#include "../core/rly_frame.h"

static int failures;
static int checks;

static void expect_bytes(const char *what, const uint8_t *got, size_t got_len,
                         const uint8_t *want, size_t want_len) {
    checks++;
    if (got_len != want_len || memcmp(got, want, want_len) != 0) {
        failures++;
        printf("FAIL %s\n  want:", what);
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

static void expect_int(const char *what, long got, long want) {
    checks++;
    if (got != want) {
        failures++;
        printf("FAIL %s: want %ld, got %ld\n", what, want, got);
        return;
    }
    printf("ok   %s\n", what);
}

static size_t encode(const rly_frame *frame, uint8_t *out) {
    size_t n = rly_encode(frame, out, RLY_MAX_FRAME);
    if (n == 0) {
        printf("FAIL encode returned 0\n");
        failures++;
    }
    return n;
}

int main(void) {
    uint8_t buf[RLY_MAX_FRAME];
    rly_frame frame;

    /* 1.3 relay 1 off -> 55 AA 01 02 00 01 01 04 */
    const uint8_t relay_off[] = {0x55, 0xAA, 0x01, 0x02, 0x00, 0x01, 0x01, 0x04};
    expect_int("relay_set arg", rly_relay_set(0x01, 1, 0, &frame), RLY_OK);
    expect_bytes("relay 1 off", buf, encode(&frame, buf), relay_off, sizeof relay_off);

    /* 1.3 relay 1 on -> 55 AA 01 02 00 01 00 03 */
    const uint8_t relay_on[] = {0x55, 0xAA, 0x01, 0x02, 0x00, 0x01, 0x00, 0x03};
    expect_int("relay_set arg (on)", rly_relay_set(0x01, 1, 1, &frame), RLY_OK);
    expect_bytes("relay 1 on", buf, encode(&frame, buf), relay_on, sizeof relay_on);

    /* 1.4 get relay state -> 55 AA 01 00 01 01 */
    const uint8_t relay_get[] = {0x55, 0xAA, 0x01, 0x00, 0x01, 0x01};
    expect_int("relay_get", rly_relay_get(0x01, &frame), RLY_OK);
    expect_bytes("get relay state", buf, encode(&frame, buf), relay_get, sizeof relay_get);

    /* 1.5 set network -> 192.168.1.10 / 192.168.1.1 / 255.255.255.0 / 2000 */
    const uint8_t net_set[] = {0x55, 0xAA, 0x01, 0x0E, 0x02, 0xC0, 0xA8, 0x01, 0x0A, 0xC0,
                               0xA8, 0x01, 0x01, 0xFF, 0xFF, 0xFF, 0x00, 0x07, 0xD0, 0xC1};
    rly_netconfig cfg = {{192, 168, 1, 10}, {192, 168, 1, 1}, {255, 255, 255, 0}, 2000};
    expect_int("net_set", rly_net_set(0x01, &cfg, &frame), RLY_OK);
    expect_bytes("set network info", buf, encode(&frame, buf), net_set, sizeof net_set);

    /* 1.10 set baudrate 115200 -> 55 AA 01 04 08 00 01 C2 00 CF */
    const uint8_t baud_set[] = {0x55, 0xAA, 0x01, 0x04, 0x08, 0x00, 0x01, 0xC2, 0x00, 0xCF};
    expect_int("baud_set", rly_baud_set(0x01, 115200, &frame), RLY_OK);
    expect_bytes("set baudrate 115200", buf, encode(&frame, buf), baud_set, sizeof baud_set);

    /* 1.11 set RS485 address 1 -> 2 -> 55 AA 01 01 09 02 0C */
    const uint8_t rs485_set[] = {0x55, 0xAA, 0x01, 0x01, 0x09, 0x02, 0x0C};
    expect_int("rs485_set", rly_rs485_set(0x01, 0x02, &frame), RLY_OK);
    expect_bytes("set RS485 address", buf, encode(&frame, buf), rs485_set, sizeof rs485_set);

    /* Decode keeps every field of the network frame. */
    expect_int("decode net frame", rly_decode(net_set, sizeof net_set, &frame), RLY_OK);
    expect_int("decoded addr", frame.addr, 0x01);
    expect_int("decoded cmd", frame.cmd, RLY_CMD_NET_SET);
    expect_int("decoded len", frame.len, 14);
    expect_int("decoded port hi", frame.data[12], 0x07);
    expect_int("decoded port lo", frame.data[13], 0xD0);

    /* Rejections. */
    uint8_t bad[sizeof net_set];
    memcpy(bad, net_set, sizeof bad);
    bad[sizeof bad - 1] ^= 0xFFu;
    expect_int("bad crc rejected", rly_decode(bad, sizeof bad, &frame), RLY_ERR_CRC);

    memcpy(bad, net_set, sizeof bad);
    bad[0] = 0x56;
    expect_int("bad sync rejected", rly_decode(bad, sizeof bad, &frame), RLY_ERR_SYNC);

    expect_int("short frame rejected", rly_decode(net_set, 5, &frame), RLY_ERR_SHORT);
    expect_int("null arg rejected", rly_decode(net_set, sizeof net_set, NULL), RLY_ERR_ARG);

    /* Frame boundary detection on a stream with two frames back to back. */
    uint8_t stream[32];
    memcpy(stream, relay_get, sizeof relay_get);
    memcpy(stream + sizeof relay_get, relay_off, sizeof relay_off);
    size_t consumed = 0;
    expect_int("stream frame 1", rly_frame_size(stream, sizeof relay_get, &consumed), 1);
    expect_int("stream frame 1 len", (long)consumed, 6);
    expect_int("stream frame 2", rly_frame_size(stream + consumed, sizeof relay_off, &consumed),
               1);
    expect_int("stream frame 2 len", (long)consumed, 8);

    /* Relay bitmap response. */
    rly_frame response = {0};
    response.addr = 0x01;
    response.cmd = RLY_CMD_RELAY_GET;
    response.len = 1;
    response.data[0] = 0x07;
    uint8_t mask = 0;
    expect_int("relay mask parse", rly_relay_parse_mask(&response, &mask), RLY_OK);
    expect_int("relay mask value", mask, 0x07);

    /* Out-of-range relay and undersized buffer. */
    expect_int("relay 0 rejected", rly_relay_set(0x01, 0, 1, &frame), RLY_ERR_ARG);
    expect_int("relay 9 rejected", rly_relay_set(0x01, 9, 1, &frame), RLY_ERR_ARG);
    rly_relay_set(0x01, 1, 1, &frame);
    expect_int("small buffer returns 0", (long)rly_encode(&frame, buf, 4), 0);

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
