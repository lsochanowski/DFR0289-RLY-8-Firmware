#include "systick.h"

#define REG(addr) (*(volatile uint32_t *)(addr))

#define SYST_CSR REG(0xE000E010u)
#define SYST_RVR REG(0xE000E014u)
#define SYST_CVR REG(0xE000E018u)

#define SYST_CSR_ENABLE (1u << 0)
#define SYST_CSR_TICKINT (1u << 1)
#define SYST_CSR_CLKSOURCE (1u << 2) /* 1 = processor clock */

#define SYSTICK_1MS_RELOAD 72000u /* 72 MHz / 1000 */

static volatile uint32_t ms_ticks;

void systick_init(void) {
    ms_ticks = 0;
    SYST_RVR = SYSTICK_1MS_RELOAD - 1u;
    SYST_CVR = 0;
    SYST_CSR = SYST_CSR_CLKSOURCE | SYST_CSR_TICKINT | SYST_CSR_ENABLE;
}

uint32_t systick_ms(void) { return ms_ticks; }

/* Installed as exception 15 in the vector table (app/startup.c). */
void SysTick_Handler(void) { ms_ticks++; }

/* lwIP (NO_SYS=1) calls sys_now() for its timers. */
uint32_t sys_now(void) { return ms_ticks; }
