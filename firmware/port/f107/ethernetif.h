/* lwIP netif glue for the RLY-8 firmware. */
#ifndef ETHERNETIF_H
#define ETHERNETIF_H

#include "lwip/netif.h"

/* netif->state must point to a 6-byte station address (kept alive by the caller). */
err_t ethernetif_init(struct netif *netif);

/* Drains the DMA receive ring into lwIP; call from the main loop. */
void ethernetif_input(struct netif *netif);

#endif /* ETHERNETIF_H */
