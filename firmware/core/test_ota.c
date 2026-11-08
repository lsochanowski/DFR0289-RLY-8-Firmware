/* Host tests for the OTA writer and the bootloader decision/install logic. */
#include <stdio.h>
#include <string.h>

#include "rly_crc.h"
#include "rly_ota.h"

static int checks, failures;

static void expect_int(const char *name, long got, long want) {
    checks++;
    if (got != want) {
        failures++;
        printf("FAIL %s: got %ld want %ld\n", name, got, want);
    } else {
        printf("ok   %s\n", name);
    }
}

/* ---- fake STM32F1 flash: erase -> 0xFF, program only onto erased half-words ---- */
#define FLASH_SIZE 0x40000u
static uint8_t flash[FLASH_SIZE];
static long ops_left = -1; /* >= 0: fail ("power cut") after this many operations */

static int tick(void) {
    if (ops_left == 0) {
        return -1;
    }
    if (ops_left > 0) {
        ops_left--;
    }
    return 0;
}

static int f_erase(void *ctx, uint32_t addr) {
    (void)ctx;
    if (tick() != 0 || addr < RLY_FLASH_BASE || (addr - RLY_FLASH_BASE) % RLY_FLASH_PAGE) {
        return -1;
    }
    memset(flash + (addr - RLY_FLASH_BASE), 0xFF, RLY_FLASH_PAGE);
    return 0;
}

static int f_program(void *ctx, uint32_t addr, const uint8_t *d, uint32_t n) {
    (void)ctx;
    if (tick() != 0 || (n & 1u) || (addr & 1u)) {
        return -1;
    }
    uint8_t *p = flash + (addr - RLY_FLASH_BASE);
    for (uint32_t i = 0; i < n; i += 2) {
        if (p[i] != 0xFF || p[i + 1] != 0xFF) {
            return -1; /* PGERR: not erased */
        }
        p[i] = d[i];
        p[i + 1] = d[i + 1];
    }
    return 0;
}

static const uint8_t *f_map(void *ctx, uint32_t addr) {
    (void)ctx;
    return flash + (addr - RLY_FLASH_BASE);
}

static const rly_flash_ops OPS = {f_erase, f_program, f_map, NULL};

/* Builds a plausible application image linked at `base`. */
static uint32_t make_image(uint8_t *img, uint32_t len, uint32_t base, const char *ver, uint8_t fill) {
    memset(img, fill, len);
    uint32_t sp = 0x20010000u, rv = base + 0x401u;
    memcpy(img, &sp, 4);
    memcpy(img + 4, &rv, 4);
    rly_app_info info = {RLY_APP_INFO_MAGIC, RLY_APP_INFO_VERSION, base + len, 0, {0}};
    strncpy(info.fw_version, ver, sizeof info.fw_version - 1);
    memcpy(img + RLY_APP_INFO_OFFSET, &info, sizeof info);
    return len;
}

static rly_img_status stream(const uint8_t *img, uint32_t len, uint32_t chunk, rly_ota_ctrl *c) {
    rly_ota_writer w;
    rly_img_status st = rly_ota_begin(&w, &OPS, RLY_STAGING_BASE, RLY_SLOT_SIZE, len);
    for (uint32_t off = 0; st == RLY_IMG_OK && off < len; off += chunk) {
        st = rly_ota_write(&w, img + off, off + chunk > len ? len - off : chunk);
    }
    return st != RLY_IMG_OK ? st : rly_ota_finish(&w, RLY_APP_BASE, c);
}

static void put_ctrl(const rly_ota_ctrl *c) {
    f_erase(NULL, RLY_OTA_CTRL_ADDR);
    f_program(NULL, RLY_OTA_CTRL_ADDR, (const uint8_t *)c, sizeof *c);
}

static uint8_t img[70001];
static uint8_t old_img[60000];

