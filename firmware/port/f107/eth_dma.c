/* Ethernet MAC + DMA driver for the STM32F107 (Synopsys DesignWare core).
 *
 * Register offsets and bit positions follow RM0008 §34.6 and were cross-checked against the
 * vendored CMSIS header (third_party/cmsis/stm32f107xc.h): MACCR RE=2, TE=3, DM=11, FES=14;
 * DMAOMR SR=1, ST=13, TSF=21, RSF=25; DMABMR SWR=0, PBL=8, AAB=25.
 *
 * Descriptor layout (4 words: status, control/size, buffer address, next descriptor) is the
 * standard Synopsys one; the bit constants below are flagged where a hardware check matters.
 */
#include "eth_dma.h"

#include <stddef.h>

#define REG(addr) (*(volatile uint32_t *)(addr))

#define RCC_AHBENR REG(0x40021014u)
#define ETH_MAC_BASE 0x40028000u
#define ETH_DMA_BASE 0x40029000u

#define MACCR REG(ETH_MAC_BASE + 0x00u)
#define MACFFR REG(ETH_MAC_BASE + 0x04u)
#define MACA0HR REG(ETH_MAC_BASE + 0x40u)
#define MACA0LR REG(ETH_MAC_BASE + 0x44u)

#define DMABMR REG(ETH_DMA_BASE + 0x00u)
#define DMATPDR REG(ETH_DMA_BASE + 0x04u)
#define DMARPDR REG(ETH_DMA_BASE + 0x08u)
#define DMARDLAR REG(ETH_DMA_BASE + 0x0Cu)
#define DMATDLAR REG(ETH_DMA_BASE + 0x10u)
#define DMASR REG(ETH_DMA_BASE + 0x14u)
#define DMAOMR REG(ETH_DMA_BASE + 0x18u)
#define DMAIER REG(ETH_DMA_BASE + 0x1Cu)

/* --- MACCR bits (RM0008 §34.6.1) --- */
#define MACCR_RE (1u << 2)
#define MACCR_TE (1u << 3)
#define MACCR_DM (1u << 11)
#define MACCR_FES (1u << 14)
#define MACCR_BL_10 (0x0u << 5)

/* --- DMAOMR bits (§34.6.12) --- */
#define DMAOMR_SR (1u << 1)
#define DMAOMR_OSF (1u << 2)
#define DMAOMR_ST (1u << 13)
#define DMAOMR_TSF (1u << 21)
#define DMAOMR_RSF (1u << 25)

/* --- DMABMR bits (§34.6.10) --- */
#define DMABMR_SWR (1u << 0)
#define DMABMR_PBL_32 (0x20u << 8)
#define DMABMR_AAB (1u << 25)

/* --- DMASR bits (§34.6.13) --- */
#define DMASR_TPS (0x7u << 20)
#define DMASR_RPS (0x7u << 17)
#define DMASR_NIS (1u << 16)
#define DMASR_TU (1u << 2)
#define DMASR_RU (1u << 7)

/* --- descriptor bits (RM0008 §34.6.3/§34.6.4) ---
 * These are the values that must be validated on hardware; they are identical on F1/F4. */
#define TDESC_OWN 0x80000000u
#define TDESC_IC 0x40000000u
#define TDESC_LS 0x20000000u
#define TDESC_FS 0x10000000u
#define TDESC_TCH 0x00100000u /* second address = next descriptor */
#define TDESC_TBS1_MASK 0x1FFFu

#define RDESC_OWN 0x80000000u
#define RDESC_FL_MASK 0x3FFF0000u
#define RDESC_FL_SHIFT 16u
#define RDESC_ES 0x00008000u
#define RDESC_FS 0x00000200u
#define RDESC_LS 0x00000100u
#define RDESC_RCH 0x00004000u /* RDES1: second address = next descriptor */
#define RDESC_RBS1_MASK 0x1FFFu

typedef struct {
    volatile uint32_t status;
    volatile uint32_t size;
    volatile uint32_t addr;
    volatile uint32_t next;
} eth_desc;

static eth_desc rx_ring[ETH_DMA_RX_RING] __attribute__((aligned(4)));
static eth_desc tx_ring[ETH_DMA_TX_RING] __attribute__((aligned(4)));
static uint8_t rx_buffers[ETH_DMA_RX_RING][ETH_DMA_BUFFER_SIZE] __attribute__((aligned(4)));
static uint8_t tx_buffers[ETH_DMA_TX_RING][ETH_DMA_BUFFER_SIZE] __attribute__((aligned(4)));

volatile eth_dma_stats eth_stats;

static uint32_t tx_index;
static uint32_t rx_index;

