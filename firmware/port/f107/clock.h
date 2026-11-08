/* Clock bring-up for the RLY-8 board: HSE 25 MHz -> PREDIV2/5 -> PLL2 x8 -> PREDIV1/5 -> PLL x9 -> 72 MHz.
 *
 * The HSI/2 path was tried on this part and the PLL never locked, so HSE is the only
 * supported source here.
 */
#ifndef CLOCK_H
#define CLOCK_H

#include <stdint.h>

/* Returns 0 on success, negative when HSE or the PLL fails to lock. */
int clock_init_72mhz(void);

/* Drive the PHY reference clock on PA8/MCO: 25 MHz (MII) or 50 MHz from PLL3 (RMII). */
int clock_phy_mco(int rmii);

/* Last status bits captured during init, for diagnostics over SWD. */
typedef struct {
    uint32_t hse_ready;
    uint32_t pll_ready;
    uint32_t switched_to_pll;
} clock_status;

extern volatile clock_status clock_last_status;

#endif /* CLOCK_H */
