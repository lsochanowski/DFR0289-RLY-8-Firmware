/* All non-relay pins driven LOW *and* MDIO polled at the same time.
 *
 * Rationale: the PHY answers on MDIO only sometimes (one hit ever). If its reset or enable
 * is one of the pins we have been treating as "unknown", the PHY will only answer while
 * that pin is held in the right state. So hold every non-relay pin low, keep PA2/PC1 as
 * MDIO/MDC, and scan all 32 addresses repeatedly.
 *
 * Excluded from driving (same as the relay/eth functions require):
 *   PA13, PA14  SWDIO/SWCLK
 *   PA2, PC1    MDIO / MDC (alternate function)
 *   PB15        relay 8
 *   PC6..PC12   relays 7,6,1,2,3,4,5
 *
 * RAM report: r[1..5] driven masks A..E, r[6] responses, r[7] first answering address,
 * r[8] ID, r[9] rounds completed, r[10] boot count, r[12..43] per-address hit counts.
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

#define MODE_OUT_PP_2MHZ 0x2u
#define ETHMACEN (1u << 14)
#define ETHMACRST (1u << 14)

/* Port A: drop PA13/PA14 (SWD) and PA2 (MDIO). Port C: drop PC1 (MDC) and the relay block. */
#define MASK_A 0x9FFBu /* 0x9FFF without bit 2 */
#define MASK_B 0x7FFFu
#define MASK_C 0xE03Du /* 0xE03F without bit 1 */
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

static void delay_ms(uint32_t ms) {
    for (uint32_t i = 0; i < ms; i++) {
        for (volatile uint32_t j = 0; j < 7000u; j++) {
        }
    }
}

static void drive_port(uint32_t base, uint32_t mask) {
    volatile uint32_t *odr = (volatile uint32_t *)(uintptr_t)(base + 0x0Cu);
    volatile uint32_t *crl = (volatile uint32_t *)(uintptr_t)(base + 0x00u);
    volatile uint32_t *crh = (volatile uint32_t *)(uintptr_t)(base + 0x04u);

    *odr &= ~mask; /* drive low */

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

static void mdio_pins(void) {
    volatile uint32_t *gpioa_crl = (volatile uint32_t *)0x40010800u;
    volatile uint32_t *gpioc_crh = (volatile uint32_t *)0x40011004u;
    *gpioa_crl = (*gpioa_crl & ~(0xFu << 8)) | (0xBu << 8); /* PA2 = MDIO, AF push-pull */
    *gpioc_crh = (*gpioc_crh & ~(0xFu << 4)) | (0xBu << 4); /* PC1 = MDC */
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

int main(void) {
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
    r[7] = 0xFFFFFFFFu;
    r[10] = *boot & 0xFFFFu;

    clock_to_72mhz();
    RCC_APB2ENR |= (0x1Fu << 2);
    RCC_AHBENR |= ETHMACEN;
    RCC_AHBRSTR |= ETHMACRST;
    delay_ms(1);
    RCC_AHBRSTR &= ~ETHMACRST;

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
    mdio_pins();
    delay_ms(50);

    uint32_t total = 0;
    for (uint32_t round = 0; round < 200u; round++) {
        for (uint32_t pa = 0; pa < 32u; pa++) {
            uint16_t id1 = mdio_read((uint8_t)pa, 2);
            uint16_t id2 = mdio_read((uint8_t)pa, 3);
            if (id1 == 0xFFFFu && id2 == 0xFFFFu) {
                continue;
            }
            if (id1 == 0x0000u && id2 == 0x0000u) {
                continue;
            }
            r[12u + pa] = r[12u + pa] + 1u;
            if (r[7] == 0xFFFFFFFFu) {
                r[7] = pa;
                r[8] = ((uint32_t)id1 << 16) | id2;
            }
            total++;
        }
        r[6] = total;
        r[9] = round + 1u;
    }

    for (;;) {
    }
}
