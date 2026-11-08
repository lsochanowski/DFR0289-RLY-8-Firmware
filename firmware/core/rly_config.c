#include "rly_config.h"

#include <string.h>

const char *const rly_config_keys[] = {
    "dhcp",      "ip",        "mask",      "gw",          "dns",        "hostname", "name",
    "mqtt",      "mqtt_host", "mqtt_port", "mqtt_user",   "mqtt_pass",  "mqtt_prefix",
    "admin_pass", NULL};

static uint32_t ip4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return (uint32_t)a | ((uint32_t)b << 8) | ((uint32_t)c << 16) | ((uint32_t)d << 24);
}

static void copy_str(char *dst, size_t cap, const char *src) {
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void rly_config_defaults(rly_config *cfg) {
    if (cfg == NULL) {
        return;
    }
    memset(cfg, 0, sizeof *cfg);
    cfg->dhcp = 1;
    /* Used when dhcp=off, and as the fallback when no DHCP server answers. */
    cfg->ip = ip4(192, 168, 9, 50);
    cfg->mask = ip4(255, 255, 255, 0);
    cfg->gw = ip4(192, 168, 9, 1);
    cfg->dns = ip4(192, 168, 9, 1);
    copy_str(cfg->device_name, sizeof cfg->device_name, "RLY-8");
    cfg->mqtt_enabled = 1;
    cfg->mqtt_port = 1883;
    copy_str(cfg->mqtt_prefix, sizeof cfg->mqtt_prefix, "homeassistant");
    copy_str(cfg->admin_pass, sizeof cfg->admin_pass, "admin");
    rly_config_seal(cfg);
}

uint32_t rly_crc32(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

void rly_config_seal(rly_config *cfg) {
    cfg->magic = RLY_CFG_MAGIC;
    cfg->version = RLY_CFG_VERSION;
    cfg->size = (uint16_t)sizeof *cfg;
    cfg->crc = rly_crc32(cfg, offsetof(rly_config, crc));
}

int rly_config_valid(const rly_config *cfg) {
    if (cfg == NULL || cfg->magic != RLY_CFG_MAGIC || cfg->version != RLY_CFG_VERSION ||
        cfg->size != sizeof *cfg) {
        return 0;
    }
    return cfg->crc == rly_crc32(cfg, offsetof(rly_config, crc));
}

int rly_parse_ipv4(const char *s, uint32_t *out) {
    uint32_t parts[4];
    int n = 0;
    if (s == NULL) {
        return 0;
    }
    while (n < 4) {
        uint32_t v = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10u + (uint32_t)(*s - '0');
            s++;
            if (++digits > 3) {
                return 0;
            }
        }
        if (digits == 0 || v > 255u) {
            return 0;
        }
        parts[n++] = v;
        if (n < 4) {
            if (*s != '.') {
                return 0;
            }
            s++;
        }
    }
    if (*s != '\0') {
        return 0;
    }
    if (out != NULL) {
        *out = ip4((uint8_t)parts[0], (uint8_t)parts[1], (uint8_t)parts[2], (uint8_t)parts[3]);
    }
    return 1;
}

static size_t put_uint(char *out, size_t cap, size_t used, unsigned v) {
    char d[10];
    size_t n = 0;
    do {
        d[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v != 0u);
    while (n > 0 && used + 1 < cap) {
        out[used++] = d[--n];
    }
    out[used < cap ? used : cap - 1] = '\0';
    return used;
}

void rly_format_ipv4(uint32_t ip, char *out, size_t cap) {
    size_t used = 0;
    if (out == NULL || cap == 0) {
        return;
    }
    out[0] = '\0';
    for (int i = 0; i < 4; i++) {
        used = put_uint(out, cap, used, (ip >> (8 * i)) & 0xFFu);
        if (i < 3 && used + 1 < cap) {
            out[used++] = '.';
            out[used] = '\0';
        }
    }
}

/* Trims leading/trailing spaces into a bounded copy. */
static rly_cfg_status set_str(char *dst, size_t cap, const char *value) {
    while (*value == ' ' || *value == '\t') {
        value++;
    }
    size_t n = strlen(value);
    while (n > 0 && (value[n - 1] == ' ' || value[n - 1] == '\t' || value[n - 1] == '\r' ||
                     value[n - 1] == '\n')) {
        n--;
    }
    if (n >= cap) {
        return RLY_CFG_TOO_LONG;
    }
    memcpy(dst, value, n);
    dst[n] = '\0';
    return RLY_CFG_OK;
}

static int parse_bool(const char *v, uint8_t *out) {
    if (strcmp(v, "on") == 0 || strcmp(v, "1") == 0 || strcmp(v, "yes") == 0 ||
        strcmp(v, "true") == 0) {
        *out = 1;
        return 1;
    }
    if (strcmp(v, "off") == 0 || strcmp(v, "0") == 0 || strcmp(v, "no") == 0 ||
        strcmp(v, "false") == 0) {
        *out = 0;
        return 1;
    }
    return 0;
}

/* Host names: letters, digits, '-' (and '.' for the broker), no leading '-'. */
static int valid_host(const char *s, int allow_dot) {
    if (s[0] == '-') {
        return 0;
    }
    for (; *s; s++) {
        char c = *s;
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                 c == '-' || (allow_dot && c == '.');
        if (!ok) {
            return 0;
        }
    }
    return 1;
}

/* Printable text without characters that would break JSON or MQTT topics. Bytes >= 0x80
 * (UTF-8, e.g. "Garaż") are accepted only where allow_utf8 is set. */
static int valid_text(const char *s, int allow_space, int allow_utf8) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20u || c == 0x7Fu || c == '"' || c == '\\' || (!allow_space && c == ' ') ||
            (c > 0x7Fu && !allow_utf8)) {
            return 0;
        }
    }
    return 1;
}

