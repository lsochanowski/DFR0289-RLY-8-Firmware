/* Minimal mDNS (RFC 6762) for broker autodetection and "<host>.local" reachability.
 *
 * Querier: asks for _mqtt._tcp.local (PTR) and for plain host names such as
 * homeassistant.local (A), then scans answers + additional records for SRV and A.
 * Responder: answers A queries for our own "<hostname>.local".
 */
#ifndef RLY_MDNS_H
#define RLY_MDNS_H

#include <stddef.h>
#include <stdint.h>

#define RLY_MDNS_PORT 5353
#define RLY_MDNS_TYPE_A 1u
#define RLY_MDNS_TYPE_PTR 12u
#define RLY_MDNS_TYPE_SRV 33u
#define RLY_MDNS_TYPE_ANY 255u
#define RLY_MDNS_NAME_MAX 96
#define RLY_MDNS_MAX_A 4

typedef struct {
    int have_srv;
    uint16_t srv_port;
    char srv_target[RLY_MDNS_NAME_MAX];
    int a_count;
    char a_name[RLY_MDNS_MAX_A][RLY_MDNS_NAME_MAX];
    uint32_t a_ip[RLY_MDNS_MAX_A]; /* lwIP byte order */
} rly_mdns_answers;

/* One-question query. Returns length or 0. */
size_t rly_mdns_build_query(const char *name, uint16_t qtype, uint8_t *out, size_t cap);

/* Collects SRV records whose owner ends with `srv_service` (e.g. "_mqtt._tcp.local") and all
 * A records from a response. Returns 1 when the packet is a well-formed response. */
int rly_mdns_parse_response(const uint8_t *buf, size_t len, const char *srv_service,
                            rly_mdns_answers *out);

/* IPv4 of `name` among the collected A records, 0 when absent. */
uint32_t rly_mdns_lookup_a(const rly_mdns_answers *a, const char *name);

/* Returns 1 when buf is a query that asks for A/ANY of `fqdn` (e.g. "rly8-a1b2c3.local").
 * *unicast is set when the QU bit asks for a unicast reply. */
int rly_mdns_query_wants(const uint8_t *buf, size_t len, const char *fqdn, int *unicast);

/* Authoritative answer for fqdn -> ip (cache-flush set, TTL 120 s). Returns length or 0. */
size_t rly_mdns_build_a_answer(const char *fqdn, uint32_t ip, uint8_t *out, size_t cap);

#endif /* RLY_MDNS_H */
