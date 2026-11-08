#include "rly_ha.h"

#include <string.h>

#include "rly_version.h"

#define APPEND_OVERFLOW ((size_t)-1)

static size_t append(char *out, size_t cap, size_t used, const char *s) {
    size_t n = strlen(s);
    if (used == APPEND_OVERFLOW || used + n + 1 > cap) {
        return APPEND_OVERFLOW;
    }
    memcpy(out + used, s, n);
    out[used + n] = '\0';
    return used + n;
}

static size_t append_uint(char *out, size_t cap, size_t used, unsigned value) {
    char digits[11];
    size_t n = 0;
    if (used == APPEND_OVERFLOW) {
        return APPEND_OVERFLOW;
    }
    if (value == 0) {
        digits[n++] = '0';
    }
    while (value > 0) {
        digits[n++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    if (used + n + 1 > cap) {
        return APPEND_OVERFLOW;
    }
    for (size_t i = 0; i < n; i++) {
        out[used + i] = digits[n - 1 - i];
    }
    out[used + n] = '\0';
    return used + n;
}

/* "<prefix><node>/relay<N><suffix>" */
static size_t build_relay_topic(char *out, size_t cap, const char *prefix, const char *node,
                                uint8_t relay, const char *suffix) {
    size_t used = append(out, cap, 0, prefix);
    used = append(out, cap, used, node);
    used = append(out, cap, used, "/relay");
    used = append_uint(out, cap, used, relay);
    return append(out, cap, used, suffix);
}

void rly_ha_defaults(rly_ha_config *cfg, const char *node_id) {
    if (cfg == NULL) {
        return;
    }
    memset(cfg, 0, sizeof *cfg);
    if (node_id != NULL) {
        size_t n = strlen(node_id);
        if (n >= sizeof cfg->node_id) {
            n = sizeof cfg->node_id - 1;
        }
        memcpy(cfg->node_id, node_id, n);
        cfg->node_id[n] = '\0';
    }
    (void)append(cfg->device_name, sizeof cfg->device_name, 0, "RLY-8");
    (void)append(cfg->sw_version, sizeof cfg->sw_version, 0, RLY_FW_VERSION);
    (void)append(cfg->prefix, sizeof cfg->prefix, 0, "homeassistant");
}

void rly_ha_topics(const rly_ha_config *cfg, uint8_t relay, char *discovery, char *command,
                   char *state, char *availability, size_t cap) {
    if (cfg == NULL || cap == 0) {
        return;
    }
    if (discovery != NULL) {
        discovery[0] = '\0';
        size_t used = append(discovery, cap, 0, cfg->prefix[0] ? cfg->prefix : "homeassistant");
        used = append(discovery, cap, used, "/switch/");
        if (used == APPEND_OVERFLOW ||
            build_relay_topic(discovery + used, cap - used, "", cfg->node_id, relay, "/config") ==
                APPEND_OVERFLOW) {
            discovery[0] = '\0';
        }
    }
    if (command != NULL) {
        command[0] = '\0';
        if (build_relay_topic(command, cap, "", cfg->node_id, relay, "/set") == APPEND_OVERFLOW) {
            command[0] = '\0';
        }
    }
    if (state != NULL) {
        state[0] = '\0';
        if (build_relay_topic(state, cap, "", cfg->node_id, relay, "/state") == APPEND_OVERFLOW) {
            state[0] = '\0';
        }
    }
    if (availability != NULL) {
        availability[0] = '\0';
        size_t used = append(availability, cap, 0, cfg->node_id);
        if (append(availability, cap, used, "/status") == APPEND_OVERFLOW) {
            availability[0] = '\0';
        }
    }
}

size_t rly_ha_discovery(const rly_ha_config *cfg, uint8_t relay, char *out, size_t cap) {
    if (cfg == NULL || out == NULL || cap == 0 || relay < 1 || relay > RLY_RELAY_COUNT) {
        return 0;
    }
    if (cap > 0) {
        out[0] = '\0';
    }
    char state[RLY_TOPIC_MAX];
    char command[RLY_TOPIC_MAX];
    char availability[RLY_TOPIC_MAX];
    rly_ha_topics(cfg, relay, NULL, command, state, availability, sizeof state);
    if (command[0] == '\0' || state[0] == '\0' || availability[0] == '\0') {
        return 0;
    }

    size_t used = append(out, cap, 0, "{\"name\":\"");
    used = append(out, cap, used, cfg->device_name);
    used = append(out, cap, used, " Relay ");
    used = append_uint(out, cap, used, relay);
    used = append(out, cap, used, "\",\"uniq_id\":\"");
    used = append(out, cap, used, cfg->node_id);
    used = append(out, cap, used, "_relay");
    used = append_uint(out, cap, used, relay);
    used = append(out, cap, used, "\",\"stat_t\":\"");
    used = append(out, cap, used, state);
    used = append(out, cap, used, "\",\"cmd_t\":\"");
    used = append(out, cap, used, command);
    used = append(out, cap, used, "\",\"avty_t\":\"");
    used = append(out, cap, used, availability);
    used = append(out, cap, used,
                  "\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"dev\":{\"ids\":[\"");
    used = append(out, cap, used, cfg->node_id);
    used = append(out, cap, used, "\"],\"name\":\"");
    used = append(out, cap, used, cfg->device_name);
    used = append(out, cap, used,
                  "\",\"mf\":\"DFRobot\",\"mdl\":\"DFR0289 (custom fw)\",\"sw\":\"");
    used = append(out, cap, used, cfg->sw_version);
    if (cfg->config_url[0] != '\0') {
        used = append(out, cap, used, "\",\"cu\":\"");
        used = append(out, cap, used, cfg->config_url);
    }
    used = append(out, cap, used, "\"}}");

    if (used == APPEND_OVERFLOW) {
        out[0] = '\0';
        return 0;
    }
    return used;
}

const char *rly_ha_state_payload(int on) { return on ? "ON" : "OFF"; }
