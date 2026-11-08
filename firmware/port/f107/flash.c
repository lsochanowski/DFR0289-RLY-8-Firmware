#include "flash.h"

#define REG(addr) (*(volatile uint32_t *)(addr))

#define FLASH_KEYR REG(0x40022004u)
#define FLASH_SR REG(0x4002200Cu)
#define FLASH_CR REG(0x40022010u)
#define FLASH_AR REG(0x40022014u)

#define FLASH_SR_BSY (1u << 0)
#define FLASH_SR_PGERR (1u << 2)
#define FLASH_SR_WRPRTERR (1u << 4)
#define FLASH_SR_EOP (1u << 5)
#define FLASH_CR_PG (1u << 0)
#define FLASH_CR_PER (1u << 1)
#define FLASH_CR_STRT (1u << 6)
#define FLASH_CR_LOCK (1u << 7)

static int wait_idle(void) {
    for (uint32_t spin = 0; spin < 2000000u; spin++) {
        if ((FLASH_SR & FLASH_SR_BSY) == 0u) {
            uint32_t sr = FLASH_SR;
            FLASH_SR = FLASH_SR_EOP | FLASH_SR_PGERR | FLASH_SR_WRPRTERR; /* write-1-to-clear */
            return (sr & (FLASH_SR_PGERR | FLASH_SR_WRPRTERR)) ? -1 : 0;
        }
    }
    return -1;
}

static void unlock(void) {
    if (FLASH_CR & FLASH_CR_LOCK) {
        FLASH_KEYR = 0x45670123u;
        FLASH_KEYR = 0xCDEF89ABu;
    }
}

static void lock(void) { FLASH_CR |= FLASH_CR_LOCK; }

int flash_erase_page(uint32_t addr) {
    unlock();
    int r = wait_idle();
    if (r == 0) {
        FLASH_CR |= FLASH_CR_PER;
        FLASH_AR = addr;
        FLASH_CR |= FLASH_CR_STRT;
        r = wait_idle();
        FLASH_CR &= ~FLASH_CR_PER;
    }
    lock();
    return r;
}

int flash_program(uint32_t addr, const uint8_t *data, uint32_t len) {
    if ((addr & 1u) || (len & 1u)) {
        return -1;
    }
    unlock();
    int r = wait_idle();
    FLASH_CR |= FLASH_CR_PG;
    for (uint32_t i = 0; r == 0 && i < len; i += 2u) {
        uint16_t hw = (uint16_t)(data[i] | ((uint16_t)data[i + 1u] << 8));
        *(volatile uint16_t *)(uintptr_t)(addr + i) = hw;
        r = wait_idle();
        if (r == 0 && *(volatile uint16_t *)(uintptr_t)(addr + i) != hw) {
            r = -1;
        }
    }
    FLASH_CR &= ~FLASH_CR_PG;
    lock();
    return r;
}

static int ops_erase(void *ctx, uint32_t addr) {
    (void)ctx;
    return flash_erase_page(addr);
}

static int ops_program(void *ctx, uint32_t addr, const uint8_t *data, uint32_t len) {
    (void)ctx;
    return flash_program(addr, data, len);
}

static const uint8_t *ops_map(void *ctx, uint32_t addr) {
    (void)ctx;
    return (const uint8_t *)(uintptr_t)addr;
}

const rly_flash_ops flash_ops = {ops_erase, ops_program, ops_map, 0};
