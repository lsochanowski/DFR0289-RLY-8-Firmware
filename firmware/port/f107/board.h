/* STM32F107RC board layer for the RLY-8 replacement firmware.
 *
 * Values here are measured on the actual board over SWD (see docs/hardware-findings.md),
 * not copied from a reference design:
 *   - HSE = 25 MHz oscillator (measured with DWT_CYCCNT), shared with the PHY via MCO/PA8
 *   - 72 MHz via PLL2: 25/5*8/5*9 (the old "8 MHz x9" setting actually ran at ~139 MHz)
 *   - 256 KB flash / 64 KB RAM, RDP level 0
 *   - PHY is a TI DP83848AA
 */
#ifndef BOARD_H
#define BOARD_H

#include <stdint.h>

#define BOARD_MCU_NAME "STM32F107VC"
#define BOARD_FLASH_BYTES (256u * 1024u)
#define BOARD_RAM_BYTES (64u * 1024u)

#define BOARD_HSE_HZ 25000000u
#define BOARD_SYSCLK_HZ 72000000u
#define BOARD_APB1_HZ 36000000u

/* GPIO port bases (RM0008). */
#define GPIOA_BASE 0x40010800u
#define GPIOB_BASE 0x40010C00u
#define GPIOC_BASE 0x40011000u
#define GPIOD_BASE 0x40011400u
#define GPIOE_BASE 0x40011800u

/* Pins reserved by the debug interface: driving them kills the debugger, so the
 * identification probes and the relay driver must both skip them. */
#define BOARD_PIN_SWDIO 13u /* PA13 */
#define BOARD_PIN_SWCLK 14u /* PA14 */

#define BOARD_RELAY_COUNT 8u

#endif /* BOARD_H */
