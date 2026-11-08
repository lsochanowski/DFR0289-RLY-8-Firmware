#include "rly_ota.h"

#include <string.h>

#include "rly_crc.h"

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int rly_app_vectors_ok(const uint8_t *app, uint32_t base, uint32_t slot_size) {
    uint32_t sp = rd32(app);
    uint32_t rv = rd32(app + 4);
    if (sp <= RLY_RAM_BASE || sp > RLY_RAM_END || (sp & 3u) != 0u) {
        return 0;
    }
    if ((rv & 1u) == 0u || rv - 1u < base || rv - 1u >= base + slot_size) {
        return 0;
    }
    return 1;
}

rly_img_status rly_image_check(const uint8_t *img, uint32_t len, uint32_t base) {
    if (len < RLY_OTA_HEAD_KEEP) {
        return RLY_IMG_TOO_SMALL;
    }
    if (len > RLY_SLOT_SIZE) {
        return RLY_IMG_TOO_BIG;
    }
    uint32_t sp = rd32(img);
    if (sp <= RLY_RAM_BASE || sp > RLY_RAM_END || (sp & 3u) != 0u) {
        return RLY_IMG_BAD_SP;
    }
    uint32_t rv = rd32(img + 4);
    if ((rv & 1u) == 0u || rv - 1u < base || rv - 1u >= base + len) {
        return RLY_IMG_BAD_RESET;
    }
    const uint8_t *info = img + RLY_APP_INFO_OFFSET;
    if (rd32(info) != RLY_APP_INFO_MAGIC || rd32(info + 4) != RLY_APP_INFO_VERSION) {
        return RLY_IMG_BAD_MAGIC;
    }
    if (rd32(info + 8) != base + len) {
        return RLY_IMG_BAD_SIZE;
    }
    return RLY_IMG_OK;
}

void rly_image_version(const uint8_t *img, uint32_t len, char *out, size_t cap) {
    if (cap == 0) {
        return;
    }
    out[0] = '\0';
    if (len < RLY_OTA_HEAD_KEEP || rd32(img + RLY_APP_INFO_OFFSET) != RLY_APP_INFO_MAGIC) {
        return;
    }
    const char *v = (const char *)img + RLY_APP_INFO_OFFSET + offsetof(rly_app_info, fw_version);
    size_t n = 0;
    while (n < 15 && n + 1 < cap && v[n] >= 0x20 && v[n] < 0x7F) {
        out[n] = v[n];
        n++;
    }
    out[n] = '\0';
}

const char *rly_img_status_text(rly_img_status s) {
    switch (s) {
    case RLY_IMG_OK:
        return "ok";
    case RLY_IMG_TOO_SMALL:
        return "plik za mały";
    case RLY_IMG_TOO_BIG:
        return "plik za duży";
    case RLY_IMG_BAD_SP:
    case RLY_IMG_BAD_RESET:
    case RLY_IMG_BAD_MAGIC:
        return "to nie jest plik aktualizacji RLY-8 (użyj *-update.bin)";
    case RLY_IMG_BAD_SIZE:
        return "plik niekompletny albo zmieniony";
    case RLY_IMG_BAD_CRC:
        return "błąd sumy kontrolnej po zapisie";
    default:
        return "błąd zapisu flash";
    }
}

/* ---------------------------------------------------------------- writer */

rly_img_status rly_ota_begin(rly_ota_writer *w, const rly_flash_ops *ops, uint32_t dest,
                             uint32_t cap, uint32_t total) {
    memset(w, 0, sizeof *w);
    w->ops = *ops;
    w->dest = dest;
    w->cap = cap;
    w->total = total;
    w->erased_end = dest;
    w->crc_state = RLY_CRC32_INIT;
    if (total < RLY_OTA_HEAD_KEEP) {
        w->error = RLY_IMG_TOO_SMALL;
    } else if (total > cap) {
        w->error = RLY_IMG_TOO_BIG;
    }
    return w->error;
}

/* Programs buf (even length, or the padded final chunk). */
static rly_img_status flush(rly_ota_writer *w, int final) {
    if (w->buf_len == 0u) {
        return RLY_IMG_OK;
    }
    uint32_t n = w->buf_len;
    if (n & 1u) {
        if (!final) {
            return RLY_IMG_OK; /* keep the odd byte for the next call */
        }
        w->buf[n++] = 0xFFu;
    }
    uint32_t addr = w->dest + (w->off - w->buf_len);
    while (w->erased_end < addr + n) {
        if (w->ops.erase_page(w->ops.ctx, w->erased_end) != 0) {
            return w->error = RLY_IMG_IO;
        }
        w->erased_end += RLY_FLASH_PAGE;
    }
    if (w->ops.program(w->ops.ctx, addr, w->buf, n) != 0) {
        return w->error = RLY_IMG_IO;
    }
    w->buf_len = 0;
    return RLY_IMG_OK;
}

