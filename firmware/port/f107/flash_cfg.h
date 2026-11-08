/* Configuration storage in the last two 2 KB flash pages (A/B, newest valid copy wins).
 *
 * 0x0803F000 and 0x0803F800 are reserved in the linker script (FLASH LENGTH = 252K), so
 * a growing image can never overwrite them. A save always goes to the page that does NOT
 * hold the current copy, so a power cut mid-write leaves the previous config intact.
 */
#ifndef FLASH_CFG_H
#define FLASH_CFG_H

#include "../../core/rly_config.h"

/* Loads the newest valid copy; returns 1 when found, 0 when defaults were used. */
int flash_cfg_load(rly_config *cfg);
/* Seals and writes cfg; returns 0 on success. */
int flash_cfg_save(rly_config *cfg);
/* Erases both pages (factory reset). */
void flash_cfg_erase(void);

#endif /* FLASH_CFG_H */
