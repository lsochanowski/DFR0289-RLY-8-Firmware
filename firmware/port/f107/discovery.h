/* Name services on lwIP: mDNS responder for "<hostname>.local" and MQTT broker discovery.
 *
 * Broker resolution, depending on the configured mqtt_host:
 *   ""             autodetect: mDNS PTR _mqtt._tcp.local (SRV gives host + port), then
 *                  homeassistant.local (A) with the configured port; retried every 10 s
 *   a.b.c.d        used as is
 *   name.local     mDNS A query
 *   anything else  unicast DNS (server from DHCP or the static config)
 */
#ifndef DISCOVERY_H
#define DISCOVERY_H

#include <stdint.h>

#include "lwip/ip_addr.h"
#include "lwip/netif.h"

void discovery_init(struct netif *netif, const char *hostname);
/* Starts (or restarts) broker resolution. host may be "" for autodetect. */
void discovery_broker_start(const char *host, uint16_t port);
/* Call regularly; handles timeouts and retries. */
void discovery_poll(uint32_t now_ms);
/* Once resolved: 1 and the address/port. */
int discovery_broker(ip_addr_t *ip, uint16_t *port);
/* Human-readable source of the broker address ("ręcznie", "mDNS _mqtt._tcp", ...). */
const char *discovery_source(void);
/* Send an unsolicited mDNS announcement (after the address changes). */
void discovery_announce(void);

#endif /* DISCOVERY_H */
