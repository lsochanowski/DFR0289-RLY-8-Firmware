/* Web panel: routing, HTML pages and form handling (platform independent).
 *
 * Every page needs HTTP Basic auth, user "admin", password from the config.
 * State-changing requests are POST only and carry a per-boot token (field "t"), so a
 * foreign page cannot make the browser flip relays with cached credentials.
 *
 *   GET  /               status + relay buttons
 *   GET  /settings       network / MQTT / password form
 *   POST /save           apply + persist settings, then reboot
 *   POST /relay          n=1..8|all, s=on|off|toggle
 *   POST /reboot
 *   POST /factory        restore factory defaults, then reboot
 *   GET  /api/status     JSON status (for scripts)
 *   POST /update?t=<tok> raw application image (streamed into the staging slot, see
 *                        core/rly_ota.h); the bootloader installs it after a reboot
 */
#ifndef RLY_WEB_H
#define RLY_WEB_H

#include <stddef.h>
#include <stdint.h>

#include "rly_config.h"
#include "rly_http.h"
#include "rly_ota.h"

typedef struct {
    const char *fw_version;
    const char *node_id;
    uint8_t mac[6];
    uint32_t ip, mask, gw; /* current, lwIP byte order */
    int link_up;
    int speed_100;
    int full_duplex;
    int dhcp_bound;      /* address came from DHCP */
    int static_fallback; /* DHCP timed out, fallback address in use */
    uint32_t uptime_s;
    const char *mqtt_state; /* "wyłączone", "łączenie", "połączony", "błąd" ... */
    const char *mqtt_broker; /* resolved "a.b.c.d:port" or "" */
    const char *mqtt_source; /* "ręcznie", "mDNS _mqtt._tcp", "homeassistant.local", ... */
    uint8_t relay_mask;
    int usb_connected;
} rly_web_status;

typedef enum {
    RLY_WEB_ACT_NONE = 0,
    RLY_WEB_ACT_SAVE_REBOOT = 1, /* config was changed: persist, then reboot */
    RLY_WEB_ACT_REBOOT = 2,
    RLY_WEB_ACT_FACTORY = 3,
    RLY_WEB_ACT_RELAYS = 4 /* relay_mask in the result changed */
} rly_web_action;

typedef struct {
    rly_web_action action;
    uint8_t relay_mask; /* desired mask for RLY_WEB_ACT_RELAYS */
} rly_web_result;

/* Handles one parsed request. `cfg` is the live config: /save edits a copy and only writes
 * it back on success. Returns the response length in out (headers + body). */
size_t rly_web_handle(const rly_http_request *req, rly_config *cfg, const rly_web_status *st,
                      const char *token, char *out, size_t cap, rly_web_result *res);

/* Firmware upload, checked as soon as the request headers are in: Basic auth, the token in
 * the query string and the announced size. Returns 1 to accept; otherwise writes a complete
 * error response into out and returns 0. */
int rly_web_upload_allowed(const rly_http_request *req, const rly_config *cfg, const char *token,
                           char *out, size_t cap, size_t *len);
/* Response for a finished upload. */
size_t rly_web_upload_result(rly_img_status st, const char *version, char *out, size_t cap);

#endif /* RLY_WEB_H */
