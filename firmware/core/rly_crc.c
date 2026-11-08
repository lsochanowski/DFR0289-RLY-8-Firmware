#include "rly_crc.h"

uint32_t rly_crc32_update(uint32_t crc, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return crc;
}

uint32_t rly_crc32(const void *data, size_t len) {
    return ~rly_crc32_update(RLY_CRC32_INIT, data, len);
}
