/* USB CDC-ACM (virtual COM port) on the STM32F107 OTG_FS core, bare metal.
 *
 * Wiring (RLY-8): USB-B -> PA11 (DM) / PA12 (DP), VBUS sense on PA9 (assumed, ST VCP style).
 * VID/PID 0x0483/0x5740 so the vendor's stmcdc.inf matches on Windows; macOS/Linux need no
 * driver. Clock: USB 48 MHz = PLL VCO 144 MHz / 3 (RCC_CFGR OTGFSPRE = 0), so SYSCLK must be
 * 72 MHz from the PLL before usb_cdc_init().
 *
 * Interrupt driven (OTG_FS_IRQHandler, IRQ 67). The main loop only touches the ring buffers:
 *   usb_cdc_read()  - drains the RX ring (filled by the ISR from bulk OUT EP 0x01)
 *   usb_cdc_write() - fills the TX ring; the ISR / usb_cdc_poll() ship it on bulk IN EP 0x81
 *
 * Build option USB_NO_VBUS_SENSE=1: for boards where PA9 is NOT connected to VBUS (see
 * usb_cdc.c for what it does and why it is uncertain on this core).
 */
#ifndef USB_CDC_H
#define USB_CDC_H

#include <stdint.h>

typedef struct {
    uint32_t irqs;
    uint32_t resets;      /* USBRST seen */
    uint32_t enum_done;   /* ENUMDNE seen */
    uint32_t setups;      /* SETUP packets handled */
    uint32_t stalls;      /* unsupported requests answered with STALL */
    uint32_t configured;  /* SET_CONFIGURATION(1) count */
    uint32_t rx_bytes;    /* bytes received on EP 0x01 */
    uint32_t rx_dropped;  /* bytes lost because the RX ring was full */
    uint32_t tx_bytes;    /* bytes queued into the IN FIFO on EP 0x81 */
    uint32_t sessions_ended; /* GOTGINT SEDET (cable pulled) */
    uint32_t last_gintsts;
    uint32_t last_setup[2]; /* raw 8-byte SETUP packet */
} usb_cdc_stats_t;

extern volatile usb_cdc_stats_t usb_cdc_stats;

void usb_cdc_init(void);
int usb_cdc_read(uint8_t *buf, int cap);
int usb_cdc_write(const uint8_t *buf, int len);
int usb_cdc_configured(void);
int usb_cdc_dtr(void);
void usb_cdc_poll(void);

/* Installed in the vector table (app/startup.c) at index 16 + 67. */
void OTG_FS_IRQHandler(void);

#endif /* USB_CDC_H */