int main(void) {
    memset(flash, 0xFF, sizeof flash);
    rly_ota_ctrl ctrl;

    /* installed "old" application */
    make_image(old_img, sizeof old_img, RLY_APP_BASE, "1.9.0", 0x11);
    memcpy(flash + (RLY_APP_BASE - RLY_FLASH_BASE), old_img, sizeof old_img);
    expect_int("boot: run app", rly_boot_decide(&OPS), RLY_BOOT_RUN_APP);

    /* image checks */
    uint32_t len = make_image(img, sizeof img, RLY_APP_BASE, "2.0.0", 0x5A); /* odd length */
    expect_int("image ok", rly_image_check(img, len, RLY_APP_BASE), RLY_IMG_OK);
    expect_int("wrong link base rejected", rly_image_check(img, len, RLY_FLASH_BASE) != RLY_IMG_OK, 1);
    expect_int("truncated", rly_image_check(img, len - 100, RLY_APP_BASE), RLY_IMG_BAD_SIZE);
    char ver[16];
    rly_image_version(img, len, ver, sizeof ver);
    expect_int("version string", strcmp(ver, "2.0.0"), 0);
    uint8_t junk[1024];
    memset(junk, 0x42, sizeof junk);
    expect_int("random file rejected", rly_image_check(junk, sizeof junk, RLY_APP_BASE) != RLY_IMG_OK, 1);
    expect_int("too big", rly_image_check(img, RLY_SLOT_SIZE + 2, RLY_APP_BASE), RLY_IMG_TOO_BIG);

    /* streaming with awkward chunk sizes (odd, smaller and larger than the internal chunk) */
    const uint32_t chunks[] = {1, 7, 64, 1460, 4097};
    for (size_t i = 0; i < sizeof chunks / sizeof chunks[0]; i++) {
        rly_img_status st = stream(img, len, chunks[i], &ctrl);
        char name[48];
        snprintf(name, sizeof name, "stream chunk %u", (unsigned)chunks[i]);
        expect_int(name, st, RLY_IMG_OK);
    }
    expect_int("staging matches", memcmp(flash + (RLY_STAGING_BASE - RLY_FLASH_BASE), img, len), 0);
    expect_int("ctrl crc", (long)ctrl.crc, (long)rly_crc32(img, len));
    expect_int("ctrl pending", rly_ota_ctrl_pending(&ctrl), 1);
    expect_int("app untouched by upload",
               memcmp(flash + (RLY_APP_BASE - RLY_FLASH_BASE), old_img, sizeof old_img), 0);

    /* writer rejects bad input */
    rly_ota_writer w;
    expect_int("begin too big", rly_ota_begin(&w, &OPS, RLY_STAGING_BASE, RLY_SLOT_SIZE, RLY_SLOT_SIZE + 1),
               RLY_IMG_TOO_BIG);
    rly_ota_begin(&w, &OPS, RLY_STAGING_BASE, RLY_SLOT_SIZE, len);
    rly_ota_write(&w, img, 1000);
    expect_int("short upload", rly_ota_finish(&w, RLY_APP_BASE, &ctrl), RLY_IMG_BAD_SIZE);
    rly_ota_begin(&w, &OPS, RLY_STAGING_BASE, RLY_SLOT_SIZE, 10);
    expect_int("overlong upload", rly_ota_write(&w, img, 20) != RLY_IMG_OK, 1);
    uint8_t v1[4096];
    make_image(v1, sizeof v1, RLY_FLASH_BASE, "1.0.0", 0);
    expect_int("v1 image rejected", stream(v1, sizeof v1, 512, &ctrl) != RLY_IMG_OK, 1);
    ops_left = 3;
    expect_int("flash error surfaces", stream(img, len, 1460, &ctrl), RLY_IMG_IO);
    ops_left = -1;

    /* nothing pending yet -> still runs the old app */
    expect_int("no ctrl -> run app", rly_boot_decide(&OPS), RLY_BOOT_RUN_APP);

    /* pending + good staging -> install */
    expect_int("re-stream", stream(img, len, 1460, &ctrl), RLY_IMG_OK);
    put_ctrl(&ctrl);
    expect_int("boot: install", rly_boot_decide(&OPS), RLY_BOOT_INSTALL);

    /* power cut in the middle of the copy, then a clean retry */
    ops_left = 20;
    expect_int("install interrupted", rly_boot_install(&OPS, &ctrl), -1);
    ops_left = -1;
    expect_int("still pending after cut", rly_boot_decide(&OPS), RLY_BOOT_INSTALL);
    expect_int("install retry", rly_boot_install(&OPS, &ctrl), 0);
    expect_int("app = new image", memcmp(flash + (RLY_APP_BASE - RLY_FLASH_BASE), img, len), 0);
    f_erase(NULL, RLY_OTA_CTRL_ADDR); /* bootloader clears the record */
    expect_int("after install: run app", rly_boot_decide(&OPS), RLY_BOOT_RUN_APP);

    /* pending record but staging corrupted -> discard */
    stream(img, len, 1460, &ctrl);
    put_ctrl(&ctrl);
    flash[RLY_STAGING_BASE - RLY_FLASH_BASE + 5000] ^= 0x01;
    expect_int("corrupt staging -> discard", rly_boot_decide(&OPS), RLY_BOOT_DISCARD);
    f_erase(NULL, RLY_OTA_CTRL_ADDR);

    /* tampered control record is ignored */
    stream(img, len, 1460, &ctrl);
    ctrl.size += 2;
    put_ctrl(&ctrl);
    expect_int("bad ctrl crc ignored", rly_boot_decide(&OPS), RLY_BOOT_RUN_APP);
    f_erase(NULL, RLY_OTA_CTRL_ADDR);

    /* empty application slot */
    for (uint32_t a = RLY_APP_BASE; a < RLY_APP_BASE + RLY_SLOT_SIZE; a += RLY_FLASH_PAGE) {
        f_erase(NULL, a);
    }
    expect_int("erased app -> no app", rly_boot_decide(&OPS), RLY_BOOT_NO_APP);

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
