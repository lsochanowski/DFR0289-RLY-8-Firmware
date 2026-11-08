/* Does the DP83848AA answer late?
 *
 * Every earlier scan started ~50 ms after reset and finished within ~2 s. If the PHY
 * needs longer to come out of its own reset (some supervisors hold it for hundreds of ms),
 * those scans would all miss it. This probe polls all 32 addresses every ~10 ms for up to
 * 10 seconds and records the first round that produced an answer, plus the ID and address.
 *
 *   0x20008000 magic "RLY1"
 *   0x20008004 rounds executed
 *   0x20008008 first answering round (0xFFFFFFFF = never)
 *   0x2000800C answering address
 *   0x20008010 ID (id1<<16 | id2)
 *   0x20008014 responses in the last round (stability check)
 *   0x20008018 boot count
 *   0x20008040 + pa*4: total responses per address
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
#define MAX_ROUNDS 1000u /* ~10 s of polling */

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

static void delay_ms(uint32_t ms) {
    for (uint32_t i = 0; i < ms; i++) {
        for (volatile uint32_t j = 0; j < 7000u; j++) {
        }
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
    r[2] = 0xFFFFFFFFu;
    r[6] = *boot & 0xFFFFu;

    clock_to_72mhz();
    RCC_APB2ENR |= (1u << 2) | (1u << 4);
    RCC_AHBENR |= ETHMACEN;
    RCC_AHBRSTR |= ETHMACRST;
    delay_ms(1);
    RCC_AHBRSTR &= ~ETHMACRST;

    GPIOA_CRL = (GPIOA_CRL & ~(0xFu << 8)) | (0xBu << 8); /* PA2 = MDIO */
    GPIOC_CRH = (GPIOC_CRH & ~(0xFu << 4)) | (0xBu << 4); /* PC1 = MDC */

    for (uint32_t round = 0; round < MAX_ROUNDS; round++) {
        uint32_t responses = 0;
        for (uint32_t pa = 0; pa < 32u; pa++) {
            uint16_t id1 = mdio_read((uint8_t)pa, 2);
            uint16_t id2 = mdio_read((uint8_t)pa, 3);
            if (id1 == 0xFFFFu && id2 == 0xFFFFu) {
                continue;
            }
            if (id1 == 0x0000u && id2 == 0x0000u) {
                continue;
            }
            responses++;
            r[16u + pa] = r[16u + pa] + 1u;
            if (r[2] == 0xFFFFFFFFu) {
                r[2] = round;
                r[3] = pa;
                r[4] = ((uint32_t)id1 << 16) | id2;
            }
        }
        r[5] = responses;
        r[1] = round + 1u;
        delay_ms(10);
    }

    r[191] = 0x444E4521u;
    for (;;) {
    }
}
