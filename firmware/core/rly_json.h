/* Minimal, allocation-free parser for the RLY-8 JSON command dialect.
 *
 * The device dialect is a flat object of string values, e.g.
 *   {"get":"relayStatus"}   {"relay1":"on","relay2":"off"}   {"reboot":"1"}
 * so we do not need a general JSON parser (no nesting, no escapes, no numbers).
 */
#ifndef RLY_JSON_H
#define RLY_JSON_H

#include <stddef.h>
#include <stdint.h>

#include "rly_frame.h"

#define RLY_JSON_MAX_PAIRS 10
#define RLY_JSON_KEY_MAX 16
#define RLY_JSON_VAL_MAX 40

typedef struct {
    char key[RLY_JSON_KEY_MAX];
    char val[RLY_JSON_VAL_MAX];
} rly_json_pair;

typedef struct {
    rly_json_pair pairs[RLY_JSON_MAX_PAIRS];
    size_t count;
} rly_json_obj;

/* Parses one flat JSON object of string values. */
rly_status rly_json_parse(const char *s, size_t n, rly_json_obj *out);

/* Returns the value for key, or NULL. */
const char *rly_json_get(const rly_json_obj *obj, const char *key);

/* relayN ("relay1".."relay8") entries: mentioned and on get bitmaps, bit0 = relay 1. */
rly_status rly_json_relays(const rly_json_obj *obj, uint8_t *mentioned, uint8_t *on);

/* {"relay1":"on",...,"relay8":"off"} for the given bitmap. Returns bytes written. */
size_t rly_json_relay_status(char *out, size_t cap, uint8_t mask);

/* {"resp":"ok"} and friends. */
size_t rly_json_resp(char *out, size_t cap, const char *resp);

#endif /* RLY_JSON_H */
