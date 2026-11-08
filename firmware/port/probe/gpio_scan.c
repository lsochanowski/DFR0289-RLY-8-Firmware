/* PHY reset hunt v6 — safe variant.
 *
 * Lessons from the failed runs (user observed relays clicking one by one and the board
 * resetting): the previous probe drove every pin in both polarities and then all pins at
 * once, which energised the relay coils together and browned the rail out. The RAM
 * readbacks were garbage because the MCU was restarting mid-scan.
 *
 * This probe:
 *   - drives only ONE pin at a time, and only HIGH (PHY reset is normally active-low,
 *     so HIGH releases it; LOW is what would assert other resets),
 *   - never drives more than one relay coil at a time,
 *   - counts its own boots in RAM at 0x2000F000, so resets during the run are visible,
 *   - stops at the first pin that makes a PHY answer and leaves that pin released.
 *
 *   0x2000F000 boot marker   (0xB007C0DE in the high half, boot count in the low half)
 *   0x20008000 magic "RLY1"  0x20008004 hits        0x20008008 pin-states tried
 *   0x2000800C baseline ID   0x20008010 raw idle sample
 *   0x20008014 last pin code 0x20008018 pin that worked (0xFFFF none) + state
 *   0x20008020 + i*16: code (port<<8 | pin), state (1 high), id1, id2
 */
#include <stdint.h>

#define REG(addr) (*(volatile uint32_t *)(addr))

#define RCC_CR REG(0x40021000u)
#define RCC_CFGR REG(0x40021004u)
#define RCC_AHBENR REG(0x40021014u)
#define RCC_APB2ENR REG(0x40021018u)
#define RCC_AHBRSTR REG(0x40021028u)
#define FLASH_ACR REG(0x40022000u)

#define ETH_MACMIIAR REG(0x40028010u)
#define ETH_MACMIIDR REG(0x40028014u)

#define RCC_AHBENR_ETHMACEN (1u << 14)
#define RCC_AHBRSTR_ETHMACRST (1u << 14)

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

static void delay(uint32_t rounds) {
    for (volatile uint32_t i = 0; i < rounds; i++) {
    }
}

static void pin_mode(uint32_t base, uint32_t pin, uint32_t mode) {
    volatile uint32_t *cr = (volatile uint32_t *)(base + (pin < 8u ? 0x00u : 0x04u));
    uint32_t shift = (pin & 7u) * 4u;
    uint32_t value = *cr;
    value &= ~(0xFu << shift);
    value |= (mode & 0xFu) << shift;
    *cr = value;
}

static void pin_write(uint32_t base, uint32_t pin, int high) {
    volatile uint32_t *odr = (volatile uint32_t *)(base + 0x0Cu);
    if (high) {
        *odr |= (1u << pin);
    } else {
        *odr &= ~(1u << pin);
    }
}

static uint16_t mdio_read(uint8_t pa, uint8_t mr) {
    uint32_t ar = ((uint32_t)pa << 11) | ((uint32_t)mr << 6) | 1u;
    ETH_MACMIIAR = (ETH_MACMIIAR & 0xFFFF0000u) | ar;
    (void)ETH_MACMIIAR;
    for (volatile uint32_t spin = 0; spin < 20000u; spin++) {
        if ((ETH_MACMIIAR & 1u) == 0u) {
            return (uint16_t)(ETH_MACMIIDR & 0xFFFFu);
        }
    }
    return 0xFFFFu;
}

static int find_phy(uint16_t *out_id1, uint16_t *out_id2) {
    for (uint8_t pa = 0; pa < 32u; pa++) {
        uint16_t id1 = mdio_read(pa, 2);
        uint16_t id2 = mdio_read(pa, 3);
        if (id1 != mdio_read(pa, 2) || id2 != mdio_read(pa, 3)) {
            continue;
        }
        if (id1 == 0xFFFFu && id2 == 0xFFFFu) {
            continue;
        }
        if (id1 == 0x0000u && id2 == 0x0000u) {
            continue;
        }
        *out_id1 = id1;
        *out_id2 = id2;
        return 1;
    }
    return 0;
}

static int reserved(uint32_t port, uint32_t pin) {
    if (port == 0u && (pin == 2u || pin == 13u || pin == 14u)) {
        return 1; /* MDIO, SWDIO, SWCLK */
    }
    if (port == 2u && pin == 1u) {
        return 1; /* MDC */
    }
    return 0;
}

int main(void) {
    /* Boot marker survives resets because it is written once per start, never cleared. */
    volatile uint32_t *boot = (volatile uint32_t *)0x2000F000u;
    if ((*boot & 0xFFFF0000u) == 0xB0070000u) {
        *boot = 0xB0070000u | ((*boot & 0xFFFFu) + 1u);
    } else {
        *boot = 0xB0070001u;
    }

    volatile uint32_t *r = (volatile uint32_t *)0x20008000u;
    for (uint32_t i = 0; i < 192u; i++) {
        r[i] = 0;
    }
    r[0] = 0x524C5931u;

    clock_to_72mhz();
    RCC_APB2ENR |= (0x1Fu << 2);
    RCC_AHBENR |= RCC_AHBENR_ETHMACEN;
    RCC_AHBRSTR |= RCC_AHBRSTR_ETHMACRST;
    delay(1000);
    RCC_AHBRSTR &= ~RCC_AHBRSTR_ETHMACRST;
    delay(10000);

    pin_mode(PORTA, 2u, 0xBu); /* MDIO */
    pin_mode(PORTC, 1u, 0xBu); /* MDC */

    delay(40000);
    uint16_t id1 = 0;
    uint16_t id2 = 0;
    r[3] = find_phy(&id1, &id2) ? (((uint32_t)id1 << 16) | id2) : 0u;
    r[4] = ((uint32_t)mdio_read(0, 2) << 16) | mdio_read(0, 3);

    const uint32_t ports[5] = {PORTA, PORTB, PORTC, PORTD, PORTE};
    uint32_t hits = 0;
    uint32_t tried = 0;
    uint32_t winner = 0xFFFFFFFFu;

    for (uint32_t p = 0; p < 5u && winner == 0xFFFFFFFFu; p++) {
        for (uint32_t pin = 0; pin < 16u && winner == 0xFFFFFFFFu; pin++) {
            if (reserved(p, pin)) {
                continue;
            }
            r[5] = (p << 8) | pin;
            pin_write(ports[p], pin, 1);
            pin_mode(ports[p], pin, MODE_OUT_PP_2MHZ);
            delay(140000); /* ~20 ms: let a PHY boot after reset release */
            tried++;
            uint16_t h1 = 0;
            uint16_t h2 = 0;
            if (find_phy(&h1, &h2)) {
                uint32_t slot = 8u + hits * 4u;
                r[slot + 0] = (p << 8) | pin;
                r[slot + 1] = 1u;
                r[slot + 2] = h1;
                r[slot + 3] = h2;
                hits++;
                winner = (p << 8) | pin;
                r[6] = ((uint32_t)h1 << 16) | h2;
                /* Leave this pin released and stop: do not disturb the winning state. */
                break;
            }
            pin_mode(ports[p], pin, MODE_FLOAT_INPUT);
            delay(40000);
        }
    }

    r[1] = hits;
    r[2] = tried;
    r[7] = winner;
    r[191] = 0x444E4521u;
    for (;;) {
    }
}
