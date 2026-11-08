/* lwIP netif glue for the STM32F107 MAC driver (eth_dma.c).
 *
 * Polled mode (NO_SYS=1): the main loop calls ethernetif_input(), the DMA rings are drained
 * without interrupts. Frames are copied once, directly between DMA buffers and pbufs (no
 * bounce buffers: RAM is tight).
 */
#include "lwip/opt.h"

#include "lwip/def.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "netif/ethernet.h"

#include "eth_dma.h"
#include "ethernetif.h"


static err_t low_level_output(struct netif *netif, struct pbuf *p) {
    LWIP_UNUSED_ARG(netif);
    if (p->tot_len > ETH_DMA_BUFFER_SIZE) {
        return ERR_BUF;
    }
    /* Flatten the pbuf chain straight into the DMA buffer. */
    uint8_t *buf = eth_dma_tx_acquire();
    if (buf == NULL) {
        return ERR_IF;
    }
    pbuf_copy_partial(p, buf, p->tot_len, 0);
    return eth_dma_tx_commit(p->tot_len) == 0 ? ERR_OK : ERR_IF;
}

err_t ethernetif_init(struct netif *netif) {
    const uint8_t *mac = (const uint8_t *)netif->state;

    netif->name[0] = 'e';
    netif->name[1] = 'n';
    netif->output = etharp_output;
    netif->linkoutput = low_level_output;
    netif->mtu = 1500;
    netif->hwaddr_len = ETH_HWADDR_LEN;
    if (mac != NULL) {
        for (uint32_t i = 0; i < ETH_HWADDR_LEN; i++) {
            netif->hwaddr[i] = mac[i];
        }
    }
    netif->flags |= NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP;
    return ERR_OK;
}

void ethernetif_input(struct netif *netif) {
    int len;
    const uint8_t *frame;
    while ((len = eth_dma_rx_peek(&frame)) != 0) {
        if (len < 0) {
            continue; /* bad frame, already released */
        }
        struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_POOL);
        if (p != NULL && pbuf_take(p, frame, (u16_t)len) != ERR_OK) {
            pbuf_free(p);
            p = NULL;
        }
        eth_dma_rx_release();
        if (p != NULL && netif->input(p, netif) != ERR_OK) {
            pbuf_free(p);
        }
    }
}
