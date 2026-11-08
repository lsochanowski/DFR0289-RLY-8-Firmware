/* Codec for the RLY-8 (DFRobot DFR0289) binary frame format.
 *
 * Frame: 55 AA | addr | len | cmd | data[len] | crc
 * crc = 8-bit sum of every preceding byte (no weighting, no XOR).
 *
 * Portable C11, no dependencies: shared by the STM32 firmware and the host tests.
 */
#ifndef RLY_FRAME_H
#define RLY_FRAME_H

#include <stddef.h>
#include <stdint.h>

#define RLY_SYNC0 0x55u
#define RLY_SYNC1 0xAAu
#define RLY_MAX_DATA 64u
/* 2 sync + addr + len + cmd + crc */
#define RLY_OVERHEAD 6u
#define RLY_MAX_FRAME (RLY_OVERHEAD + RLY_MAX_DATA)

#define RLY_RELAY_COUNT 8u

/* Command words proven by the datasheet examples. */
#define RLY_CMD_RELAY_SET 0x00u
#define RLY_CMD_RELAY_GET 0x01u
#define RLY_CMD_NET_SET 0x02u
#define RLY_CMD_BAUD_SET 0x08u
#define RLY_CMD_RS485_SET 0x09u

typedef enum {
    RLY_OK = 0,
    RLY_ERR_ARG = -1,
    RLY_ERR_SYNC = -2,
    RLY_ERR_SHORT = -3,
    RLY_ERR_LEN = -4,
    RLY_ERR_CRC = -5
} rly_status;

typedef struct {
    uint8_t addr;
    uint8_t cmd;
    uint8_t len;
    uint8_t data[RLY_MAX_DATA];
} rly_frame;

typedef struct {
    uint8_t ip[4];
    uint8_t gateway[4];
    uint8_t netmask[4];
    uint16_t port;
} rly_netconfig;

uint8_t rly_crc(const uint8_t *buf, size_t n);

/* Writes the whole frame into out; returns its length or 0 if it does not fit. */
size_t rly_encode(const rly_frame *frame, uint8_t *out, size_t cap);

/* Parses exactly len bytes; returns RLY_OK, or a negative rly_status. */
rly_status rly_decode(const uint8_t *in, size_t n, rly_frame *out);

/* True when n holds at least one complete frame; consumed gets its length. */
int rly_frame_size(const uint8_t *in, size_t n, size_t *consumed);

rly_status rly_relay_set(uint8_t addr, uint8_t relay, int closed, rly_frame *out);
rly_status rly_relay_get(uint8_t addr, rly_frame *out);
rly_status rly_net_set(uint8_t addr, const rly_netconfig *cfg, rly_frame *out);
rly_status rly_baud_set(uint8_t addr, uint32_t baud, rly_frame *out);
rly_status rly_rs485_set(uint8_t addr, uint8_t new_addr, rly_frame *out);

/* Relay bitmap from a RLY_CMD_RELAY_GET response: bit0 = relay 1. */
rly_status rly_relay_parse_mask(const rly_frame *response, uint8_t *mask);

#endif /* RLY_FRAME_H */
