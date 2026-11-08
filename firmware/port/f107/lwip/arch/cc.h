/* lwIP architecture header for the RLY-8 firmware (bare metal, Cortex-M3).
 *
 * lwIP finds this as "arch/cc.h", so this directory (firmware/port/f107/lwip) must be on
 * the include path.
 */
#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

#include <stdint.h>

/* lwIP only pulls in arch/sys_arch.h for NO_SYS==0, but the SYS_LIGHTWEIGHT_PROT block in
 * lwip/sys.h uses sys_prot_t regardless. cc.h is always included, so define it here. */
#include "arch/sys_arch.h"

/* Byte order: STM32 is little-endian. */
#ifndef BYTE_ORDER
#define BYTE_ORDER LITTLE_ENDIAN
#endif

typedef uint8_t u8_t;
typedef int8_t s8_t;
typedef uint16_t u16_t;
typedef int16_t s16_t;
typedef uint32_t u32_t;
typedef int32_t s32_t;
typedef uintptr_t mem_ptr_t;

#define U16_F "hu"
#define S16_F "hd"
#define X16_F "hx"
#define U32_F "lu"
#define S32_F "ld"
#define X32_F "lx"
#define SZT_F "lu"

#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_STRUCT __attribute__((packed))
#define PACK_STRUCT_END
#define PACK_STRUCT_FIELD(x) x

/* No stdio on the target; diagnostics are compiled out (LWIP_DEBUG 0 in lwipopts.h). */
#define LWIP_PLATFORM_DIAG(x)                                                                     \
    do {                                                                                          \
        (void)0;                                                                                  \
    } while (0)
#define LWIP_PLATFORM_ASSERT(x)                                                                   \
    do {                                                                                          \
        (void)(x);                                                                                \
    } while (0)

/* DNS query IDs / source ports; not cryptographic (xorshift seeded from UID + SysTick). */
uint32_t rly_lwip_rand(void);
#define LWIP_RAND() rly_lwip_rand()

#endif /* LWIP_ARCH_CC_H */
