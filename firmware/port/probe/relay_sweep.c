/* Relay pin sweep: drive one GPIO at a time so a human can watch which relay clicks.
 *
 * Order (fixed, so the observer can correlate):
 *   for port in A, B, C, D, E:
 *       for pin in 0..15 except the SWD pair (PA13/PA14):
 *           drive HIGH for ~800 ms, release (input, ODR low) for ~250 ms
 *       then a ~3 s pause before the next port
 *
 * Nothing else is touched: no ETH clocks, no MDIO. The pin being driven is mirrored to
 * RAM at 0x20008000 (magic + current port/pin + a per-port counter), so the sweep can be
 * cross-checked afterwards over SWD.
 */
#include <stdint.h>

#define REG(addr) (*(volatile uint32_t *)(addr))

#define RCC_CR REG(0x40021000u)
#define RCC_CFGR REG(0x40021004u)
#define RCC_APB2ENR REG(0x40021018u)
#define FLASH_ACR REG(0x40022000u)

#define PORTA 0x40010800u
#define PORTB 0x40010C00u
#define PORTC 0x40011000u
#define PORTD 0x40011400u
#define PORTE 0x40011800u

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

/* Rough millisecond delay at 72 MHz. */
static void delay_ms(uint32_t ms) {
    for (uint32_t i = 0; i < ms; i++) {
        for (volatile uint32_t j = 0; j < 7000u; j++) {
        }
    }
}

static void pin_high(uint32_t base, uint32_t pin) {
    volatile uint32_t *odr = (volatile uint32_t *)(base + 0x0Cu);
    volatile uint32_t *cr = (volatile uint32_t *)(base + (pin < 8u ? 0x00u : 0x04u));
    uint32_t shift = (pin & 7u) * 4u;
    *odr |= (1u << pin);
    uint32_t value = *cr;
    value &= ~(0xFu << shift);
    value |= (MODE_OUT_PP_2MHZ << shift);
    *cr = value;
}

static void pin_release(uint32_t base, uint32_t pin) {
    volatile uint32_t *odr = (volatile uint32_t *)(base + 0x0Cu);
    volatile uint32_t *cr = (volatile uint32_t *)(base + (pin < 8u ? 0x00u : 0x04u));
    uint32_t shift = (pin & 7u) * 4u;
    uint32_t value = *cr;
    value &= ~(0xFu << shift);
    value |= (MODE_FLOAT_INPUT << shift);
    *cr = value;
    *odr &= ~(1u << pin);
}

int main(void) {
    volatile uint32_t *r = (volatile uint32_t *)0x20008000u;
    r[0] = 0x524C5931u;
    r[1] = 0;

    clock_to_72mhz();
    RCC_APB2ENR |= (0x1Fu << 2); /* IOPA..IOPE */

    const uint32_t ports[5] = {PORTA, PORTB, PORTC, PORTD, PORTE};

    for (uint32_t p = 0; p < 5u; p++) {
        r[2] = p; /* current port index, so a sweep in progress is identifiable */
        for (uint32_t pin = 0; pin < 16u; pin++) {
            if (p == 0u && (pin == 13u || pin == 14u)) {
                continue; /* SWDIO / SWCLK: never drive these */
            }
            r[3] = pin;
            r[4] = r[4] + 1u;
            pin_high(ports[p], pin);
            delay_ms(800);
            pin_release(ports[p], pin);
            delay_ms(250);
        }
        /* Long gap marks the port boundary for the observer. */
        delay_ms(3000);
    }

    r[5] = 0x444F4E45u; /* "DONE" */
    for (;;) {
    }
}
