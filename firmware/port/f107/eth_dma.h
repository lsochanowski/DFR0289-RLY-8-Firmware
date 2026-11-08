/* Ethernet MAC + DMA driver for the STM32F107 (Synopsys DesignWare core).
 *
 * Kept separate from lwIP: this is the hardware side (descriptor rings, MAC setup), while
 * ethernetif.c adapts it to a lwIP netif.
 *
 * Register names come from the CMSIS device header (third_party/cmsis/stm32f107xc.h);
 * descriptor bit layouts are defined here because CMSIS does not provide them, and they are
 * documented inline against RM0008 (Ethernet peripheral, descriptor tables).
 */
#ifndef ETH_DMA_H
#define ETH_DMA_H

#include <stdint.h>

#define ETH_DMA_RX_RING 4u
#define ETH_DMA_TX_RING 2u
#define ETH_DMA_BUFFER_SIZE 1524u

/* Returns 0 on success. mac is the 6-byte station address. */
int eth_dma_init(const uint8_t mac[6]);

/* Queues one frame (<= ETH_DMA_BUFFER_SIZE). Returns 0 on success, -1 when no descriptor
 * is free (caller should retry later). */
int eth_dma_send(const uint8_t *frame, uint32_t len);

/* Copies at most max_len bytes of one received frame. Returns the frame length, 0 when
 * nothing is pending, -1 on a DMA error. */
int eth_dma_recv(uint8_t *frame, uint32_t max_len);

/* Zero-copy variants (used by ethernetif.c to avoid 3 KB of bounce buffers):
 * tx_acquire returns the next free DMA buffer or NULL; tx_commit hands len bytes to the DMA.
 * rx_peek returns the frame length (>0) and a pointer into the DMA buffer, 0 when nothing is
 * pending, -1 for a dropped bad frame; call rx_release once the frame has been copied. */
uint8_t *eth_dma_tx_acquire(void);
int eth_dma_tx_commit(uint32_t len);
int eth_dma_rx_peek(const uint8_t **frame);
void eth_dma_rx_release(void);

/* Cumulative counters, handy in the boot report. */
typedef struct {
    uint32_t rx_frames;
    uint32_t rx_errors;
    uint32_t tx_frames;
    uint32_t tx_dropped;
} eth_dma_stats;

extern volatile eth_dma_stats eth_stats;

#endif /* ETH_DMA_H */
