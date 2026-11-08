/* Ethernet MAC + DP83848AA PHY bring-up for the RLY-8 board (STM32F107RC).
 *
 * The PHY has no crystal: X1 is fed from PA8/MCO (25 MHz HSE for MII, 50 MHz PLL3 for
 * RMII). The PHY straps MII (RBR=0x0001 after reset), so MII is the default.
 * Build with -DETH_MODE_MII=0 for the RMII variant.
 */
#ifndef ETH_H
#define ETH_H

#include <stdint.h>

#ifndef ETH_MODE_MII
#define ETH_MODE_MII 1
#endif

typedef struct {
    uint8_t phy_address; /* 0xFF when nothing answered */
    uint8_t link_up;
    uint8_t speed_100;
    uint8_t full_duplex;
    uint8_t autoneg_done;
    uint16_t phy_id1;
    uint16_t phy_id2;
    uint16_t bmsr;
    uint16_t physts; /* DP83848 register 0x10 */
    uint16_t bmcr;   /* register 0x00: bit11 = power-down, bit10 = isolate */
    uint16_t rbr;    /* DP83848 register 0x17: bit5 = RMII mode */
    uint16_t phyctrl;/* DP83848 register 0x19 */
} eth_status;

extern volatile eth_status eth_last_status;

/* Enables clocks, resets the MAC, configures the PHY pins, finds the PHY and starts
 * auto-negotiation. Returns 0 when a PHY was found and configured. */
int eth_init(void);

/* Reads BMSR/PHYSTS into eth_last_status. Returns 1 when the link is up. */
int eth_poll_link(void);

#endif /* ETH_H */
