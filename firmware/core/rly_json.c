#include "rly_json.h"

#include <stdio.h>
#include <string.h>

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

static void skip_space(const char **p, const char *end) {
    while (*p < end && is_space(**p)) {
        (*p)++;
    }
}

/* An object ends at '}'; anything but trailing whitespace after it is malformed. */
static rly_status finish(const char **p, const char *end) {
    skip_space(p, end);
    return *p == end ? RLY_OK : RLY_ERR_ARG;
}

/* Copies a quoted string; returns 0 on success. Stops at the closing quote. */
static int read_quoted(const char **p, const char *end, char *dst, size_t cap) {
    size_t len = 0;
    if (*p >= end || **p != '"') {
        return -1;
    }
    (*p)++;
    while (*p < end && **p != '"') {
        if (len + 1 >= cap) {
            return -1;
        }
        dst[len++] = **p;
        (*p)++;
    }
    if (*p >= end) {
        return -1;
    }
    (*p)++; /* closing quote */
    dst[len] = '\0';
    return 0;
}

rly_status rly_json_parse(const char *s, size_t n, rly_json_obj *out) {
    if (s == NULL || out == NULL) {
        return RLY_ERR_ARG;
    }
    const char *p = s;
    const char *end = s + n;
    out->count = 0;

    skip_space(&p, end);
    if (p >= end || *p != '{') {
        return RLY_ERR_ARG;
    }
    p++;
    skip_space(&p, end);
    if (p < end && *p == '}') {
        p++;
        return finish(&p, end); /* empty object is valid, if useless */
    }

    for (;;) {
        if (out->count >= RLY_JSON_MAX_PAIRS) {
            return RLY_ERR_LEN;
        }
        rly_json_pair *pair = &out->pairs[out->count];
        skip_space(&p, end);
        if (read_quoted(&p, end, pair->key, sizeof pair->key) != 0) {
            return RLY_ERR_ARG;
        }
        skip_space(&p, end);
        if (p >= end || *p != ':') {
            return RLY_ERR_ARG;
        }
        p++;
        skip_space(&p, end);
        if (read_quoted(&p, end, pair->val, sizeof pair->val) != 0) {
            return RLY_ERR_ARG;
        }
        out->count++;
        skip_space(&p, end);
        if (p < end && *p == ',') {
            p++;
            continue;
        }
        if (p < end && *p == '}') {
            p++;
            return finish(&p, end);
        }
        return RLY_ERR_ARG;
    }
}

const char *rly_json_get(const rly_json_obj *obj, const char *key) {
    if (obj == NULL || key == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < obj->count; i++) {
        if (strcmp(obj->pairs[i].key, key) == 0) {
            return obj->pairs[i].val;
        }
    }
    return NULL;
}

rly_status rly_json_relays(const rly_json_obj *obj, uint8_t *mentioned, uint8_t *on) {
    if (obj == NULL || mentioned == NULL || on == NULL) {
        return RLY_ERR_ARG;
    }
    *mentioned = 0;
    *on = 0;
    for (size_t i = 0; i < obj->count; i++) {
        const char *key = obj->pairs[i].key;
        if (strncmp(key, "relay", 5) != 0 || key[5] < '1' || key[5] > '8' || key[6] != '\0') {
            continue;
        }
        uint8_t bit = (uint8_t)(1u << (key[5] - '1'));
        *mentioned |= bit;
        if (strcmp(obj->pairs[i].val, "on") == 0) {
            *on |= bit;
        } else if (strcmp(obj->pairs[i].val, "off") != 0) {
            return RLY_ERR_ARG; /* only on/off are legal */
        }
    }
    return RLY_OK;
}

size_t rly_json_relay_status(char *out, size_t cap, uint8_t mask) {
    if (out == NULL || cap == 0) {
        return 0;
    }
    size_t used = 0;
    int written = snprintf(out, cap, "{");
    if (written < 0 || (size_t)written >= cap) {
        return 0;
    }
    used = (size_t)written;
    for (unsigned relay = 1; relay <= RLY_RELAY_COUNT; relay++) {
        int on = (mask >> (relay - 1)) & 1u;
        written = snprintf(out + used, cap - used, "%s\"relay%u\":\"%s\"", relay == 1 ? "" : ",",
                           relay, on ? "on" : "off");
        if (written < 0 || (size_t)written >= cap - used) {
            return 0;
        }
        used += (size_t)written;
    }
    written = snprintf(out + used, cap - used, "}");
    if (written < 0 || (size_t)written >= cap - used) {
        return 0;
    }
    return used + (size_t)written;
}

size_t rly_json_resp(char *out, size_t cap, const char *resp) {
    if (out == NULL || cap == 0 || resp == NULL) {
        return 0;
    }
    int written = snprintf(out, cap, "{\"resp\":\"%s\"}", resp);
    if (written < 0 || (size_t)written >= cap) {
        return 0;
    }
    return (size_t)written;
}
