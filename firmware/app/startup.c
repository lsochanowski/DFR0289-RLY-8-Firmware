/* Startup for the RLY-8 application firmware (STM32F107VC).
 *
 * The vector table uses designated initializers on purpose: a plain list silently shifts
 * every handler when one entry is added or removed, which is exactly how SysTick ended up on
 * the PendSV slot once (and then jumped to address 0 → INVSTATE → HardFault).
 */
#include <stdint.h>

#include "../core/rly_ota.h"
#include "../core/rly_version.h"

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss;
extern uint32_t _image_end;

void Reset_Handler(void);
void Default_Handler(void);
void SysTick_Handler(void);
/* Weak: links to Default_Handler when port/f107/usb_cdc.c is not part of the image. */
void OTG_FS_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
int main(void);

__attribute__((section(".isr_vector"), used))
const uint32_t vectors[16 + 68] = {
    [0] = 0x20010000u, /* initial stack pointer, top of 64 KB RAM */
    [1] = (uint32_t)(void *)Reset_Handler,
    [2] = (uint32_t)(void *)Default_Handler,  /* NMI */
    [3] = (uint32_t)(void *)Default_Handler,  /* HardFault */
    [4] = (uint32_t)(void *)Default_Handler,  /* MemManage */
    [5] = (uint32_t)(void *)Default_Handler,  /* BusFault */
    [6] = (uint32_t)(void *)Default_Handler,  /* UsageFault */
    [7] = 0u,                                 /* reserved */
    [8] = 0u,                                 /* reserved */
    [9] = 0u,                                 /* reserved */
    [10] = 0u,                                /* reserved */
    [11] = (uint32_t)(void *)Default_Handler, /* SVCall */
    [12] = (uint32_t)(void *)Default_Handler, /* DebugMon */
    [13] = 0u,                                /* reserved */
    [14] = (uint32_t)(void *)Default_Handler, /* PendSV */
    [15] = (uint32_t)(void *)SysTick_Handler, /* SysTick: lwIP time base */
    /* External interrupts (connectivity line, RM0008 Table 61). Only enabled IRQs can fire;
     * everything else parks in Default_Handler. */
    [16 ... 82] = (uint32_t)(void *)Default_Handler,
    [83] = (uint32_t)(void *)OTG_FS_IRQHandler, /* IRQ 67: USB OTG FS */
};

void Default_Handler(void) {
    for (;;) {
    }
}

/* Image header checked by the web-panel updater and by the bootloader (core/rly_ota.h). */
__attribute__((section(".app_info"), used))
const rly_app_info app_info = {
    .magic = RLY_APP_INFO_MAGIC,
    .info_version = RLY_APP_INFO_VERSION,
    .image_end = (uint32_t)&_image_end,
    .reserved = 0u,
    .fw_version = RLY_FW_VERSION,
};

void Reset_Handler(void) {
    /* The bootloader jumps here with VTOR still pointing at its own table. */
    *(volatile uint32_t *)0xE000ED08u = (uint32_t)vectors; /* SCB_VTOR */
    uint32_t *src = &_sidata;
    uint32_t *dst = &_sdata;
    while (dst < &_edata) {
        *dst++ = *src++;
    }
    for (dst = &_sbss; dst < &_ebss;) {
        *dst++ = 0;
    }
    (void)main();
    for (;;) {
    }
}
