#include "eth.h"

#include "board.h"
#include "clock.h"

#define REG(addr) (*(volatile uint32_t *)(addr))

#define RCC_AHBENR REG(0x40021014u)
#define RCC_APB2ENR REG(0x40021018u)
#define RCC_AHBRSTR REG(0x40021028u)
#define AFIO_MAPR REG(0x40010004u) /* bit 23 = MII_RMII_SEL */

#define ETH_MACCR REG(0x40028000u)
#define ETH_MACMIIAR REG(0x40028010u) /* RM0008: MAC base + 0x10 */
#define ETH_MACMIIDR REG(0x40028014u) /* RM0008: MAC base + 0x14 */

#define RCC_AHBENR_ETHMACEN (1u << 14)
#define RCC_AHBENR_ETHMACTXEN (1u << 15)
#define RCC_AHBENR_ETHMACRXEN (1u << 16)
#define RCC_AHBRSTR_ETHMACRST (1u << 14)

#define MODE_AF_PP_50MHZ 0xBu
#define MODE_INPUT_FLOAT 0x4u

volatile eth_status eth_last_status;

static void pin_mode(uint32_t base, uint8_t pin, uint32_t mode) {
    volatile uint32_t *cr = (volatile uint32_t *)(base + (pin < 8u ? 0x00u : 0x04u));
    uint32_t shift = (uint32_t)(pin & 7u) * 4u;
    uint32_t value = *cr;
    value &= ~(0xFu << shift);
    value |= (mode & 0xFu) << shift;
    *cr = value;
}

static uint16_t mdio_read(uint8_t pa, uint8_t mr) {
    uint32_t ar = ((uint32_t)pa << 11) | ((uint32_t)mr << 6) | 1u; /* CR = 0: 60-100 MHz */
    ETH_MACMIIAR = (ETH_MACMIIAR & 0xFFFF0000u) | ar;
    (void)ETH_MACMIIAR; /* make sure the write lands before polling */
    for (volatile uint32_t spin = 0; spin < 20000u; spin++) {
        if ((ETH_MACMIIAR & 1u) == 0u) {
            return (uint16_t)(ETH_MACMIIDR & 0xFFFFu);
        }
    }
    return 0xFFFFu;
}

static void mdio_write(uint8_t pa, uint8_t mr, uint16_t value) {
    ETH_MACMIIDR = value;
    uint32_t ar = ((uint32_t)pa << 11) | ((uint32_t)mr << 6) | (1u << 1) | 1u; /* MW = 1 */
    ETH_MACMIIAR = (ETH_MACMIIAR & 0xFFFF0000u) | ar;
    (void)ETH_MACMIIAR;
    for (volatile uint32_t spin = 0; spin < 20000u; spin++) {
        if ((ETH_MACMIIAR & 1u) == 0u) {
            return;
        }
    }
}

