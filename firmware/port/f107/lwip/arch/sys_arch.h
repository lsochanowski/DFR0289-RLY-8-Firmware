/* lwIP architecture: critical-section type for a bare-metal Cortex-M3.
 *
 * lwIP finds this as "arch/sys_arch.h" (directory firmware/port/f107/lwip on the include
 * path). sys_prot_t carries the saved PRIMASK so nesting works.
 */
#ifndef LWIP_ARCH_SYS_ARCH_H
#define LWIP_ARCH_SYS_ARCH_H

#include <stdint.h>

typedef uint32_t sys_prot_t;

#endif /* LWIP_ARCH_SYS_ARCH_H */
