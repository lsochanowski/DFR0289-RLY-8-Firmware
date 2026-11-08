/* Relay identification by click count.
 *
 * One port per run: pin N clicks N+1 times, with a clear gap between pins, so a human
 * can map "which relay clicked how many times" to "which GPIO". The port is chosen at
 * build time: -DPORT_INDEX=0 (A), 1 (B), 2 (C), 3 (D), 4 (E).
 *
 * Timing: 200 ms on / 250 ms off per click, 1.5 s between pins, then stop.
 * PA13/PA14 (SWDIO/SWCLK) are never driven. Progress is mirrored to RAM at 0x20008000.
 */
#include <stdint.h>

#define REG(addr) (*(volatile uint32_t *)(addr))

#ifndef PORT_INDEX
#define PORT_INDEX 0
#endif

/* Some relay drivers (PNP transistor, or a sinking buffer fed from VCC) energise the coil
 * when the MCU pin is LOW. Build with -DACTIVE_LOW=1 to sweep that polarity instead. */
#ifndef ACTIVE_LOW
#define ACTIVE_LOW 0
#endif

/* Optional pin window, for re-checking a suspected 8-pin relay block quickly:
 * -DSTART_PIN=8 -DEND_PIN=15 still uses N+1 clicks per pin. */
#ifndef START_PIN
#define START_PIN 0
#endif
#ifndef END_PIN
#define END_PIN 15
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

static volatile uint32_t *port_base(uint32_t index) {
    switch (index) {
    case 0:
        return (volatile uint32_t *)0x40010800u; /* A */
    case 1:
        return (volatile uint32_t *)0x40010C00u; /* B */
    case 2:
        return (volatile uint32_t *)0x40011000u; /* C */
    case 3:
        return (volatile uint32_t *)0x40011400u; /* D */
    default:
        return (volatile uint32_t *)0x40011800u; /* E */
    }
}

static void set_mode(volatile uint32_t *base, uint32_t pin, uint32_t mode) {
    volatile uint32_t *cr = base + (pin < 8u ? 0x00u : 0x01u);
    uint32_t shift = (pin & 7u) * 4u;
    uint32_t value = *cr;
    value &= ~(0xFu << shift);
    value |= (mode & 0xFu) << shift;
    *cr = value;
}

static void set_pin(volatile uint32_t *base, uint32_t pin, int high) {
    volatile uint32_t *odr = base + 0x03u;
    if (high) {
        *odr |= (1u << pin);
    } else {
        *odr &= ~(1u << pin);
    }
}

int main(void) {
    /* Counts boots so repeated sweeps can be recognised as resets rather than a loop. */
    volatile uint32_t *boot = (volatile uint32_t *)0x2000F000u;
    if ((*boot & 0xFFFF0000u) == 0xB0070000u) {
        *boot = 0xB0070000u | ((*boot & 0xFFFFu) + 1u);
    } else {
        *boot = 0xB0070001u;
    }

    volatile uint32_t *r = (volatile uint32_t *)0x20008000u;
    r[0] = 0x524C5931u;
    r[1] = PORT_INDEX;
    r[5] = 0;
    r[6] = *boot & 0xFFFFu;

    clock_to_72mhz();
    RCC_APB2ENR |= (0x1Fu << 2);

    volatile uint32_t *base = port_base(PORT_INDEX);

    for (uint32_t pin = START_PIN; pin <= END_PIN; pin++) {
        if (PORT_INDEX == 0u && (pin == 13u || pin == 14u)) {
            continue; /* never drive the SWD pair */
        }
        uint32_t clicks = pin + 1u;
        r[2] = pin;
        for (uint32_t c = 0; c < clicks; c++) {
            r[3] = c + 1u;
#if ACTIVE_LOW
            set_pin(base, pin, 0);
            set_mode(base, pin, MODE_OUT_PP_2MHZ);
            delay_ms(200);
            set_mode(base, pin, MODE_FLOAT_INPUT);
            set_pin(base, pin, 1);
#else
            set_pin(base, pin, 1);
            set_mode(base, pin, MODE_OUT_PP_2MHZ);
            delay_ms(200);
            set_mode(base, pin, MODE_FLOAT_INPUT);
            set_pin(base, pin, 0);
#endif
            delay_ms(250);
        }
        delay_ms(1500); /* separates one pin's click train from the next */
    }

    r[4] = 0x444F4E45u; /* "DONE" */
    for (;;) {
    }
}
