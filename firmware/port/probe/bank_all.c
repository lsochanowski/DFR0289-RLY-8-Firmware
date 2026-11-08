/* Drive EVERY pin of one GPIO bank at once and leave them driven.
 *
 * The user's idea: instead of hunting pin by pin, switch on the whole bank and see which
 * relays close. That answers "which bank?" in one look.
 *
 * Build: -DBANK=n (0=A .. 4=E) [-DACTIVE_LOW=1]
 * PA13/PA14 (SWDIO/SWCLK) are always skipped on bank A: driving them kills the debugger,
 * and then the result could not even be read back.
 *
 * The pins stay energised until the next reset, so the observer can take their time.
 * RAM report at 0x20008000: magic, bank, pins driven (bitmap), polarity, state.
 */
#include <stdint.h>

#define REG(addr) (*(volatile uint32_t *)(addr))

#ifndef BANK
#define BANK 0
#endif
#ifndef ACTIVE_LOW
#define ACTIVE_LOW 0
#endif

/* Optional pin subset: -DMASK=0x00FF drives only PB0..PB7. Default = every pin of the bank.
 * Bisection uses this to halve the search space each round. */
#ifndef MASK
#define MASK 0xFFFFu
#endif

#define RCC_CR REG(0x40021000u)
#define RCC_CFGR REG(0x40021004u)
#define RCC_APB2ENR REG(0x40021018u)
#define FLASH_ACR REG(0x40022000u)

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

static uint32_t port_base(uint32_t index) {
    switch (index) {
    case 0:
        return 0x40010800u;
    case 1:
        return 0x40010C00u;
    case 2:
        return 0x40011000u;
    case 3:
        return 0x40011400u;
    default:
        return 0x40011800u;
    }
}

int main(void) {
    volatile uint32_t *r = (volatile uint32_t *)0x20008000u;
    r[0] = 0x524C5931u;
    r[1] = BANK;
    r[2] = ACTIVE_LOW;

    clock_to_72mhz();
    RCC_APB2ENR |= (0x1Fu << 2);

    uint32_t base = port_base(BANK);
    volatile uint32_t *odr = (volatile uint32_t *)(uintptr_t)(base + 0x0Cu);
    volatile uint32_t *crl = (volatile uint32_t *)(uintptr_t)(base + 0x00u);
    volatile uint32_t *crh = (volatile uint32_t *)(uintptr_t)(base + 0x04u);

    uint32_t driven = 0;

    for (uint32_t pin = 0; pin < 16u; pin++) {
        if ((MASK & (1u << pin)) == 0u) {
            continue;
        }
        if (BANK == 0u && (pin == 13u || pin == 14u)) {
            continue; /* SWDIO / SWCLK */
        }
        driven |= (1u << pin);
    }

    /* Latch the driven level FIRST, then switch the pins to outputs, so the transition is
     * a single edge per pin rather than a glitch. */
    if (ACTIVE_LOW) {
        *odr &= ~driven;
    } else {
        *odr |= driven;
    }

    uint32_t crl_value = *crl;
    uint32_t crh_value = *crh;
    for (uint32_t pin = 0; pin < 16u; pin++) {
        if ((driven & (1u << pin)) == 0u) {
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

    r[3] = driven;
    r[4] = 0x4E4F5F4Fu; /* "ON_O" — pins are energised and stay that way */
    for (;;) {
    }
}