static void eth_pins(void) {
#if ETH_MODE_MII
    /* MII: needs 25 MHz TX/RX clocks from the PHY and the full 4-bit data path. */
    pin_mode(GPIOA_BASE, 0u, MODE_INPUT_FLOAT);  /* ETH_MII_CRS */
    pin_mode(GPIOA_BASE, 1u, MODE_INPUT_FLOAT);  /* ETH_MII_RX_CLK */
    pin_mode(GPIOA_BASE, 2u, MODE_AF_PP_50MHZ);  /* ETH_MDIO */
    pin_mode(GPIOA_BASE, 3u, MODE_INPUT_FLOAT);  /* ETH_MII_COL */
    pin_mode(GPIOA_BASE, 7u, MODE_INPUT_FLOAT);  /* ETH_MII_RX_DV */
    pin_mode(GPIOB_BASE, 0u, MODE_INPUT_FLOAT);  /* ETH_MII_RXD2 */
    pin_mode(GPIOB_BASE, 1u, MODE_INPUT_FLOAT);  /* ETH_MII_RXD3 */
    pin_mode(GPIOB_BASE, 8u, MODE_AF_PP_50MHZ);  /* ETH_MII_TXD3 */
    pin_mode(GPIOB_BASE, 10u, MODE_INPUT_FLOAT); /* ETH_MII_RX_ER */
    pin_mode(GPIOB_BASE, 11u, MODE_AF_PP_50MHZ); /* ETH_MII_TX_EN */
    pin_mode(GPIOB_BASE, 12u, MODE_AF_PP_50MHZ); /* ETH_MII_TXD0 */
    pin_mode(GPIOB_BASE, 13u, MODE_AF_PP_50MHZ); /* ETH_MII_TXD1 */
    pin_mode(GPIOC_BASE, 1u, MODE_AF_PP_50MHZ);  /* ETH_MDC */
    pin_mode(GPIOC_BASE, 2u, MODE_AF_PP_50MHZ);  /* ETH_MII_TXD2 */
    pin_mode(GPIOC_BASE, 3u, MODE_INPUT_FLOAT);  /* ETH_MII_TX_CLK */
    pin_mode(GPIOC_BASE, 4u, MODE_INPUT_FLOAT);  /* ETH_MII_RXD0 */
    pin_mode(GPIOC_BASE, 5u, MODE_INPUT_FLOAT);  /* ETH_MII_RXD1 */
#else
    /* RMII: PHY supplies 50 MHz on PA1. */
    pin_mode(GPIOA_BASE, 1u, MODE_INPUT_FLOAT);  /* ETH_RMII_REF_CLK */
    pin_mode(GPIOA_BASE, 2u, MODE_AF_PP_50MHZ);  /* ETH_MDIO */
    pin_mode(GPIOA_BASE, 7u, MODE_INPUT_FLOAT);  /* ETH_RMII_CRS_DV */
    pin_mode(GPIOB_BASE, 11u, MODE_AF_PP_50MHZ); /* ETH_RMII_TX_EN */
    pin_mode(GPIOB_BASE, 12u, MODE_AF_PP_50MHZ); /* ETH_RMII_TXD0 */
    pin_mode(GPIOB_BASE, 13u, MODE_AF_PP_50MHZ); /* ETH_RMII_TXD1 */
    pin_mode(GPIOC_BASE, 1u, MODE_AF_PP_50MHZ);  /* ETH_MDC */
    pin_mode(GPIOC_BASE, 4u, MODE_INPUT_FLOAT);  /* ETH_RMII_RXD0 */
    pin_mode(GPIOC_BASE, 5u, MODE_INPUT_FLOAT);  /* ETH_RMII_RXD1 */
#endif
}