static rly_cfg_status set_ip(uint32_t *dst, const char *value) {
    uint32_t v;
    while (*value == ' ') {
        value++;
    }
    if (!rly_parse_ipv4(value, &v)) {
        return RLY_CFG_BAD_VALUE;
    }
    *dst = v;
    return RLY_CFG_OK;
}

rly_cfg_status rly_config_set(rly_config *cfg, const char *key, const char *value) {
    char tmp[RLY_CFG_HOST_MAX];
    rly_cfg_status st;
    if (cfg == NULL || key == NULL || value == NULL) {
        return RLY_CFG_BAD_VALUE;
    }
    if (strcmp(key, "dhcp") == 0 || strcmp(key, "mqtt") == 0) {
        uint8_t b;
        if ((st = set_str(tmp, sizeof tmp, value)) != RLY_CFG_OK) {
            return st;
        }
        if (!parse_bool(tmp, &b)) {
            return RLY_CFG_BAD_VALUE;
        }
        if (key[0] == 'd') {
            cfg->dhcp = b;
        } else {
            cfg->mqtt_enabled = b;
        }
        return RLY_CFG_OK;
    }
    if (strcmp(key, "ip") == 0) {
        return set_ip(&cfg->ip, value);
    }
    if (strcmp(key, "mask") == 0) {
        return set_ip(&cfg->mask, value);
    }
    if (strcmp(key, "gw") == 0) {
        return set_ip(&cfg->gw, value);
    }
    if (strcmp(key, "dns") == 0) {
        return set_ip(&cfg->dns, value);
    }
    if (strcmp(key, "mqtt_port") == 0) {
        if ((st = set_str(tmp, sizeof tmp, value)) != RLY_CFG_OK) {
            return st;
        }
        uint32_t v = 0;
        const char *p = tmp;
        if (*p == '\0') {
            return RLY_CFG_BAD_VALUE;
        }
        for (; *p; p++) {
            if (*p < '0' || *p > '9' || v > 65535u) {
                return RLY_CFG_BAD_VALUE;
            }
            v = v * 10u + (uint32_t)(*p - '0');
        }
        if (v == 0u || v > 65535u) {
            return RLY_CFG_BAD_VALUE;
        }
        cfg->mqtt_port = (uint16_t)v;
        return RLY_CFG_OK;
    }

    struct {
        const char *key;
        char *dst;
        size_t cap;
        int kind; /* 0 host, 1 hostname (no dots), 2 text with spaces, 3 text no spaces,
                     4 secret (any printable) */
        int allow_empty;
    } strs[] = {
        {"hostname", cfg->hostname, sizeof cfg->hostname, 1, 1},
        {"name", cfg->device_name, sizeof cfg->device_name, 2, 0},
        {"mqtt_host", cfg->mqtt_host, sizeof cfg->mqtt_host, 0, 1},
        {"mqtt_user", cfg->mqtt_user, sizeof cfg->mqtt_user, 3, 1},
        {"mqtt_pass", cfg->mqtt_pass, sizeof cfg->mqtt_pass, 4, 1},
        {"mqtt_prefix", cfg->mqtt_prefix, sizeof cfg->mqtt_prefix, 3, 0},
        {"admin_pass", cfg->admin_pass, sizeof cfg->admin_pass, 4, 0},
    };
    for (size_t i = 0; i < sizeof strs / sizeof strs[0]; i++) {
        if (strcmp(key, strs[i].key) != 0) {
            continue;
        }
        char buf[RLY_CFG_HOST_MAX];
        if ((st = set_str(buf, strs[i].cap, value)) != RLY_CFG_OK) {
            return st;
        }
        if (buf[0] == '\0' && !strs[i].allow_empty) {
            return RLY_CFG_BAD_VALUE;
        }
        int ok = 1;
        switch (strs[i].kind) {
        case 0:
            ok = valid_host(buf, 1);
            break;
        case 1:
            ok = valid_host(buf, 0);
            break;
        case 2:
            ok = valid_text(buf, 1, 1);
            break;
        case 3:
            ok = valid_text(buf, 0, 0) && strchr(buf, '+') == NULL && strchr(buf, '#') == NULL;
            break;
        default:
            ok = valid_text(buf, 1, 1);
            break;
        }
        if (!ok) {
            return RLY_CFG_BAD_VALUE;
        }
        memcpy(strs[i].dst, buf, strlen(buf) + 1);
        return RLY_CFG_OK;
    }
    return RLY_CFG_UNKNOWN_KEY;
}

