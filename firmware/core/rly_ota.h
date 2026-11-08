/* Firmware update (OTA) logic shared by the application and the bootloader.
 *
 * Flash map (STM32F107RC, 256 KB, 2 KB pages):
 *   0x08000000  8 KB    bootloader
 *   0x08002000  120 KB  application slot (linked here)
 *   0x08020000  120 KB  staging slot (new image is streamed here by the web panel)
 *   0x0803E000  2 KB    OTA control page (rly_ota_ctrl)
 *   0x0803E800  2 KB    reserved
 *   0x0803F000  4 KB    configuration pages A/B (flash_cfg.c)
 *
 * Update flow: the app writes the staging slot and, only after the CRC and the image header
 * check pass, a control record "pending". The bootloader re-checks the staging CRC, copies
 * staging -> application, verifies the copy and then erases the control page. A power cut
 * during the copy leaves "pending" in place, so the copy simply runs again.
 *
 * Nothing here touches hardware: flash access goes through rly_flash_ops, so the whole flow
 * (including interrupted installs) is tested on the host.
 */
#ifndef RLY_OTA_H
#define RLY_OTA_H

#include <stddef.h>
#include <stdint.h>

#define RLY_FLASH_BASE 0x08000000u
#define RLY_FLASH_PAGE 2048u
#define RLY_BOOT_SIZE 0x2000u
#define RLY_APP_BASE 0x08002000u
#define RLY_SLOT_SIZE 0x1E000u /* 120 KB */
#define RLY_STAGING_BASE 0x08020000u
#define RLY_OTA_CTRL_ADDR 0x0803E000u

#define RLY_RAM_BASE 0x20000000u
#define RLY_RAM_END 0x20010000u

/* Image header placed by the linker at a fixed offset after the vector table. */
#define RLY_APP_INFO_OFFSET 0x200u
#define RLY_APP_INFO_MAGIC 0x38594C52u /* "RLY8" */
#define RLY_APP_INFO_VERSION 1u

typedef struct {
    uint32_t magic;
    uint32_t info_version;
    uint32_t image_end; /* link address one past the last byte of the .bin */
    uint32_t reserved;
    char fw_version[16];
} rly_app_info;

#define RLY_OTA_CTRL_MAGIC 0x5041544Fu /* "OTAP" */
#define RLY_OTA_PENDING 0x444E4550u    /* "PEND" */

typedef struct {
    uint32_t magic;
    uint32_t state;
    uint32_t size;
    uint32_t crc; /* CRC-32 of the staged image */
    uint32_t hdr_crc; /* CRC-32 of the four fields above */
} rly_ota_ctrl;

typedef enum {
    RLY_IMG_OK = 0,
    RLY_IMG_TOO_SMALL = -1,
    RLY_IMG_TOO_BIG = -2,
    RLY_IMG_BAD_SP = -3,
    RLY_IMG_BAD_RESET = -4,
    RLY_IMG_BAD_MAGIC = -5, /* not an RLY-8 application image (e.g. a v1 or "full" image) */
    RLY_IMG_BAD_SIZE = -6,  /* truncated or padded file */
    RLY_IMG_BAD_CRC = -7,
    RLY_IMG_IO = -8
} rly_img_status;

/* Validates a complete application image linked at `base`. */
rly_img_status rly_image_check(const uint8_t *img, uint32_t len, uint32_t base);
/* Minimal sanity check of an installed application (vectors only; length unknown). */
int rly_app_vectors_ok(const uint8_t *app, uint32_t base, uint32_t slot_size);
/* Copies the version string from an image header ("" when absent). */
void rly_image_version(const uint8_t *img, uint32_t len, char *out, size_t cap);
const char *rly_img_status_text(rly_img_status s);

/* Flash access. program() always gets an even length and a half-word aligned address. */
typedef struct {
    int (*erase_page)(void *ctx, uint32_t addr);
    int (*program)(void *ctx, uint32_t addr, const uint8_t *data, uint32_t len);
    const uint8_t *(*map)(void *ctx, uint32_t addr); /* memory-mapped read pointer */
    void *ctx;
} rly_flash_ops;

/* ---- streaming writer (application side) ---- */

#define RLY_OTA_HEAD_KEEP (RLY_APP_INFO_OFFSET + sizeof(rly_app_info))
#define RLY_OTA_CHUNK 64u

typedef struct {
    rly_flash_ops ops;
    uint32_t dest;  /* staging base */
    uint32_t cap;   /* staging size */
    uint32_t total; /* announced length */
    uint32_t off;   /* bytes accepted so far */
    uint32_t erased_end;
    uint32_t crc_state;
    uint32_t buf_len;
    uint8_t buf[RLY_OTA_CHUNK];
    uint8_t head[RLY_OTA_HEAD_KEEP]; /* first bytes, for the header check at the end */
    rly_img_status error;
} rly_ota_writer;

rly_img_status rly_ota_begin(rly_ota_writer *w, const rly_flash_ops *ops, uint32_t dest,
                             uint32_t cap, uint32_t total);
rly_img_status rly_ota_write(rly_ota_writer *w, const uint8_t *data, uint32_t n);
/* Flushes, checks length, header and CRC of what landed in flash; fills ctrl on success. */
rly_img_status rly_ota_finish(rly_ota_writer *w, uint32_t link_base, rly_ota_ctrl *ctrl);

void rly_ota_ctrl_seal(rly_ota_ctrl *c);
int rly_ota_ctrl_pending(const rly_ota_ctrl *c);

/* ---- bootloader side ---- */

typedef enum {
    RLY_BOOT_RUN_APP = 0,  /* nothing pending, application looks sane */
    RLY_BOOT_INSTALL = 1,  /* valid pending image in staging */
    RLY_BOOT_DISCARD = 2,  /* pending record but staging is corrupt: clear it, run the app */
    RLY_BOOT_NO_APP = 3    /* nothing to run */
} rly_boot_action;

rly_boot_action rly_boot_decide(const rly_flash_ops *ops);
/* Copies staging -> application and verifies it; 0 on success. Does not clear the ctrl page. */
int rly_boot_install(const rly_flash_ops *ops, const rly_ota_ctrl *ctrl);

#endif /* RLY_OTA_H */
