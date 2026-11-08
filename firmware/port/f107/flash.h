/* STM32F1 internal flash: page erase and half-word programming (RM0008 / PM0075).
 * Shared by the config store, the OTA writer and the bootloader. HSI must be on while
 * programming (it is never switched off by clock.c). */
#ifndef FLASH_H
#define FLASH_H

#include <stdint.h>

#include "../../core/rly_ota.h"

int flash_erase_page(uint32_t addr);
/* addr half-word aligned, len even */
int flash_program(uint32_t addr, const uint8_t *data, uint32_t len);

/* rly_flash_ops bound to the functions above */
extern const rly_flash_ops flash_ops;

#endif /* FLASH_H */