rly_img_status rly_ota_write(rly_ota_writer *w, const uint8_t *data, uint32_t n) {
    if (w->error != RLY_IMG_OK) {
        return w->error;
    }
    if (w->off + n > w->total) {
        return w->error = RLY_IMG_BAD_SIZE;
    }
    w->crc_state = rly_crc32_update(w->crc_state, data, n);
    for (uint32_t i = 0; i < n; i++) {
        if (w->off < RLY_OTA_HEAD_KEEP) {
            w->head[w->off] = data[i];
        }
        w->buf[w->buf_len++] = data[i];
        w->off++;
        if (w->buf_len == RLY_OTA_CHUNK) {
            if (flush(w, 0) != RLY_IMG_OK) {
                return w->error;
            }
        }
    }
    return RLY_IMG_OK;
}

void rly_ota_ctrl_seal(rly_ota_ctrl *c) { c->hdr_crc = rly_crc32(c, offsetof(rly_ota_ctrl, hdr_crc)); }

int rly_ota_ctrl_pending(const rly_ota_ctrl *c) {
    return c->magic == RLY_OTA_CTRL_MAGIC && c->state == RLY_OTA_PENDING &&
           c->hdr_crc == rly_crc32(c, offsetof(rly_ota_ctrl, hdr_crc)) && c->size >= 8u &&
           c->size <= RLY_SLOT_SIZE;
}

rly_img_status rly_ota_finish(rly_ota_writer *w, uint32_t link_base, rly_ota_ctrl *ctrl) {
    if (w->error != RLY_IMG_OK) {
        return w->error;
    }
    if (w->off != w->total) {
        return w->error = RLY_IMG_BAD_SIZE;
    }
    if (flush(w, 1) != RLY_IMG_OK) {
        return w->error;
    }
    rly_img_status st = rly_image_check(w->head, w->total, link_base);
    if (st != RLY_IMG_OK) {
        /* the header check only needs the first bytes; pass the real length */
        return w->error = st;
    }
    uint32_t crc = ~w->crc_state;
    const uint8_t *flash = w->ops.map(w->ops.ctx, w->dest);
    if (flash == NULL || rly_crc32(flash, w->total) != crc) {
        return w->error = RLY_IMG_BAD_CRC;
    }
    memset(ctrl, 0, sizeof *ctrl);
    ctrl->magic = RLY_OTA_CTRL_MAGIC;
    ctrl->state = RLY_OTA_PENDING;
    ctrl->size = w->total;
    ctrl->crc = crc;
    rly_ota_ctrl_seal(ctrl);
    return RLY_IMG_OK;
}

/* ---------------------------------------------------------------- bootloader */

rly_boot_action rly_boot_decide(const rly_flash_ops *ops) {
    const rly_ota_ctrl *c = (const rly_ota_ctrl *)(const void *)ops->map(ops->ctx, RLY_OTA_CTRL_ADDR);
    if (c != NULL && rly_ota_ctrl_pending(c)) {
        const uint8_t *staging = ops->map(ops->ctx, RLY_STAGING_BASE);
        if (staging != NULL && rly_crc32(staging, c->size) == c->crc &&
            rly_image_check(staging, c->size, RLY_APP_BASE) == RLY_IMG_OK) {
            return RLY_BOOT_INSTALL;
        }
        return RLY_BOOT_DISCARD;
    }
    const uint8_t *app = ops->map(ops->ctx, RLY_APP_BASE);
    if (app != NULL && rly_app_vectors_ok(app, RLY_APP_BASE, RLY_SLOT_SIZE)) {
        return RLY_BOOT_RUN_APP;
    }
    return RLY_BOOT_NO_APP;
}

int rly_boot_install(const rly_flash_ops *ops, const rly_ota_ctrl *ctrl) {
    const uint8_t *staging = ops->map(ops->ctx, RLY_STAGING_BASE);
    uint32_t size = ctrl->size;
    for (uint32_t a = RLY_APP_BASE; a < RLY_APP_BASE + size; a += RLY_FLASH_PAGE) {
        if (ops->erase_page(ops->ctx, a) != 0) {
            return -1;
        }
    }
    uint8_t chunk[RLY_OTA_CHUNK];
    for (uint32_t off = 0; off < size; off += RLY_OTA_CHUNK) {
        uint32_t n = size - off < RLY_OTA_CHUNK ? size - off : RLY_OTA_CHUNK;
        memcpy(chunk, staging + off, n);
        if (n & 1u) {
            chunk[n++] = 0xFFu;
        }
        if (ops->program(ops->ctx, RLY_APP_BASE + off, chunk, n) != 0) {
            return -1;
        }
    }
    const uint8_t *app = ops->map(ops->ctx, RLY_APP_BASE);
    return rly_crc32(app, size) == ctrl->crc ? 0 : -1;
}
