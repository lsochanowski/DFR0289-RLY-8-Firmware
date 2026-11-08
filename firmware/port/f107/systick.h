/* 1 ms SysTick for lwIP's sys_now() (NO_SYS=1) and for retry timers.
 *
 * The core runs at 72 MHz (see clock.c), so a 1 ms period is LOAD = 72000 - 1.
 */
#ifndef SYSTICK_H
#define SYSTICK_H

#include <stdint.h>

void systick_init(void);

/* Milliseconds since boot; this is lwIP's sys_now() backend. */
uint32_t systick_ms(void);

#endif /* SYSTICK_H */
