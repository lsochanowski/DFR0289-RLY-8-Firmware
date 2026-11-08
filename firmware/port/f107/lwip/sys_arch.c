/* lwIP critical sections on bare-metal Cortex-M3: mask interrupts, restore PRIMASK.
 *
 * The firmware is single-threaded (NO_SYS=1) and calls lwIP from the main loop, but RX/TX
 * may still be touched from the Ethernet interrupt, so real masking is worth the few cycles.
 */
#include "lwip/opt.h"
#include "lwip/sys.h"

#include "arch/sys_arch.h"

sys_prot_t sys_arch_protect(void) {
    uint32_t primask;
    __asm volatile("mrs %0, primask" : "=r"(primask));
    __asm volatile("cpsid i" ::: "memory");
    return primask;
}

void sys_arch_unprotect(sys_prot_t pval) {
    __asm volatile("msr primask, %0" ::"r"(pval) : "memory");
}

uint32_t rly_lwip_rand(void) {
    static uint32_t x;
    if (x == 0u) {
        x = *(const volatile uint32_t *)0x1FFFF7E8u ^ *(const volatile uint32_t *)0x1FFFF7F0u;
    }
    x ^= *(volatile uint32_t *)0xE000E018u; /* SysTick VAL: timing jitter */
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    if (x == 0u) {
        x = 0x9E3779B9u;
    }
    return x;
}
