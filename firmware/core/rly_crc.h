/* CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320) — config pages and firmware images. */
#ifndef RLY_CRC_H
#define RLY_CRC_H

#include <stddef.h>
#include <stdint.h>

#define RLY_CRC32_INIT 0xFFFFFFFFu

/* Incremental form: state = RLY_CRC32_INIT; state = rly_crc32_update(state, ...);
 * result = ~state. */
uint32_t rly_crc32_update(uint32_t state, const void *data, size_t len);
uint32_t rly_crc32(const void *data, size_t len);

#endif /* RLY_CRC_H */
