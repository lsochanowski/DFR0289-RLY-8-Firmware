#include "clock.h"

#include "board.h"

#define REG(addr) (*(volatile uint32_t *)(addr))

#define RCC_CR REG(0x40021000u)
#define RCC_CFGR REG(0x40021004u)
#define FLASH_ACR REG(0x40022000u)

#define RCC_CR_HSEON (1u << 16)
#define RCC_CR_HSERDY (1u << 17)
#define RCC_CR_PLLON (1u << 24)
#define RCC_CR_PLLRDY (1u << 25)
#define RCC_CR_PLL2ON (1u << 26)
#define RCC_CR_PLL2RDY (1u << 27)
#define RCC_CR_PLL3ON (1u << 28)
#define RCC_CR_PLL3RDY (1u << 29)
#define RCC_CFGR2 REG(0x4002102Cu)
#define RCC_APB2ENR REG(0x40021018u)
#define GPIOA_CRH REG(0x40010804u)

#define RCC_CFGR2_PREDIV1SRC_PLL2 (1u << 16)
#define RCC_CFGR2_PLL3MUL_X10 (0x8u << 12)
#define RCC_CFGR2_PLL2MUL_X8 (0x6u << 8)
#define RCC_CFGR2_PREDIV2_DIV5 (0x4u << 4)
#define RCC_CFGR2_PREDIV1_DIV5 (0x4u << 0)

#define MCO_HSE 0x6u
#define MCO_PLL3 0xBu

#define SPIN_LIMIT 200000u

volatile clock_status clock_last_status;

int clock_init_72mhz(void) {
    clock_last_status.hse_ready = 0;
    clock_last_status.pll_ready = 0;
    clock_last_status.switched_to_pll = 0;

    /* Two wait states are required above 48 MHz; keep the prefetch buffer on. */
    FLASH_ACR = 0x12u;

    RCC_CR |= RCC_CR_HSEON;
    for (volatile uint32_t spin = 0; spin < SPIN_LIMIT; spin++) {
        if (RCC_CR & RCC_CR_HSERDY) {
            clock_last_status.hse_ready = 1;
            break;
        }
    }
    if (!clock_last_status.hse_ready) {
        return -1;
    }

    /* HSE is a 25 MHz oscillator (measured with DWT_CYCCNT; it also clocks the PHY via MCO).
     * Connectivity-line chain: HSE /5 = 5 MHz -> PLL2 x8 = 40 MHz -> PREDIV1 /5 = 8 MHz
     * -> PLL x9 = 72 MHz. PLL3 shares PREDIV2: 5 MHz x10 = 50 MHz (RMII reference, if needed). */
    RCC_CFGR2 = RCC_CFGR2_PREDIV1SRC_PLL2 | RCC_CFGR2_PLL3MUL_X10 | RCC_CFGR2_PLL2MUL_X8 |
                RCC_CFGR2_PREDIV2_DIV5 | RCC_CFGR2_PREDIV1_DIV5;
    RCC_CR |= RCC_CR_PLL2ON;
    for (volatile uint32_t spin = 0; spin < SPIN_LIMIT; spin++) {
        if (RCC_CR & RCC_CR_PLL2RDY) {
            break;
        }
    }
    if (!(RCC_CR & RCC_CR_PLL2RDY)) {
        return -4;
    }

    /* PLLSRC = PREDIV1, PLLMUL = x9, AHB /1, APB1 /2, APB2 /1, MCO = HSE (25 MHz to the PHY). */
    uint32_t cfgr = RCC_CFGR;
    cfgr &= ~((0xFu << 24) | (0xFu << 18) | (0x7u << 11) | (0x7u << 8) | (0xFu << 4) | (1u << 17) | 0x3u);
    cfgr |= (0x7u << 18) | (1u << 16) | (0x4u << 8);
    RCC_CFGR = cfgr;

    RCC_CR |= RCC_CR_PLLON;
    for (volatile uint32_t spin = 0; spin < SPIN_LIMIT; spin++) {
        if (RCC_CR & RCC_CR_PLLRDY) {
            clock_last_status.pll_ready = 1;
            break;
        }
    }
    if (!clock_last_status.pll_ready) {
        return -2;
    }

    uint32_t sw = RCC_CFGR;
    sw = (sw & ~0x3u) | 0x2u; /* SW = PLL */
    RCC_CFGR = sw;
    for (volatile uint32_t spin = 0; spin < SPIN_LIMIT; spin++) {
        if ((RCC_CFGR & 0xCu) == 0x8u) { /* SWS = PLL */
            clock_last_status.switched_to_pll = 1;
            break;
        }
    }
    return clock_last_status.switched_to_pll ? 0 : -3;
}

/* The DP83848 has no crystal of its own: its X1 input is wired to PA8 (MCO). Without this
 * the PHY answers on MDIO but never sends link pulses (measured: link only after MCO on). */
int clock_phy_mco(int rmii) {
    uint32_t src = MCO_HSE; /* MII: 25 MHz */
    if (rmii) {
        RCC_CR |= RCC_CR_PLL3ON; /* RMII: 50 MHz from PLL3 */
        for (volatile uint32_t spin = 0; spin < SPIN_LIMIT; spin++) {
            if (RCC_CR & RCC_CR_PLL3RDY) {
                break;
            }
        }
        if (!(RCC_CR & RCC_CR_PLL3RDY)) {
            return -1;
        }
        src = MCO_PLL3;
    }
    RCC_APB2ENR |= (1u << 2); /* IOPAEN */
    GPIOA_CRH = (GPIOA_CRH & ~0xFu) | 0xBu; /* PA8: AF push-pull 50 MHz */
    RCC_CFGR = (RCC_CFGR & ~(0xFu << 24)) | (src << 24);
    return 0;
}
