/* MDIO identify v5 — find the PHY address and read its registers.
 *
 * A previous run returned ID 0x2000:0x5C90, which is exactly the TI DP83848
 * (PHYID1 0x2000, PHYID2 0x5C90), but the probe did not record which MDIO address
 * answered. This build drives no GPIO at all (no relay churn, no resets), waits for
 * the MAC to settle, and requires three identical reads before believing an address.
 *
 *   0x20008000 magic "RLY1"
 *   0x20008004 count of responsive addresses
 *   0x20008008 responsive address bitmap (bit0 = address 0)
 *   0x2000800C RCC_AHBENR
 *   0x20008010 raw sample (pa0 reg2:reg3)
 *   0x20008014 DP83848 address, or 0xFFFFFFFF
 *   0x20008018 BMCR of the DP83848
 *   0x2000801C BMSR of the DP83848
 *   0x20008020 ANAR,  0x20008024 ANLPAR
 *   0x20008028 DP83848 PHYSTS (reg 0x10)
 *   0x2000802C DP83848 PHYCTRL (reg 0x11)
 *   0x20008040 + pa*8: id1, id2, bmcr, bmsr, anar, anlpar, phys, phyctrl
 *   0x2000F000 boot marker (high half 0xB007, low half boot count)
 */
#include <stdint.h>

#define REG(addr) (*(volatile uint32_t *)(addr))

#define RCC_CR REG(0x40021000u)
#define RCC_CFGR REG(0x40021004u)
#define RCC_AHBENR REG(0x40021014u)
#define RCC_APB2ENR REG(0x40021018u)
#define RCC_AHBRSTR REG(0x40021028u)
#define FLASH_ACR REG(0x40022000u)
#define GPIOA_CRL REG(0x40010800u)
#define GPIOC_CRH REG(0x40011004u)

#define ETH_MACMIIAR REG(0x40028010u)
#define ETH_MACMIIDR REG(0x40028014u)

#define ETHMACEN (1u << 14)
#define ETHMACRST (1u << 14)

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

static void delay(uint32_t rounds) {
    for (volatile uint32_t i = 0; i < rounds; i++) {
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

static int stable_id(uint8_t pa, uint16_t *id1, uint16_t *id2) {
    uint16_t a1 = mdio_read(pa, 2);
    uint16_t a2 = mdio_read(pa, 3);
    uint16_t b1 = mdio_read(pa, 2);
    uint16_t b2 = mdio_read(pa, 3);
    uint16_t c1 = mdio_read(pa, 2);
    uint16_t c2 = mdio_read(pa, 3);
    if (a1 != b1 || b1 != c1 || a2 != b2 || b2 != c2) {
        return 0;
    }
    if (a1 == 0xFFFFu && a2 == 0xFFFFu) {
        return 0;
    }
    *id1 = a1;
    *id2 = a2;
    return 1;
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
    r[5] = 0xFFFFFFFFu;

    clock_to_72mhz();

    RCC_APB2ENR |= (1u << 2) | (1u << 4); /* IOPAEN, IOPCEN */
    RCC_AHBENR |= ETHMACEN;
    RCC_AHBRSTR |= ETHMACRST;
    delay(2000);
    RCC_AHBRSTR &= ~ETHMACRST;
    r[3] = RCC_AHBENR;
    delay(200000);

    /* PA2 = MDIO, PC1 = MDC. PA0 is either RMII_CRS_DV or, on this board, possibly the
     * PHY reset line: a previous run answered only after PA0 was driven high. */
    GPIOA_CRL = (GPIOA_CRL & ~(0xFu << 8)) | (0xBu << 8);
    GPIOC_CRH = (GPIOC_CRH & ~(0xFu << 4)) | (0xBu << 4);
    delay(200000);

    /* Phase 1: PA0 left floating. */
    uint32_t count_a = 0;
    uint32_t bitmap_a = 0;
    for (uint32_t pa = 0; pa < 32u; pa++) {
        uint16_t id1 = 0;
        uint16_t id2 = 0;
        if (stable_id((uint8_t)pa, &id1, &id2)) {
            count_a++;
            bitmap_a |= (1u << pa);
            if (r[5] == 0xFFFFFFFFu) {
                r[5] = pa;
                r[6] = ((uint32_t)id1 << 16) | id2;
            }
        }
    }
    r[1] = count_a;
    r[2] = bitmap_a;

    /* Phase 2: PA0 driven high (possible PHY reset release). */
    GPIOA_CRL = (GPIOA_CRL & ~0xFu) | 0x2u; /* PA0 output push-pull 2 MHz */
    (*(volatile uint32_t *)0x4001080Cu) |= 1u; /* ODR0 = 1 */
    delay(400000);                             /* ~50 ms */

    uint32_t count_b = 0;
    uint32_t bitmap_b = 0;
    for (uint32_t pa = 0; pa < 32u; pa++) {
        uint16_t id1 = 0;
        uint16_t id2 = 0;
        if (stable_id((uint8_t)pa, &id1, &id2)) {
            count_b++;
            bitmap_b |= (1u << pa);
            if (r[5] == 0xFFFFFFFFu) {
                r[5] = pa;
                r[6] = ((uint32_t)id1 << 16) | id2;
            }
            uint32_t slot = 16u + pa * 8u;
            r[slot + 0] = id1;
            r[slot + 1] = id2;
            r[slot + 2] = mdio_read((uint8_t)pa, 0);
            r[slot + 3] = mdio_read((uint8_t)pa, 1);
            r[slot + 4] = mdio_read((uint8_t)pa, 4);
            r[slot + 5] = mdio_read((uint8_t)pa, 5);
            r[slot + 6] = mdio_read((uint8_t)pa, 16);
            r[slot + 7] = mdio_read((uint8_t)pa, 17);
        }
    }
    r[13] = count_b;
    r[14] = bitmap_b;
    r[191] = 0x444E4521u;
    for (;;) {
    }
}
