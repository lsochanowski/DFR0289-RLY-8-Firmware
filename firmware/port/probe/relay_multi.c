/* Self-describing sweep over the remaining candidate pins.
 *
 * Entry i in the table clicks (i+1) times, so one observation identifies the pin:
 * "relay 6 clicked 14 times" -> table entry 14. This replaces many short per-port runs.
 *
 * Table (13 + 8 = 21 entries, all still unproven):
 *   1..13 : PA0, PA1, PA3, PA4, PA5, PA6, PA7, PA8, PA9, PA10, PA11, PA12, PA15
 *  14..21 : PC0, PC1, PC2, PC3, PC4, PC5, PC6, PC7
 *
 * PA13/PA14 are excluded (SWDIO/SWCLK). Timing: 150 ms on / 200 ms off, 2 s between pins.
 * Polarity: ACTIVE_LOW=1 drives the pin low to energise (build switch), matching the
 * possibility that relays 6-8 use an inverting driver.
 */
#include <stdint.h>

#define REG(addr) (*(volatile uint32_t *)(addr))

#ifndef ACTIVE_LOW
#define ACTIVE_LOW 1
#endif

#define RCC_CR REG(0x40021000u)
#define RCC_CFGR REG(0x40021004u)
#define RCC_APB2ENR REG(0x40021018u)
#define FLASH_ACR REG(0x40022000u)

#define MODE_FLOAT_INPUT 0x4u
#define MODE_OUT_PP_2MHZ 0x2u

#define PORTA 0x40010800u
#define PORTC 0x40011000u

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

static void set_mode(uint32_t base, uint32_t pin, uint32_t mode) {
    volatile uint32_t *cr = (volatile uint32_t *)(base + (pin < 8u ? 0x00u : 0x04u));
    uint32_t shift = (pin & 7u) * 4u;
    uint32_t value = *cr;
    value &= ~(0xFu << shift);
    value |= (mode & 0xFu) << shift;
    *cr = value;
}

static void set_pin(uint32_t base, uint32_t pin, int high) {
    volatile uint32_t *odr = (volatile uint32_t *)(base + 0x0Cu);
    if (high) {
        *odr |= (1u << pin);
    } else {
        *odr &= ~(1u << pin);
    }
}

static const uint32_t table_port[21] = {
    PORTA, PORTA, PORTA, PORTA, PORTA, PORTA, PORTA, PORTA, PORTA, PORTA, PORTA, PORTA, PORTA,
    PORTC, PORTC, PORTC, PORTC, PORTC, PORTC, PORTC, PORTC,
};

static const uint32_t table_pin[21] = {
    0, 1, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 15,
    0, 1, 2, 3, 4, 5, 6, 7,
};

int main(void) {
    volatile uint32_t *r = (volatile uint32_t *)0x20008000u;
    r[0] = 0x524C5931u;
    r[1] = 21;

    clock_to_72mhz();
    RCC_APB2ENR |= (1u << 2) | (1u << 4); /* IOPAEN, IOPCEN */

    for (uint32_t i = 0; i < 21u; i++) {
        r[2] = i + 1u; /* entry number == click count */
        uint32_t base = table_port[i];
        uint32_t pin = table_pin[i];
        for (uint32_t c = 0; c < i + 1u; c++) {
            r[3] = c + 1u;
#if ACTIVE_LOW
            set_pin(base, pin, 0);
            set_mode(base, pin, MODE_OUT_PP_2MHZ);
            delay_ms(150);
            set_mode(base, pin, MODE_FLOAT_INPUT);
            set_pin(base, pin, 1);
#else
            set_pin(base, pin, 1);
            set_mode(base, pin, MODE_OUT_PP_2MHZ);
            delay_ms(150);
            set_mode(base, pin, MODE_FLOAT_INPUT);
            set_pin(base, pin, 0);
#endif
            delay_ms(200);
        }
        delay_ms(2000); /* separates one entry's train from the next */
    }

    r[4] = 0x444F4E45u; /* "DONE" */
    for (;;) {
    }
}
