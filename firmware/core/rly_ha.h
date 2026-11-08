/* Home Assistant MQTT discovery for the RLY-8 replacement firmware.
 *
 * Topics (node_id is the device slug, e.g. "rly8-a1b2c3"):
 *   <prefix>/switch/<node_id>/relay<N>/config         retained discovery (prefix
 *                                                    defaults to "homeassistant")
 *   <node_id>/relay<N>/set                           HA -> device ("ON"/"OFF")
 *   <node_id>/relay<N>/state                         device -> HA (retained)
 *   <node_id>/status                                 availability ("online"/"offline")
 */
#ifndef RLY_HA_H
#define RLY_HA_H

#include <stddef.h>
#include <stdint.h>

#include "rly_frame.h"

#define RLY_NODE_ID_MAX 32
#define RLY_TOPIC_MAX 96
#define RLY_HA_DISCOVERY_MAX 512

typedef struct {
    char node_id[RLY_NODE_ID_MAX];
    char device_name[40]; /* defaults to "RLY-8" */
    char sw_version[16];  /* defaults to the firmware version */
    char prefix[32];      /* discovery prefix, defaults to "homeassistant" */
    char config_url[40];  /* web panel link shown on the HA device page; "" = omit */
} rly_ha_config;

void rly_ha_defaults(rly_ha_config *cfg, const char *node_id);

/* Fills the three per-relay topics and the availability topic. Each may be NULL. */
void rly_ha_topics(const rly_ha_config *cfg, uint8_t relay, char *discovery, char *command,
                   char *state, char *availability, size_t cap);

/* Retained config payload for one relay switch entity. Returns bytes written. */
size_t rly_ha_discovery(const rly_ha_config *cfg, uint8_t relay, char *out, size_t cap);

/* "ON" / "OFF" payload for one relay state. */
const char *rly_ha_state_payload(int on);

#endif /* RLY_HA_H */
