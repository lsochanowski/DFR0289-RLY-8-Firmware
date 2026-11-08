#include "flash_cfg.h"

#include <stdint.h>
#include <string.h>

#define REG(addr) (*(volatile uint32_t *)(addr))

/* RM0008 §3.3 / PM0075: connectivity line, 2 KB pages. */
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

#define PAGE_A 0x0803F000u
#define PAGE_B 0x0803F800u
#define PAGE_SIZE 2048u

_Static_assert(sizeof(rly_config) <= PAGE_SIZE, "config must fit one flash page");
_Static_assert(sizeof(rly_config) % 2u == 0u, "flash is programmed in half-words");

static const rly_config *page(uint32_t addr) { return (const rly_config *)(uintptr_t)addr; }

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

static int erase_page(uint32_t addr) {
    if (wait_idle() != 0) {
        return -1;
    }
    FLASH_CR |= FLASH_CR_PER;
    FLASH_AR = addr;
    FLASH_CR |= FLASH_CR_STRT;
    int r = wait_idle();
    FLASH_CR &= ~FLASH_CR_PER;
    return r;
}

static int program(uint32_t addr, const void *data, uint32_t len) {
    const uint16_t *src = (const uint16_t *)data;
    FLASH_CR |= FLASH_CR_PG;
    for (uint32_t i = 0; i < len / 2u; i++) {
        *(volatile uint16_t *)(uintptr_t)(addr + 2u * i) = src[i];
        if (wait_idle() != 0) {
            FLASH_CR &= ~FLASH_CR_PG;
            return -1;
        }
    }
    FLASH_CR &= ~FLASH_CR_PG;
    return 0;
}

/* Returns the address of the newest valid copy, or 0. */
static uint32_t newest(void) {
    int a = rly_config_valid(page(PAGE_A));
    int b = rly_config_valid(page(PAGE_B));
    if (a && b) {
        /* signed difference handles seq wrap-around */
        return (int32_t)(page(PAGE_A)->seq - page(PAGE_B)->seq) >= 0 ? PAGE_A : PAGE_B;
    }
    return a ? PAGE_A : (b ? PAGE_B : 0u);
}

int flash_cfg_load(rly_config *cfg) {
    uint32_t addr = newest();
    if (addr == 0u) {
        rly_config_defaults(cfg);
        return 0;
    }
    memcpy(cfg, page(addr), sizeof *cfg);
    return 1;
}

int flash_cfg_save(rly_config *cfg) {
    uint32_t cur = newest();
    uint32_t target = (cur == PAGE_A) ? PAGE_B : PAGE_A;
    cfg->seq = (cur != 0u) ? page(cur)->seq + 1u : 1u;
    rly_config_seal(cfg);

    static rly_config copy; /* aligned RAM copy; cfg may be a packed field elsewhere */
    copy = *cfg;

    unlock();
    int r = erase_page(target);
    if (r == 0) {
        r = program(target, &copy, sizeof copy);
    }
    lock();
    if (r == 0 && memcmp(page(target), &copy, sizeof copy) != 0) {
        r = -1;
    }
    return r;
}

void flash_cfg_erase(void) {
    unlock();
    (void)erase_page(PAGE_A);
    (void)erase_page(PAGE_B);
    lock();
}
