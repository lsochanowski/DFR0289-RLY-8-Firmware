/* Drive every GPIO pin on the board except the known relay pins and the SWD pair.
 *
 * Purpose: with all relays mapped, switch on everything else at once to see what the
 * unknown pins actually control (LEDs, RS485 direction, PHY reset, buzzer, ...).
 *
 * Excluded pins:
 *   PA13, PA14  SWDIO/SWCLK — driving them kills the debugger
 *   PB15        relay 8
 *   PC6..PC12   relays 7,6,1,2,3,4,5
 *
 * Everything else is driven HIGH and left driven. Masks per port are reported in RAM so the
 * result can be cross-checked over SWD: r[1..5] = masks for ports A..E, r[6] = "DONE".
 */
#include <stdint.h>

#define REG(addr) (*(volatile uint32_t *)(addr))

#define RCC_CR REG(0x40021000u)
#define RCC_CFGR REG(0x40021004u)
#define RCC_APB2ENR REG(0x40021018u)
#define FLASH_ACR REG(0x40022000u)

#define MODE_OUT_PP_2MHZ 0x2u

/* Polarity of the driven level: default HIGH, -DDRIVE_LOW=1 drives every pin LOW instead. */
#ifndef DRIVE_LOW
#define DRIVE_LOW 0
#endif

/* Port A: drop PA13/PA14. Port B: drop PB15. Port C: drop PC6..PC12. */
#define MASK_A 0x9FFFu
#define MASK_B 0x7FFFu
#define MASK_C 0xE03Fu
#define MASK_D 0xFFFFu
#define MASK_E 0xFFFFu

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss;

void Reset_Handler(void);
int main(void);

__attribute__((section(".isr_vector"), used))
const uint32_t vectors[16] = {
    0x20010000u,
    (uint32_t)(void *)Reset_Handler,
};

void Reset_Handler(void) {
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

static void clock_to_72mhz(void) {
    FLASH_ACR = 0x12u;
    RCC_CR |= (1u << 16);
    for (volatile uint32_t spin = 0; spin < 200000u; spin++) {
        if (RCC_CR & (1u << 17)) {
            break;
        }
    }
    uint32_t cfgr = RCC_CFGR;
    cfgr &= ~((0xFu << 18) | (0x7u << 11) | (0x7u << 8) | (0xFu << 4) | (1u << 17) | 0x3u);
    cfgr |= (0x7u << 18) | (1u << 16) | (0x4u << 8);
    RCC_CFGR = cfgr;
    RCC_CR |= (1u << 24);
    for (volatile uint32_t spin = 0; spin < 200000u; spin++) {
        if (RCC_CR & (1u << 25)) {
            break;
        }
    }
    uint32_t sw = RCC_CFGR;
    sw = (sw & ~0x3u) | 0x2u;
    RCC_CFGR = sw;
    for (volatile uint32_t spin = 0; spin < 200000u; spin++) {
        if ((RCC_CFGR & 0xCu) == 0x8u) {
            break;
        }
    }
}

static void drive_port(uint32_t base, uint32_t mask) {
    volatile uint32_t *odr = (volatile uint32_t *)(uintptr_t)(base + 0x0Cu);
    volatile uint32_t *crl = (volatile uint32_t *)(uintptr_t)(base + 0x00u);
    volatile uint32_t *crh = (volatile uint32_t *)(uintptr_t)(base + 0x04u);

    /* Latch the level before switching the pin to an output: one clean edge, no glitch. */
#if DRIVE_LOW
    *odr &= ~mask;
#else
    *odr |= mask;
#endif

    uint32_t crl_value = *crl;
    uint32_t crh_value = *crh;
    for (uint32_t pin = 0; pin < 16u; pin++) {
        if ((mask & (1u << pin)) == 0u) {
            continue;
        }
        uint32_t shift = (pin & 7u) * 4u;
        if (pin < 8u) {
            crl_value &= ~(0xFu << shift);
            crl_value |= (MODE_OUT_PP_2MHZ << shift);
        } else {
            crh_value &= ~(0xFu << shift);
            crh_value |= (MODE_OUT_PP_2MHZ << shift);
        }
    }
    *crl = crl_value;
    *crh = crh_value;
}

int main(void) {
    volatile uint32_t *r = (volatile uint32_t *)0x20008000u;
    r[0] = 0x524C5931u;

    clock_to_72mhz();
    RCC_APB2ENR |= (0x1Fu << 2); /* IOPA..IOPE */

    r[1] = MASK_A;
    r[2] = MASK_B;
    r[3] = MASK_C;
    r[4] = MASK_D;
    r[5] = MASK_E;

    drive_port(0x40010800u, MASK_A);
    drive_port(0x40010C00u, MASK_B);
    drive_port(0x40011000u, MASK_C);
    drive_port(0x40011400u, MASK_D);
    drive_port(0x40011800u, MASK_E);

    r[6] = 0x4E4F5F41u; /* "A_ON" — everything is driven and stays driven */
    for (;;) {
    }
}