int eth_init(void) {
    eth_last_status.phy_address = 0xFFu;
    eth_last_status.link_up = 0;

    RCC_AHBENR |= RCC_AHBENR_ETHMACEN | RCC_AHBENR_ETHMACTXEN | RCC_AHBENR_ETHMACRXEN;
    RCC_AHBRSTR |= RCC_AHBRSTR_ETHMACRST;
    for (volatile uint32_t spin = 0; spin < 2000u; spin++) {
    }
    RCC_AHBRSTR &= ~RCC_AHBRSTR_ETHMACRST;
    for (volatile uint32_t spin = 0; spin < 20000u; spin++) {
    }

    /* STM32F1 selects MII vs RMII in AFIO_MAPR bit 23 (MII_RMII_SEL). Without it the MAC
     * stays in MII mode and the RMII data path never works, even when MDIO is fine. */
    RCC_APB2ENR |= (1u << 0); /* AFIOEN */
#if ETH_MODE_MII
    AFIO_MAPR &= ~(1u << 23);
#else
    AFIO_MAPR |= (1u << 23); /* RMII */
#endif

    /* The PHY's X1 comes from PA8/MCO; start it before talking to the PHY. */
    if (clock_phy_mco(!ETH_MODE_MII) != 0) {
        return -2;
    }
    eth_pins();
    for (volatile uint32_t spin = 0; spin < 200000u; spin++) { /* ~3 ms settle */
    }

    /* Find the PHY: three identical reads at one address, not idle-bus and not zero. */
    for (uint8_t pa = 0; pa < 32u; pa++) {
        uint16_t id1 = mdio_read(pa, 2);
        uint16_t id2 = mdio_read(pa, 3);
        if (id1 != mdio_read(pa, 2) || id2 != mdio_read(pa, 3)) {
            continue;
        }
        if ((id1 == 0xFFFFu && id2 == 0xFFFFu) || (id1 == 0u && id2 == 0u)) {
            continue;
        }
        eth_last_status.phy_address = pa;
        eth_last_status.phy_id1 = id1;
        eth_last_status.phy_id2 = id2;
        break;
    }
    if (eth_last_status.phy_address == 0xFFu) {
        return -1;
    }

    /* DP83848: software reset, then auto-negotiation with all capabilities. */
    mdio_write(eth_last_status.phy_address, 0u, 0x8000u);
    for (volatile uint32_t spin = 0; spin < 200000u; spin++) {
        if ((mdio_read(eth_last_status.phy_address, 0u) & 0x8000u) == 0u) {
            break;
        }
    }
    /* DP83848 register 0x17 (RBR) bit 5 selects RMII; the board straps MII. Keep the PHY
     * mode in step with the MAC mode (AFIO_MAPR bit 23). */
    {
        uint16_t rbr = mdio_read(eth_last_status.phy_address, 0x17u);
#if ETH_MODE_MII
        rbr = (uint16_t)(rbr & ~0x0020u);
#else
        rbr = (uint16_t)(rbr | 0x0020u);
#endif
        mdio_write(eth_last_status.phy_address, 0x17u, rbr);
    }

    mdio_write(eth_last_status.phy_address, 4u, 0x01E1u); /* advertise everything */
    mdio_write(eth_last_status.phy_address, 0u, 0x1200u); /* AN enable + restart */

    /* MAC: enable transmitter/receiver, keep it simple until a link is known. */
    uint32_t maccr = ETH_MACCR;
    maccr &= ~((1u << 14) | (1u << 11)); /* FES = 0, DM = 0 for now */
    maccr |= (1u << 3) | (1u << 2);      /* TE | RE */
    ETH_MACCR = maccr;
    return 0;
}

int eth_poll_link(void) {
    if (eth_last_status.phy_address == 0xFFu) {
        return 0;
    }
    uint8_t pa = eth_last_status.phy_address;

    /* Re-arm auto-negotiation while the link stays down: switches that boot later, or a
     * cable plugged after start-up, otherwise leave the PHY waiting forever. */
    static uint32_t down_polls;
    if (down_polls < 0xFFFFFFFFu) {
        down_polls++;
    }
    if ((eth_last_status.link_up == 0u) && down_polls > 150u) { /* ~3 s at a 20 ms poll */
        mdio_write(pa, 0u, 0x1200u); /* AN enable + restart */
        down_polls = 0;
    }

    uint16_t bmsr = mdio_read(pa, 1u);
    if ((bmsr & 0xFFFFu) != 0xFFFFu) {
        eth_last_status.bmsr = bmsr;
    }
    /* Read BMSR twice: the link bit latches low, a second read gives the live value. */
    bmsr = mdio_read(pa, 1u);
    eth_last_status.bmsr = bmsr;
    eth_last_status.link_up = (uint8_t)((bmsr >> 2) & 1u);
    eth_last_status.autoneg_done = (uint8_t)((bmsr >> 5) & 1u);
    eth_last_status.physts = mdio_read(pa, 16u);
    eth_last_status.bmcr = mdio_read(pa, 0u);
    eth_last_status.rbr = mdio_read(pa, 0x17u);
    eth_last_status.phyctrl = mdio_read(pa, 0x19u);

    /* DP83848 PHYSTS bit 2 = speed (1 = 100 Mbps), bit 4 = duplex (1 = full). */
    eth_last_status.speed_100 = (uint8_t)((eth_last_status.physts >> 2) & 1u);
    eth_last_status.full_duplex = (uint8_t)((eth_last_status.physts >> 4) & 1u);

    if (eth_last_status.link_up) {
        uint32_t maccr = ETH_MACCR;
        maccr &= ~((1u << 14) | (1u << 11));
        if (eth_last_status.speed_100) {
            maccr |= (1u << 14); /* FES */
        }
        if (eth_last_status.full_duplex) {
            maccr |= (1u << 11); /* DM */
        }
        ETH_MACCR = maccr;
    }
    return eth_last_status.link_up;
}