rly_cfg_status rly_config_get(const rly_config *cfg, const char *key, char *out, size_t cap,
                              int reveal) {
    if (cfg == NULL || key == NULL || out == NULL || cap == 0) {
        return RLY_CFG_BAD_VALUE;
    }
    out[0] = '\0';
    if (strcmp(key, "dhcp") == 0) {
        copy_str(out, cap, cfg->dhcp ? "on" : "off");
    } else if (strcmp(key, "mqtt") == 0) {
        copy_str(out, cap, cfg->mqtt_enabled ? "on" : "off");
    } else if (strcmp(key, "ip") == 0) {
        rly_format_ipv4(cfg->ip, out, cap);
    } else if (strcmp(key, "mask") == 0) {
        rly_format_ipv4(cfg->mask, out, cap);
    } else if (strcmp(key, "gw") == 0) {
        rly_format_ipv4(cfg->gw, out, cap);
    } else if (strcmp(key, "dns") == 0) {
        rly_format_ipv4(cfg->dns, out, cap);
    } else if (strcmp(key, "mqtt_port") == 0) {
        (void)put_uint(out, cap, 0, cfg->mqtt_port);
    } else if (strcmp(key, "hostname") == 0) {
        copy_str(out, cap, cfg->hostname);
    } else if (strcmp(key, "name") == 0) {
        copy_str(out, cap, cfg->device_name);
    } else if (strcmp(key, "mqtt_host") == 0) {
        copy_str(out, cap, cfg->mqtt_host);
    } else if (strcmp(key, "mqtt_user") == 0) {
        copy_str(out, cap, cfg->mqtt_user);
    } else if (strcmp(key, "mqtt_prefix") == 0) {
        copy_str(out, cap, cfg->mqtt_prefix);
    } else if (strcmp(key, "mqtt_pass") == 0 || strcmp(key, "admin_pass") == 0) {
        const char *v = key[0] == 'm' ? cfg->mqtt_pass : cfg->admin_pass;
        copy_str(out, cap, reveal ? v : (v[0] ? "***" : ""));
    } else {
        return RLY_CFG_UNKNOWN_KEY;
    }
    return RLY_CFG_OK;
}
