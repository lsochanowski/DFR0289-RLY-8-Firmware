#include "flash_cfg.h"

#include <stdint.h>
#include <string.h>

#include "flash.h"

#define PAGE_A 0x0803F000u
#define PAGE_B 0x0803F800u
#define PAGE_SIZE 2048u

_Static_assert(sizeof(rly_config) <= PAGE_SIZE, "config must fit one flash page");
_Static_assert(sizeof(rly_config) % 2u == 0u, "flash is programmed in half-words");

static const rly_config *page(uint32_t addr) { return (const rly_config *)(uintptr_t)addr; }

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

    int r = flash_erase_page(target);
    if (r == 0) {
        r = flash_program(target, (const uint8_t *)&copy, sizeof copy);
    }
    if (r == 0 && memcmp(page(target), &copy, sizeof copy) != 0) {
        r = -1;
    }
    return r;
}

void flash_cfg_erase(void) {
    (void)flash_erase_page(PAGE_A);
    (void)flash_erase_page(PAGE_B);
}
