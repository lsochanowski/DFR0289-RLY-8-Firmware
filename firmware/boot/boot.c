/* RLY-8 bootloader (first 8 KB of flash).
 *
 * Runs on the reset clock (HSI 8 MHz), touches nothing but flash and the blue LED:
 *   - pending OTA record + valid staging image  -> copy to the app slot, verify, clear, run
 *   - pending record but corrupt staging         -> clear the record, run the old app
 *   - nothing pending                            -> run the app
 *   - no valid app                               -> fast-blink the LED forever (reflash via SWD)
 * A power cut during the copy keeps the record, so the next boot simply copies again.
 * The decision and copy logic lives in core/rly_ota.c and is unit-tested on the host.
 */
#include <stdint.h>

#include "../core/rly_ota.h"
#include "../port/f107/flash.h"

#define REG(addr) (*(volatile uint32_t *)(addr))
#define RCC_APB2ENR REG(0x40021018u)
#define GPIOB_CRL REG(0x40010C00u)
#define GPIOB_BSRR REG(0x40010C10u)
#define SCB_VTOR REG(0xE000ED08u)

void Boot_Reset(void);
static void halt(void);

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss;

__attribute__((section(".isr_vector"), used))
const uint32_t boot_vectors[16] = {
    [0] = 0x20010000u,
    [1] = (uint32_t)(void *)Boot_Reset,
    [2] = (uint32_t)(void *)halt, /* NMI */
    [3] = (uint32_t)(void *)halt, /* HardFault */
    [4] = (uint32_t)(void *)halt,
    [5] = (uint32_t)(void *)halt,
    [6] = (uint32_t)(void *)halt,
};

/* Blue LED on PB7, active low. */
static void led_init(void) {
    RCC_APB2ENR |= 1u << 3;
    GPIOB_CRL = (GPIOB_CRL & ~(0xFu << 28)) | (0x2u << 28);
}

static void led(int on) { GPIOB_BSRR = on ? (1u << (7u + 16u)) : (1u << 7u); }

static void delay(uint32_t n) {
    for (volatile uint32_t i = 0; i < n; i++) {
    }
}

static void halt(void) {
    led_init();
    for (;;) { /* ~8 Hz blink: no runnable firmware */
        led(1);
        delay(60000u);
        led(0);
        delay(60000u);
    }
}

static void jump_to_app(void) {
    const uint32_t *app = (const uint32_t *)RLY_APP_BASE;
    if (!rly_app_vectors_ok((const uint8_t *)RLY_APP_BASE, RLY_APP_BASE, RLY_SLOT_SIZE)) {
        halt();
    }
    led(0);
    SCB_VTOR = RLY_APP_BASE;
    __asm volatile("msr msp, %0\n"
                   "bx %1\n" ::"r"(app[0]),
                   "r"(app[1]));
    for (;;) {
    }
}

void Boot_Reset(void) {
    uint32_t *src = &_sidata;
    for (uint32_t *dst = &_sdata; dst < &_edata;) {
        *dst++ = *src++;
    }
    for (uint32_t *dst = &_sbss; dst < &_ebss;) {
        *dst++ = 0u;
    }

    switch (rly_boot_decide(&flash_ops)) {
    case RLY_BOOT_INSTALL: {
        led_init();
        led(1); /* steady LED while installing (~3 s) */
        const rly_ota_ctrl *ctrl = (const rly_ota_ctrl *)RLY_OTA_CTRL_ADDR;
        for (int attempt = 0; attempt < 3; attempt++) {
            if (rly_boot_install(&flash_ops, ctrl) == 0) {
                (void)flash_erase_page(RLY_OTA_CTRL_ADDR);
                jump_to_app();
            }
        }
        halt(); /* staging stays valid: the next reset retries */
        break;
    }
    case RLY_BOOT_DISCARD:
        (void)flash_erase_page(RLY_OTA_CTRL_ADDR);
        jump_to_app();
        break;
    case RLY_BOOT_RUN_APP:
        jump_to_app();
        break;
    default:
        halt();
        break;
    }
}
