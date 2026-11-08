#include "rly_frame.h"

#include <string.h>

uint8_t rly_crc(const uint8_t *buf, size_t n) {
    uint8_t sum = 0;
    if (buf == NULL) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        sum = (uint8_t)(sum + buf[i]);
    }
    return sum;
}

size_t rly_encode(const rly_frame *frame, uint8_t *out, size_t cap) {
    if (frame == NULL || out == NULL || frame->len > RLY_MAX_DATA) {
        return 0;
    }
    size_t total = RLY_OVERHEAD + (size_t)frame->len;
    if (total > cap) {
        return 0;
    }
    out[0] = RLY_SYNC0;
    out[1] = RLY_SYNC1;
    out[2] = frame->addr;
    out[3] = frame->len;
    out[4] = frame->cmd;
    if (frame->len > 0) {
        memcpy(&out[5], frame->data, frame->len);
    }
    out[total - 1] = rly_crc(out, total - 1);
    return total;
}

rly_status rly_decode(const uint8_t *in, size_t n, rly_frame *out) {
    if (in == NULL || out == NULL) {
        return RLY_ERR_ARG;
    }
    if (n < RLY_OVERHEAD) {
        return RLY_ERR_SHORT;
    }
    if (in[0] != RLY_SYNC0 || in[1] != RLY_SYNC1) {
        return RLY_ERR_SYNC;
    }
    uint8_t len = in[3];
    if (len > RLY_MAX_DATA) {
        return RLY_ERR_LEN;
    }
    size_t total = RLY_OVERHEAD + (size_t)len;
    if (n < total) {
        return RLY_ERR_SHORT;
    }
    if (rly_crc(in, total - 1) != in[total - 1]) {
        return RLY_ERR_CRC;
    }
    out->addr = in[2];
    out->len = len;
    out->cmd = in[4];
    if (len > 0) {
        memcpy(out->data, &in[5], len);
    }
    return RLY_OK;
}

int rly_frame_size(const uint8_t *in, size_t n, size_t *consumed) {
    if (in == NULL || consumed == NULL) {
        return 0;
    }
    if (n < 5) {
        return 0;
    }
    if (in[0] != RLY_SYNC0 || in[1] != RLY_SYNC1) {
        return 0;
    }
    size_t total = RLY_OVERHEAD + (size_t)in[3];
    if (in[3] > RLY_MAX_DATA || n < total) {
        return 0;
    }
    *consumed = total;
    return 1;
}

rly_status rly_relay_set(uint8_t addr, uint8_t relay, int closed, rly_frame *out) {
    if (out == NULL || relay < 1 || relay > RLY_RELAY_COUNT) {
        return RLY_ERR_ARG;
    }
    out->addr = addr;
    out->cmd = RLY_CMD_RELAY_SET;
    out->len = 2;
    out->data[0] = relay;
    /* Datasheet: 0x01 with the example labelled "off", 0x00 with "on". */
    out->data[1] = closed ? 0x00u : 0x01u;
    return RLY_OK;
}

rly_status rly_relay_get(uint8_t addr, rly_frame *out) {
    if (out == NULL) {
        return RLY_ERR_ARG;
    }
    out->addr = addr;
    out->cmd = RLY_CMD_RELAY_GET;
    out->len = 0;
    return RLY_OK;
}

rly_status rly_net_set(uint8_t addr, const rly_netconfig *cfg, rly_frame *out) {
    if (cfg == NULL || out == NULL) {
        return RLY_ERR_ARG;
    }
    out->addr = addr;
    out->cmd = RLY_CMD_NET_SET;
    out->len = 14;
    memcpy(&out->data[0], cfg->ip, 4);
    memcpy(&out->data[4], cfg->gateway, 4);
    memcpy(&out->data[8], cfg->netmask, 4);
    out->data[12] = (uint8_t)(cfg->port >> 8);
    out->data[13] = (uint8_t)(cfg->port & 0xFFu);
    return RLY_OK;
}

rly_status rly_baud_set(uint8_t addr, uint32_t baud, rly_frame *out) {
    if (out == NULL) {
        return RLY_ERR_ARG;
    }
    out->addr = addr;
    out->cmd = RLY_CMD_BAUD_SET;
    out->len = 4;
    out->data[0] = (uint8_t)(baud >> 24);
    out->data[1] = (uint8_t)(baud >> 16);
    out->data[2] = (uint8_t)(baud >> 8);
    out->data[3] = (uint8_t)baud;
    return RLY_OK;
}

rly_status rly_rs485_set(uint8_t addr, uint8_t new_addr, rly_frame *out) {
    if (out == NULL) {
        return RLY_ERR_ARG;
    }
    out->addr = addr;
    out->cmd = RLY_CMD_RS485_SET;
    out->len = 1;
    out->data[0] = new_addr;
    return RLY_OK;
}

rly_status rly_relay_parse_mask(const rly_frame *response, uint8_t *mask) {
    if (response == NULL || mask == NULL) {
        return RLY_ERR_ARG;
    }
    if (response->len != 1) {
        return RLY_ERR_LEN;
    }
    *mask = response->data[0];
    return RLY_OK;
}