int eth_dma_init(const uint8_t mac[6]) {
    /* MAC clock plus its TX/RX clocks (AHBENR bits 14, 15, 16). */
    RCC_AHBENR |= (1u << 14) | (1u << 15) | (1u << 16);

    /* Software reset of the DMA engine; §34.6.10 says all other bits read as 0 afterwards. */
    DMABMR = DMABMR_SWR;
    for (volatile uint32_t spin = 0; spin < 100000u; spin++) {
        if ((DMABMR & DMABMR_SWR) == 0u) {
            break;
        }
    }
    if ((DMABMR & DMABMR_SWR) != 0u) {
        return -1;
    }

    DMABMR = DMABMR_PBL_32 | DMABMR_AAB;
    DMAOMR = DMAOMR_TSF | DMAOMR_RSF | DMAOMR_OSF;
    DMASR = 0xFFFFFFFFu; /* clear all pending flags */
    DMAIER = 0;          /* polled operation: the main loop drains the rings */

    /* Station address: MACA0HR expects the high 16 bits in the low half. */
    MACA0HR = ((uint32_t)mac[5] << 8) | mac[4];
    MACA0LR = ((uint32_t)mac[3] << 24) | ((uint32_t)mac[2] << 16) | ((uint32_t)mac[1] << 8) |
              mac[0];

    /* Rings. RX descriptors stay owned by the DMA; TX descriptors start owned by the CPU. */
    for (uint32_t i = 0; i < ETH_DMA_RX_RING; i++) {
        rx_ring[i].status = RDESC_OWN;
        rx_ring[i].size = RDESC_RCH | (ETH_DMA_BUFFER_SIZE & RDESC_RBS1_MASK);
        rx_ring[i].addr = (uint32_t)(uintptr_t)rx_buffers[i];
        rx_ring[i].next = (uint32_t)(uintptr_t)&rx_ring[(i + 1u) % ETH_DMA_RX_RING];
    }
    for (uint32_t i = 0; i < ETH_DMA_TX_RING; i++) {
        tx_ring[i].status = TDESC_TCH;
        tx_ring[i].size = 0;
        tx_ring[i].addr = (uint32_t)(uintptr_t)tx_buffers[i];
        tx_ring[i].next = (uint32_t)(uintptr_t)&tx_ring[(i + 1u) % ETH_DMA_TX_RING];
    }
    tx_index = 0;
    rx_index = 0;

    DMARDLAR = (uint32_t)(uintptr_t)&rx_ring[0];
    DMATDLAR = (uint32_t)(uintptr_t)&tx_ring[0];

    MACFFR = (1u << 4); /* PAM: pass all multicast (mDNS 224.0.0.251); unicast filtered */
    MACCR = MACCR_TE | MACCR_RE | MACCR_DM | MACCR_FES | MACCR_BL_10;

    DMAOMR |= DMAOMR_ST | DMAOMR_SR;
    DMARPDR = 0;
    DMATPDR = 0;
    return 0;
}

uint8_t *eth_dma_tx_acquire(void) {
    if ((tx_ring[tx_index].status & TDESC_OWN) != 0u) {
        eth_stats.tx_dropped++;
        return NULL; /* DMA still busy with this descriptor */
    }
    return tx_buffers[tx_index];
}

int eth_dma_tx_commit(uint32_t len) {
    if (len == 0u || len > ETH_DMA_BUFFER_SIZE) {
        return -1;
    }
    eth_desc *desc = &tx_ring[tx_index];
    desc->size = len & TDESC_TBS1_MASK;
    desc->status = TDESC_OWN | TDESC_FS | TDESC_LS | TDESC_TCH;
    DMATPDR = 0; /* wake the TX engine */
    tx_index = (tx_index + 1u) % ETH_DMA_TX_RING;
    eth_stats.tx_frames++;
    return 0;
}

int eth_dma_send(const uint8_t *frame, uint32_t len) {
    if (frame == NULL || len == 0u || len > ETH_DMA_BUFFER_SIZE) {
        return -1;
    }
    uint8_t *buffer = eth_dma_tx_acquire();
    if (buffer == NULL) {
        return -1;
    }
    for (uint32_t i = 0; i < len; i++) {
        buffer[i] = frame[i];
    }
    return eth_dma_tx_commit(len);
}

int eth_dma_rx_peek(const uint8_t **frame) {
    eth_desc *desc = &rx_ring[rx_index];
    if ((desc->status & RDESC_OWN) != 0u) {
        return 0; /* DMA has not handed this one back yet */
    }
    uint32_t length = (desc->status & RDESC_FL_MASK) >> RDESC_FL_SHIFT;
    if ((desc->status & RDESC_ES) != 0u || length == 0u || length > ETH_DMA_BUFFER_SIZE ||
        (desc->status & (RDESC_FS | RDESC_LS)) != (RDESC_FS | RDESC_LS)) {
        eth_stats.rx_errors++;
        eth_dma_rx_release();
        return -1;
    }
    eth_stats.rx_frames++;
    *frame = rx_buffers[rx_index];
    return (int)length;
}

void eth_dma_rx_release(void) {
    eth_desc *desc = &rx_ring[rx_index];
    desc->size = RDESC_RCH | (ETH_DMA_BUFFER_SIZE & RDESC_RBS1_MASK);
    desc->status = RDESC_OWN;
    rx_index = (rx_index + 1u) % ETH_DMA_RX_RING;
    DMARPDR = 0;
}

int eth_dma_recv(uint8_t *frame, uint32_t max_len) {
    const uint8_t *src;
    if (frame == NULL || max_len == 0u) {
        return -1;
    }
    int n = eth_dma_rx_peek(&src);
    if (n <= 0) {
        return n;
    }
    if ((uint32_t)n > max_len) {
        eth_dma_rx_release();
        return -1;
    }
    for (int b = 0; b < n; b++) {
        frame[b] = src[b];
    }
    eth_dma_rx_release();
    return n;
}
