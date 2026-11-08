/* Relay identification by holding each pin, one at a time — no click counting.
 *
 * Methodology change: the earlier "pin N clicks N+1 times" scheme became unusable past
 * ~10 clicks, and the polarity flipped between runs. This probe simply drives one pin at
 * a time, holds it long enough to see, and moves on in a fixed order, so the observer only
 * has to report WHICH relay closed (or that nothing did).
 *
 * Per pin: 1200 ms active, 800 ms inactive, 500 ms gap.
 * Build: -DPORT_INDEX=n -DSTART_PIN=a -DEND_PIN=b [-DACTIVE_LOW=1]
 * PA13/PA14 (SWDIO/SWCLK) are never driven. Progress is mirrored to 0x20008000.
 */
#include <stdint.h>

#define REG(addr) (*(volatile uint32_t *)(addr))

#ifndef PORT_INDEX
#define PORT_INDEX 0
#endif
#ifndef START_PIN
#define START_PIN 0
#endif
#ifndef END_PIN
#define END_PIN 15
#endif
#ifndef ACTIVE_LOW
#define ACTIVE_LOW 0
#endif

#define RCC_CR REG(0x40021000u)
#define RCC_CFGR REG(0x40021004u)
#define RCC_APB2ENR REG(0x40021018u)
#define FLASH_ACR REG(0x40022000u)

#define MODE_FLOAT_INPUT 0x4u
#define MODE_OUT_PP_2MHZ 0x2u

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

static void delay_ms(uint32_t ms) {
    for (uint32_t i = 0; i < ms; i++) {
        for (volatile uint32_t j = 0; j < 7000u; j++) {
        }
    }
}

static uint32_t port_base(uint32_t index) {
    switch (index) {
    case 0:
        return 0x40010800u; /* A */
    case 1:
        return 0x40010C00u; /* B */
    case 2:
        return 0x40011000u; /* C */
    case 3:
        return 0x40011400u; /* D */
    default:
        return 0x40011800u; /* E */
    }
}

static void set_mode(uint32_t base, uint32_t pin, uint32_t mode) {
    volatile uint32_t *cr = (volatile uint32_t *)(uintptr_t)(base + (pin < 8u ? 0x00u : 0x04u));
    uint32_t shift = (pin & 7u) * 4u;
    uint32_t value = *cr;
    value &= ~(0xFu << shift);
    value |= (mode & 0xFu) << shift;
    *cr = value;
}

static void set_pin(uint32_t base, uint32_t pin, int high) {
    volatile uint32_t *odr = (volatile uint32_t *)(uintptr_t)(base + 0x0Cu);
    if (high) {
        *odr |= (1u << pin);
    } else {
        *odr &= ~(1u << pin);
    }
}

int main(void) {
    volatile uint32_t *r = (volatile uint32_t *)0x20008000u;
    r[0] = 0x524C5931u;
    r[1] = PORT_INDEX;
    r[2] = START_PIN;
    r[3] = END_PIN;
    r[4] = ACTIVE_LOW;

    clock_to_72mhz();
    RCC_APB2ENR |= (0x1Fu << 2); /* IOPA..IOPE */

    uint32_t base = port_base(PORT_INDEX);

    for (uint32_t pin = START_PIN; pin <= END_PIN && pin < 16u; pin++) {
        if (PORT_INDEX == 0u && (pin == 13u || pin == 14u)) {
            continue; /* SWDIO / SWCLK */
        }
        r[5] = pin;
        int active_high = ACTIVE_LOW ? 0 : 1;
        /* Drive to the inactive level first, then make it an output. */
        set_pin(base, pin, ACTIVE_LOW ? 1 : 0);
        set_mode(base, pin, MODE_OUT_PP_2MHZ);
        delay_ms(200);

        set_pin(base, pin, active_high); /* energise */
        delay_ms(1200);
        set_pin(base, pin, ACTIVE_LOW ? 1 : 0); /* release */
        delay_ms(800);

        set_mode(base, pin, MODE_FLOAT_INPUT);
        delay_ms(500);
    }

    r[6] = 0x444F4E45u; /* "DONE" */
    for (;;) {
    }
}
