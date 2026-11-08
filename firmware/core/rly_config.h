/* Persistent device configuration (network, MQTT, admin password).
 *
 * The same key/value interface is used by the web panel (form field names) and by the USB
 * console ("set <key> <value>"), so validation lives in exactly one place.
 *
 * Keys:
 *   dhcp            on|off
 *   ip, mask, gw, dns   dotted IPv4 (used when dhcp=off; dns also as fallback)
 *   hostname        DHCP/mDNS host name; empty = rly8-<mac>
 *   name            device name shown in Home Assistant
 *   mqtt            on|off
 *   mqtt_host       IPv4, DNS name, *.local name; empty = autodetect (mDNS)
 *   mqtt_port       1..65535
 *   mqtt_user, mqtt_pass
 *   mqtt_prefix     HA discovery prefix (default "homeassistant")
 *   admin_pass      web panel password (user "admin")
 */
#ifndef RLY_CONFIG_H
#define RLY_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#define RLY_CFG_MAGIC 0x47464352u /* "RCFG" */
#define RLY_CFG_VERSION 1u

#define RLY_CFG_HOST_MAX 64
#define RLY_CFG_NAME_MAX 32
#define RLY_CFG_USER_MAX 32
#define RLY_CFG_PASS_MAX 48
#define RLY_CFG_PREFIX_MAX 32

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size; /* sizeof(rly_config), guards against layout changes */
    uint32_t seq;  /* bumped on every save; the newer of the two flash copies wins */

    uint8_t dhcp;
    uint8_t mqtt_enabled;
    uint16_t mqtt_port;
    uint32_t ip, mask, gw, dns; /* network byte order as in lwIP (a.b.c.d -> a | b<<8 ...) */

    char hostname[RLY_CFG_NAME_MAX];
    char device_name[RLY_CFG_NAME_MAX];
    char mqtt_host[RLY_CFG_HOST_MAX];
    char mqtt_user[RLY_CFG_USER_MAX];
    char mqtt_pass[RLY_CFG_PASS_MAX];
    char mqtt_prefix[RLY_CFG_PREFIX_MAX];
    char admin_pass[RLY_CFG_PASS_MAX];

    uint32_t crc; /* CRC-32 of every byte before this field */
} rly_config;

typedef enum {
    RLY_CFG_OK = 0,
    RLY_CFG_UNKNOWN_KEY = -1,
    RLY_CFG_BAD_VALUE = -2,
    RLY_CFG_TOO_LONG = -3
} rly_cfg_status;

/* Factory defaults: DHCP on, MQTT on with broker autodetect, admin password "admin". */
void rly_config_defaults(rly_config *cfg);

uint32_t rly_crc32(const void *data, size_t len);
void rly_config_seal(rly_config *cfg);         /* sets magic/version/size and crc */
int rly_config_valid(const rly_config *cfg);   /* 1 when magic, version, size and crc match */

/* Sets one key from text. Strings are trimmed of surrounding spaces. */
rly_cfg_status rly_config_set(rly_config *cfg, const char *key, const char *value);

/* Formats one key as text. Secrets are masked with "***" (empty stays empty) unless
 * reveal != 0. Returns RLY_CFG_UNKNOWN_KEY for unknown keys. */
rly_cfg_status rly_config_get(const rly_config *cfg, const char *key, char *out, size_t cap,
                              int reveal);

/* NULL-terminated list of all keys, in display order. */
extern const char *const rly_config_keys[];

/* Dotted-quad helpers (lwIP byte order: first octet in the lowest byte). */
int rly_parse_ipv4(const char *s, uint32_t *out);
void rly_format_ipv4(uint32_t ip, char *out, size_t cap);

#endif /* RLY_CONFIG_H */
