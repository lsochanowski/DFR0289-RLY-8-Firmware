/* USB CDC-ACM device on the STM32F105/107 OTG_FS core (RM0008 §28), no HAL.
 *
 * Programming model followed (RM0008 "OTG_FS programming model", device mode):
 *   core init  -> soft reset, PWRDWN (transceiver on), VBUSBSEN, FDMOD, TRDT
 *   device init-> DCFG full speed, FIFO sizing, interrupt masks, DCTL.SDIS = 0 (pull-up on)
 *   USBRST     -> NAK all OUT EPs, unmask EP0 IN/OUT, DAD = 0, arm EP0 for SETUP
 *   ENUMDNE    -> EP0 MPS = 64, clear global IN NAK
 *   RXFLVL     -> pop GRXSTSP; SETUP data (6) and OUT data (2) are read from the shared FIFO
 *   OEPINT     -> EP0: STUP => decode request; XFRC => end of OUT data / status stage
 *                 EP1: XFRC => re-arm (or pause while the RX ring is full)
 *   IEPINT     -> XFRC => next packet of the EP0 data stage / next bulk IN packet
 *
 * IN transfers are always one packet at a time (DIEPTSIZ PKTCNT = 1), written to the FIFO
 * right after enabling the endpoint. The FIFOs are sized so that one max-size packet always
 * fits (TX0 128 B, TX1 256 B), so no TXFE interrupt is needed.
 */
#include "usb_cdc.h"

#include <stddef.h>

#define REG(addr) (*(volatile uint32_t *)(addr))

/* --- RCC / GPIO / NVIC --- */
#define RCC_AHBENR REG(0x40021014u)
#define RCC_APB2ENR REG(0x40021018u)
#define RCC_AHBENR_OTGFSEN (1u << 12)
#define GPIOA_CRH REG(0x40010804u)
#define GPIOA_BSRR REG(0x40010810u)
#define NVIC_ISER2 REG(0xE000E108u)
#define NVIC_IPR16 REG(0xE000E410u) /* IRQ 64..67, byte 3 = IRQ 67 */

/* --- OTG_FS (base 0x5000_0000) --- */
#define OTG 0x50000000u
#define GOTGCTL REG(OTG + 0x000u)
#define GOTGINT REG(OTG + 0x004u)
#define GAHBCFG REG(OTG + 0x008u)
#define GUSBCFG REG(OTG + 0x00Cu)
#define GRSTCTL REG(OTG + 0x010u)
#define GINTSTS REG(OTG + 0x014u)
#define GINTMSK REG(OTG + 0x018u)
#define GRXSTSP REG(OTG + 0x020u)
#define GRXFSIZ REG(OTG + 0x024u)
#define DIEPTXF0 REG(OTG + 0x028u)
#define GCCFG REG(OTG + 0x038u)
#define DIEPTXF(n) REG(OTG + 0x104u + 4u * ((n) - 1u))
#define DCFG REG(OTG + 0x800u)
#define DCTL REG(OTG + 0x804u)
#define DSTS REG(OTG + 0x808u)
#define DIEPMSK REG(OTG + 0x810u)
#define DOEPMSK REG(OTG + 0x814u)
#define DAINT REG(OTG + 0x818u)
#define DAINTMSK REG(OTG + 0x81Cu)
#define DIEPEMPMSK REG(OTG + 0x834u)
#define DIEPCTL(n) REG(OTG + 0x900u + 0x20u * (n))
#define DIEPINT(n) REG(OTG + 0x908u + 0x20u * (n))
#define DIEPTSIZ(n) REG(OTG + 0x910u + 0x20u * (n))
#define DTXFSTS(n) REG(OTG + 0x918u + 0x20u * (n))
#define DOEPCTL(n) REG(OTG + 0xB00u + 0x20u * (n))
#define DOEPINT(n) REG(OTG + 0xB08u + 0x20u * (n))
#define DOEPTSIZ(n) REG(OTG + 0xB10u + 0x20u * (n))
#define PCGCCTL REG(OTG + 0xE00u)
#define FIFO(n) REG(OTG + 0x1000u + 0x1000u * (n))

/* GRSTCTL */
#define GRSTCTL_CSRST (1u << 0)
#define GRSTCTL_RXFFLSH (1u << 4)
#define GRSTCTL_TXFFLSH (1u << 5)
#define GRSTCTL_TXFNUM(n) ((uint32_t)(n) << 6)
#define GRSTCTL_AHBIDL (1u << 31)
/* GUSBCFG */
#define GUSBCFG_PHYSEL (1u << 6)
#define GUSBCFG_TRDT(n) ((uint32_t)(n) << 10)
#define GUSBCFG_TRDT_MASK (0xFu << 10)
#define GUSBCFG_FHMOD (1u << 29)
#define GUSBCFG_FDMOD (1u << 30)
/* GCCFG */
#define GCCFG_PWRDWN (1u << 16)
#define GCCFG_VBUSBSEN (1u << 19)
/* GINTSTS / GINTMSK */
#define GINT_OTGINT (1u << 2)
#define GINT_RXFLVL (1u << 4)
#define GINT_USBSUSP (1u << 11)
#define GINT_USBRST (1u << 12)
#define GINT_ENUMDNE (1u << 13)
#define GINT_IEPINT (1u << 18)
#define GINT_OEPINT (1u << 19)
#define GINT_SRQINT (1u << 30)
#define GINT_WKUINT (1u << 31)
#define GOTGINT_SEDET (1u << 2)
/* DCTL */
#define DCTL_RWUSIG (1u << 0)
#define DCTL_SDIS (1u << 1)
#define DCTL_CGINAK (1u << 8)
/* DIEPCTL / DOEPCTL */
#define EPCTL_USBAEP (1u << 15)
#define EPCTL_EPTYP_BULK (2u << 18)
#define EPCTL_EPTYP_INTR (3u << 18)
#define EPCTL_STALL (1u << 21)
#define EPCTL_TXFNUM(n) ((uint32_t)(n) << 22)
#define EPCTL_CNAK (1u << 26)
#define EPCTL_SNAK (1u << 27)
#define EPCTL_SD0PID (1u << 28)
#define EPCTL_EPDIS (1u << 30)
#define EPCTL_EPENA (1u << 31)
/* DIEPINT / DOEPINT */
#define EPINT_XFRC (1u << 0)
#define EPINT_TOC (1u << 3)  /* IN */
#define EPINT_STUP (1u << 3) /* OUT */
/* DxEPTSIZ */
#define TSIZ_PKTCNT1 (1u << 19)
#define TSIZ_STUPCNT3 (3u << 29)
/* GRXSTSP */
#define RXSTS_EPNUM(x) ((x) & 0xFu)
#define RXSTS_BCNT(x) (((x) >> 4) & 0x7FFu)
#define RXSTS_PKTSTS(x) (((x) >> 17) & 0xFu)
#define PKTSTS_OUT_DATA 2u
#define PKTSTS_SETUP_DATA 6u

/* --- FIFO layout (words; 320 words total on OTG_FS) --- */
#define RX_FIFO_WORDS 128u
#define TX0_FIFO_WORDS 32u
#define TX1_FIFO_WORDS 64u
#define TX2_FIFO_WORDS 16u

#define EP0_MPS 64u
#define BULK_MPS 64u
#define NOTIFY_MPS 8u

#define RX_RING 512u /* powers of two: index masks below */
#define TX_RING 1024u

volatile usb_cdc_stats_t usb_cdc_stats;

/* --- descriptors --- */
static const uint8_t dev_desc[18] = {
    18,   1,          /* bLength, DEVICE */
    0x00, 0x02,       /* bcdUSB 2.00 */
    0x02, 0x02, 0x00, /* class CDC, subclass ACM, protocol none (ST VCP layout) */
    EP0_MPS,          /* bMaxPacketSize0 */
    0x83, 0x04,       /* idVendor 0x0483 (STMicroelectronics) */
    0x40, 0x57,       /* idProduct 0x5740 (Virtual COM Port) */
    0x00, 0x02,       /* bcdDevice 2.00 */
    1,    2,    3,    /* iManufacturer, iProduct, iSerialNumber */
    1,                /* bNumConfigurations */
};

#define CFG_TOTAL 67u
static const uint8_t cfg_desc[CFG_TOTAL] = {
    /* configuration */
    9, 2, CFG_TOTAL, 0, 2, 1, 0, 0x80, 50, /* bus powered, 100 mA */
    /* interface 0: communication class, one interrupt IN for notifications */
    9, 4, 0, 0, 1, 0x02, 0x02, 0x01, 0,
    5, 0x24, 0x00, 0x10, 0x01, /* CDC header, bcdCDC 1.10 */
    5, 0x24, 0x01, 0x00, 0x01, /* call management: no call mgmt, data interface 1 */
    4, 0x24, 0x02, 0x02,       /* ACM: SET/GET_LINE_CODING + SET_CONTROL_LINE_STATE */
    5, 0x24, 0x06, 0x00, 0x01, /* union: master 0, slave 1 */
    7, 5, 0x82, 0x03, NOTIFY_MPS, 0, 0x10,
    /* interface 1: data class, bulk OUT + bulk IN */
    9, 4, 1, 0, 2, 0x0A, 0x00, 0x00, 0,
    7, 5, 0x01, 0x02, BULK_MPS, 0, 0,
    7, 5, 0x81, 0x02, BULK_MPS, 0, 0,
};

static const uint8_t lang_desc[4] = {4, 3, 0x09, 0x04}; /* en-US */
static const char str_manufacturer[] = "RLY-8 open firmware";
/* This board has no usable VBUS on PA9, see usb_cdc_init(). */
#ifndef USB_NO_VBUS_SENSE
#define USB_NO_VBUS_SENSE 1
#endif

static const char str_product[] = "RLY-8 Console";

/* --- state --- */
typedef enum { EP0_IDLE = 0, EP0_DATA_IN, EP0_DATA_OUT, EP0_STATUS_IN, EP0_STATUS_OUT } ep0_state_t;

static volatile ep0_state_t ep0_state;
static const uint8_t *ep0_tx_ptr;
static uint32_t ep0_tx_left;
static int ep0_tx_zlp;
static uint8_t ep0_buf[64] __attribute__((aligned(4))); /* string descriptors, small replies */
static uint8_t ep0_out[64] __attribute__((aligned(4)));
static uint32_t ep0_out_len;
static uint8_t setup_pkt[8] __attribute__((aligned(4)));
static uint8_t line_coding[7] = {0x00, 0xC2, 0x01, 0x00, 0, 0, 8}; /* 115200 8N1 */
static char serial_str[25];

static volatile int configured;
static volatile int dtr;

static uint8_t rx_ring[RX_RING];
static volatile uint32_t rx_head; /* ISR writes */
static volatile uint32_t rx_tail; /* main reads */
static volatile int rx_paused;    /* EP1 OUT not re-armed because the ring is nearly full */

static uint8_t tx_ring[TX_RING];
static volatile uint32_t tx_head; /* main writes */
static volatile uint32_t tx_tail; /* ISR / poll consume */
static volatile int tx_busy;      /* one bulk IN packet in flight */
static volatile int tx_zlp_due;   /* last packet was full-size and the ring is empty */

static void spin_ms(uint32_t ms) {
    for (uint32_t i = 0; i < ms; i++) {
        for (volatile uint32_t j = 0; j < 7000u; j++) {
        }
    }
}

static inline uint32_t irq_save(void) {
    uint32_t primask;
    __asm volatile("mrs %0, primask\n cpsid i" : "=r"(primask)::"memory");
    return primask;
}

static inline void irq_restore(uint32_t primask) {
    __asm volatile("msr primask, %0" ::"r"(primask) : "memory");
}

/* --- FIFO helpers --- */

static void fifo_write(uint32_t ep, const uint8_t *src, uint32_t len) {
    uint32_t words = (len + 3u) / 4u;
    for (uint32_t w = 0; w < words; w++) {
        uint32_t v = 0;
        for (uint32_t b = 0; b < 4u; b++) {
            uint32_t i = w * 4u + b;
            if (i < len) {
                v |= (uint32_t)src[i] << (8u * b);
            }
        }
        FIFO(ep) = v;
    }
}

/* Bulk IN reads from the TX ring, which may wrap inside a packet. */
static void fifo_write_ring(uint32_t len) {
    uint32_t t = tx_tail;
    uint32_t words = (len + 3u) / 4u;
    for (uint32_t w = 0; w < words; w++) {
        uint32_t v = 0;
        for (uint32_t b = 0; b < 4u; b++) {
            if (w * 4u + b < len) {
                v |= (uint32_t)tx_ring[t & (TX_RING - 1u)] << (8u * b);
                t++;
            }
        }
        FIFO(1) = v;
    }
    tx_tail = t;
}

static void fifo_read(uint8_t *dst, uint32_t len) {
    uint32_t words = (len + 3u) / 4u;
    for (uint32_t w = 0; w < words; w++) {
        uint32_t v = FIFO(0);
        for (uint32_t b = 0; b < 4u; b++) {
            uint32_t i = w * 4u + b;
            if (i < len && dst != NULL) {
                dst[i] = (uint8_t)(v >> (8u * b));
            }
        }
    }
}

static void fifo_read_to_rx_ring(uint32_t len) {
    uint32_t words = (len + 3u) / 4u;
    uint32_t h = rx_head;
    for (uint32_t w = 0; w < words; w++) {
        uint32_t v = FIFO(0);
        for (uint32_t b = 0; b < 4u; b++) {
            if (w * 4u + b >= len) {
                break;
            }
            if ((h - rx_tail) < RX_RING) {
                rx_ring[h & (RX_RING - 1u)] = (uint8_t)(v >> (8u * b));
                h++;
                usb_cdc_stats.rx_bytes++;
            } else {
                usb_cdc_stats.rx_dropped++;
            }
        }
    }
    rx_head = h;
}

static void flush_tx_fifo(uint32_t num) {
    GRSTCTL = GRSTCTL_TXFFLSH | GRSTCTL_TXFNUM(num);
    for (volatile uint32_t spin = 0; spin < 200000u; spin++) {
        if ((GRSTCTL & GRSTCTL_TXFFLSH) == 0u) {
            break;
        }
    }
}

static void flush_rx_fifo(void) {
    GRSTCTL = GRSTCTL_RXFFLSH;
    for (volatile uint32_t spin = 0; spin < 200000u; spin++) {
        if ((GRSTCTL & GRSTCTL_RXFFLSH) == 0u) {
            break;
        }
    }
}

/* --- EP0 --- */

/* SETUP reception: the core accepts SETUP whenever STUPCNT > 0, even if EP0 OUT is not
 * enabled (RM0008: "the core always accepts SETUP packets"). */
static void ep0_arm_setup(void) {
    DOEPTSIZ(0) = TSIZ_STUPCNT3 | TSIZ_PKTCNT1 | (3u * 8u);
}

/* EP0 OUT for a data stage or the zero-length status stage. */
static void ep0_arm_out(void) {
    DOEPTSIZ(0) = TSIZ_STUPCNT3 | TSIZ_PKTCNT1 | EP0_MPS;
    DOEPCTL(0) |= EPCTL_CNAK | EPCTL_EPENA;
}

static void ep0_send_packet(void) {
    uint32_t n = ep0_tx_left > EP0_MPS ? EP0_MPS : ep0_tx_left;
    DIEPTSIZ(0) = TSIZ_PKTCNT1 | n;
    DIEPCTL(0) |= EPCTL_CNAK | EPCTL_EPENA;
    if (n != 0u) {
        fifo_write(0, ep0_tx_ptr, n);
    }
    ep0_tx_ptr += n;
    ep0_tx_left -= n;
}

/* Starts an IN data stage, trimmed to wLength; adds a ZLP when the reply is shorter than
 * requested and ends exactly on a packet boundary. */
static void ep0_send(const uint8_t *data, uint32_t len, uint32_t wlength) {
    if (len > wlength) {
        len = wlength;
    }
    ep0_tx_ptr = data;
    ep0_tx_left = len;
    ep0_tx_zlp = (len < wlength) && (len != 0u) && ((len % EP0_MPS) == 0u);
    ep0_state = EP0_DATA_IN;
    ep0_send_packet();
}

static void ep0_send_status(void) {
    ep0_state = EP0_STATUS_IN;
    DIEPTSIZ(0) = TSIZ_PKTCNT1;
    DIEPCTL(0) |= EPCTL_CNAK | EPCTL_EPENA;
}

static void ep0_stall(void) {
    DIEPCTL(0) |= EPCTL_STALL;
    DOEPCTL(0) |= EPCTL_STALL;
    ep0_state = EP0_IDLE;
    usb_cdc_stats.stalls++;
    ep0_arm_setup();
}

static uint32_t make_string(const char *s) {
    uint32_t n = 0;
    while (s[n] != '\0' && n < 31u) {
        ep0_buf[2u + 2u * n] = (uint8_t)s[n];
        ep0_buf[3u + 2u * n] = 0;
        n++;
    }
    ep0_buf[0] = (uint8_t)(2u + 2u * n);
    ep0_buf[1] = 3;
    return 2u + 2u * n;
}

/* --- bulk endpoints --- */

static void ep1_arm_out(void) {
    DOEPTSIZ(1) = TSIZ_PKTCNT1 | BULK_MPS;
    DOEPCTL(1) |= EPCTL_CNAK | EPCTL_EPENA;
}

static void activate_data_endpoints(void) {
    DIEPCTL(1) = EPCTL_USBAEP | EPCTL_EPTYP_BULK | EPCTL_TXFNUM(1) | EPCTL_SD0PID | BULK_MPS;
    DIEPCTL(2) = EPCTL_USBAEP | EPCTL_EPTYP_INTR | EPCTL_TXFNUM(2) | EPCTL_SD0PID | NOTIFY_MPS;
    DOEPCTL(1) = EPCTL_USBAEP | EPCTL_EPTYP_BULK | EPCTL_SD0PID | BULK_MPS;
    flush_tx_fifo(1);
    flush_tx_fifo(2);
    DAINTMSK |= (1u << 1) | (1u << 2) | (1u << 17);
    rx_head = rx_tail = 0;
    rx_paused = 0;
    tx_busy = 0;
    tx_zlp_due = 0;
    ep1_arm_out();
}

static void deactivate_data_endpoints(void) {
    for (uint32_t ep = 1; ep <= 2u; ep++) {
        if (DIEPCTL(ep) & EPCTL_EPENA) {
            DIEPCTL(ep) |= EPCTL_EPDIS | EPCTL_SNAK;
        }
        DIEPCTL(ep) &= ~EPCTL_USBAEP;
    }
    if (DOEPCTL(1) & EPCTL_EPENA) {
        DOEPCTL(1) |= EPCTL_EPDIS | EPCTL_SNAK;
    }
    DOEPCTL(1) &= ~EPCTL_USBAEP;
    DAINTMSK &= ~((1u << 1) | (1u << 2) | (1u << 17));
    tx_busy = 0;
}

/* Ships the next bulk IN packet from the TX ring. Caller holds interrupts off (or is the
 * ISR) and has checked !tx_busy. */
static void tx_kick(void) {
    uint32_t avail = tx_head - tx_tail;
    if (avail == 0u && !tx_zlp_due) {
        return;
    }
    uint32_t n = avail > BULK_MPS ? BULK_MPS : avail;
    tx_busy = 1;
    tx_zlp_due = (n == BULK_MPS) && (avail == BULK_MPS);
    DIEPTSIZ(1) = TSIZ_PKTCNT1 | n;
    DIEPCTL(1) |= EPCTL_CNAK | EPCTL_EPENA;
    if (n != 0u) {
        fifo_write_ring(n);
        usb_cdc_stats.tx_bytes += n;
    }
}

/* --- request decoding --- */

static void handle_setup(void) {
    uint8_t bm = setup_pkt[0];
    uint8_t req = setup_pkt[1];
    uint16_t wvalue = (uint16_t)(setup_pkt[2] | (setup_pkt[3] << 8));
    uint16_t wlength = (uint16_t)(setup_pkt[6] | (setup_pkt[7] << 8));
    uint8_t type = (uint8_t)(bm & 0x60u);

    usb_cdc_stats.setups++;
    usb_cdc_stats.last_setup[0] = (uint32_t)setup_pkt[0] | ((uint32_t)setup_pkt[1] << 8) |
                                  ((uint32_t)setup_pkt[2] << 16) | ((uint32_t)setup_pkt[3] << 24);
    usb_cdc_stats.last_setup[1] = (uint32_t)setup_pkt[4] | ((uint32_t)setup_pkt[5] << 8) |
                                  ((uint32_t)setup_pkt[6] << 16) | ((uint32_t)setup_pkt[7] << 24);

    if (type == 0x00u) { /* standard */
        switch (req) {
        case 0x06: { /* GET_DESCRIPTOR */
            uint8_t dtype = (uint8_t)(wvalue >> 8);
            uint8_t idx = (uint8_t)(wvalue & 0xFFu);
            if (dtype == 1u) {
                ep0_send(dev_desc, sizeof dev_desc, wlength);
            } else if (dtype == 2u) {
                ep0_send(cfg_desc, sizeof cfg_desc, wlength);
            } else if (dtype == 3u && idx == 0u) {
                ep0_send(lang_desc, sizeof lang_desc, wlength);
            } else if (dtype == 3u && idx <= 3u) {
                const char *s = idx == 1u ? str_manufacturer : idx == 2u ? str_product : serial_str;
                ep0_send(ep0_buf, make_string(s), wlength);
            } else {
                ep0_stall(); /* device qualifier etc.: full-speed only device */
            }
            return;
        }
        case 0x05: /* SET_ADDRESS: OTG core takes the address now, status goes out at addr 0 */
            DCFG = (DCFG & ~(0x7Fu << 4)) | ((uint32_t)(wvalue & 0x7Fu) << 4);
            ep0_send_status();
            return;
        case 0x09: /* SET_CONFIGURATION */
            if (wvalue == 1u) {
                activate_data_endpoints();
                configured = 1;
                usb_cdc_stats.configured++;
            } else if (wvalue == 0u) {
                deactivate_data_endpoints();
                configured = 0;
                dtr = 0;
            } else {
                ep0_stall();
                return;
            }
            ep0_send_status();
            return;
        case 0x08: /* GET_CONFIGURATION */
            ep0_buf[0] = configured ? 1u : 0u;
            ep0_send(ep0_buf, 1, wlength);
            return;
        case 0x00: /* GET_STATUS (device / interface / endpoint): all zero */
            ep0_buf[0] = 0;
            ep0_buf[1] = 0;
            ep0_send(ep0_buf, 2, wlength);
            return;
        case 0x01: /* CLEAR_FEATURE */
        case 0x03: /* SET_FEATURE */
            if ((bm & 0x1Fu) == 2u && req == 0x01u) { /* endpoint halt cleared: reset toggle */
                uint8_t ep = setup_pkt[4];
                if (ep == 0x81u || ep == 0x82u) {
                    DIEPCTL(ep & 0x0Fu) = (DIEPCTL(ep & 0x0Fu) & ~EPCTL_STALL) | EPCTL_SD0PID;
                } else if (ep == 0x01u) {
                    DOEPCTL(1) = (DOEPCTL(1) & ~EPCTL_STALL) | EPCTL_SD0PID;
                }
            }
            ep0_send_status();
            return;
        case 0x0A: /* GET_INTERFACE */
            ep0_buf[0] = 0;
            ep0_send(ep0_buf, 1, wlength);
            return;
        case 0x0B: /* SET_INTERFACE (alt 0 only) */
            ep0_send_status();
            return;
        default:
            break;
        }
    } else if (type == 0x20u) { /* class, interface */
        switch (req) {
        case 0x20: /* SET_LINE_CODING: 7 bytes follow in the OUT data stage */
            if (wlength == 0u || wlength > sizeof ep0_out) {
                ep0_stall();
                return;
            }
            ep0_out_len = 0;
            ep0_state = EP0_DATA_OUT;
            ep0_arm_out();
            return;
        case 0x21: /* GET_LINE_CODING */
            ep0_send(line_coding, sizeof line_coding, wlength);
            return;
        case 0x22: /* SET_CONTROL_LINE_STATE */
            dtr = (wvalue & 1u) != 0u;
            ep0_send_status();
            return;
        case 0x23: /* SEND_BREAK */
            ep0_send_status();
            return;
        default:
            break;
        }
    }
    ep0_stall();
}

/* --- interrupt handling --- */

static void on_usb_reset(void) {
    usb_cdc_stats.resets++;
    DCTL &= ~DCTL_RWUSIG;
    flush_tx_fifo(0x10u); /* all TX FIFOs */
    for (uint32_t ep = 0; ep < 4u; ep++) {
        DIEPINT(ep) = 0xFFu;
        DOEPINT(ep) = 0xFFu;
        DOEPCTL(ep) |= EPCTL_SNAK;
    }
    deactivate_data_endpoints();
    DAINT = 0xFFFFFFFFu;
    DAINTMSK = (1u << 0) | (1u << 16);
    DOEPMSK = EPINT_XFRC | EPINT_STUP;
    DIEPMSK = EPINT_XFRC | EPINT_TOC;
    DCFG &= ~(0x7Fu << 4); /* address 0 */
    configured = 0;
    dtr = 0;
    ep0_state = EP0_IDLE;
    ep0_arm_setup();
}

static void on_enum_done(void) {
    usb_cdc_stats.enum_done++;
    DIEPCTL(0) &= ~0x3u; /* MPSIZ = 00: 64 bytes */
    DCTL |= DCTL_CGINAK;
    GUSBCFG = (GUSBCFG & ~GUSBCFG_TRDT_MASK) | GUSBCFG_TRDT(9);
}

static void on_rx_fifo(void) {
    uint32_t sts = GRXSTSP;
    uint32_t ep = RXSTS_EPNUM(sts);
    uint32_t bcnt = RXSTS_BCNT(sts);
    switch (RXSTS_PKTSTS(sts)) {
    case PKTSTS_SETUP_DATA:
        fifo_read(setup_pkt, bcnt > 8u ? 8u : bcnt);
        if (bcnt > 8u) {
            fifo_read(NULL, bcnt - 8u);
        }
        break;
    case PKTSTS_OUT_DATA:
        if (ep == 0u) {
            uint32_t room = sizeof ep0_out - ep0_out_len;
            uint32_t n = bcnt > room ? room : bcnt;
            fifo_read(&ep0_out[ep0_out_len], n);
            if (bcnt > n) {
                fifo_read(NULL, bcnt - n);
            }
            ep0_out_len += n;
        } else if (ep == 1u) {
            fifo_read_to_rx_ring(bcnt);
        } else {
            fifo_read(NULL, bcnt);
        }
        break;
    default: /* OUT/SETUP transfer completed, global NAK: nothing to pop */
        break;
    }
}

static void on_out_ep(void) {
    uint32_t pending = (DAINT & DAINTMSK) >> 16;
    if (pending & 1u) {
        uint32_t st = DOEPINT(0);
        DOEPINT(0) = st;
        if (st & EPINT_XFRC) {
            if (ep0_state == EP0_DATA_OUT) {
                for (uint32_t i = 0; i < sizeof line_coding && i < ep0_out_len; i++) {
                    line_coding[i] = ep0_out[i];
                }
                ep0_send_status();
            } else if (ep0_state == EP0_STATUS_OUT) {
                ep0_state = EP0_IDLE;
                ep0_arm_setup();
            }
        }
        if (st & EPINT_STUP) {
            ep0_state = EP0_IDLE;
            handle_setup();
        }
    }
    if (pending & 2u) {
        uint32_t st = DOEPINT(1);
        DOEPINT(1) = st;
        if (st & EPINT_XFRC) {
            if ((RX_RING - (rx_head - rx_tail)) >= BULK_MPS) {
                ep1_arm_out();
            } else {
                rx_paused = 1; /* usb_cdc_read() frees space, usb_cdc_poll() re-arms */
            }
        }
    }
}

static void on_in_ep(void) {
    uint32_t pending = DAINT & DAINTMSK & 0xFFFFu;
    if (pending & 1u) {
        uint32_t st = DIEPINT(0);
        DIEPINT(0) = st;
        if (st & EPINT_XFRC) {
            if (ep0_state == EP0_DATA_IN) {
                if (ep0_tx_left > 0u) {
                    ep0_send_packet();
                } else if (ep0_tx_zlp) {
                    ep0_tx_zlp = 0;
                    ep0_send_packet(); /* zero-length */
                } else {
                    ep0_state = EP0_STATUS_OUT;
                    ep0_arm_out();
                }
            } else if (ep0_state == EP0_STATUS_IN) {
                ep0_state = EP0_IDLE;
                ep0_arm_setup();
            }
        }
    }
    if (pending & 2u) {
        uint32_t st = DIEPINT(1);
        DIEPINT(1) = st;
        if (st & EPINT_XFRC) {
            tx_busy = 0;
            if (configured) {
                tx_kick();
            }
        }
    }
    if (pending & 4u) {
        DIEPINT(2) = DIEPINT(2);
    }
}

void OTG_FS_IRQHandler(void) {
    uint32_t sts = GINTSTS & GINTMSK;
    usb_cdc_stats.irqs++;
    usb_cdc_stats.last_gintsts = sts;

    if (sts & GINT_USBRST) {
        GINTSTS = GINT_USBRST;
        on_usb_reset();
    }
    if (sts & GINT_ENUMDNE) {
        GINTSTS = GINT_ENUMDNE;
        on_enum_done();
    }
    if (sts & GINT_RXFLVL) {
        GINTMSK &= ~GINT_RXFLVL;
        on_rx_fifo();
        GINTMSK |= GINT_RXFLVL;
    }
    if (sts & GINT_OEPINT) {
        on_out_ep();
    }
    if (sts & GINT_IEPINT) {
        on_in_ep();
    }
    if (sts & GINT_OTGINT) {
        uint32_t o = GOTGINT;
        GOTGINT = o;
        if (o & GOTGINT_SEDET) { /* VBUS gone: cable unplugged */
            usb_cdc_stats.sessions_ended++;
            configured = 0;
            dtr = 0;
            tx_busy = 0;
        }
    }
    if (sts & (GINT_USBSUSP | GINT_SRQINT | GINT_WKUINT)) {
        GINTSTS = sts & (GINT_USBSUSP | GINT_SRQINT | GINT_WKUINT);
    }
}

/* --- public API --- */

void usb_cdc_init(void) {
    static const char hex[] = "0123456789ABCDEF";
    const volatile uint8_t *uid = (const volatile uint8_t *)0x1FFFF7E8u;
    for (uint32_t i = 0; i < 12u; i++) {
        serial_str[2u * i] = hex[uid[i] >> 4];
        serial_str[2u * i + 1u] = hex[uid[i] & 0xFu];
    }
    serial_str[24] = '\0';

    RCC_APB2ENR |= (1u << 2); /* IOPAEN; PA11/PA12 are taken over by the OTG PHY */
#if defined(USB_NO_VBUS_SENSE) && USB_NO_VBUS_SENSE
    /* RLY-8 (measured 2026-11-08): with the USB cable plugged in PA9 reads 0 even with the
     * internal pull-up on (it is pulled down on the board, no VBUS on it), GOTGCTL.BSVLD
     * stays 0 and the device never connects. Driving PA9 high made BSVLD=1 and the Mac
     * enumerated the console at once. Original note:
     * WORKAROUND. The F105/107 OTG core has no NOVBUSSENS / BVALOEN override
     * (those exist only on newer cores, e.g. F4 rev Z+, and are absent from stm32f107xc.h).
     * With VBUSBSEN set the B-session comparator samples the PA9 pad whatever its GPIO mode,
     * so driving PA9 high as a push-pull output fakes "VBUS present". Only use this if PA9 is
     * really unconnected to VBUS: if VBUS is wired to PA9 this fights the divider. */
    GPIOA_CRH = (GPIOA_CRH & ~(0xFu << 4)) | (0x2u << 4); /* PA9 output PP 2 MHz */
    GPIOA_BSRR = 1u << 9;
#endif
    RCC_AHBENR |= RCC_AHBENR_OTGFSEN;
    spin_ms(2);

    /* Core soft reset (wait for the AHB master to go idle first). */
    for (volatile uint32_t spin = 0; spin < 200000u; spin++) {
        if (GRSTCTL & GRSTCTL_AHBIDL) {
            break;
        }
    }
    GRSTCTL |= GRSTCTL_CSRST;
    for (volatile uint32_t spin = 0; spin < 200000u; spin++) {
        if ((GRSTCTL & GRSTCTL_CSRST) == 0u) {
            break;
        }
    }
    spin_ms(1);

    /* Transceiver on + VBUS sensing for the B-device (PA9). */
    GCCFG = GCCFG_PWRDWN | GCCFG_VBUSBSEN;
    spin_ms(2);

    /* Force device mode (takes effect after ~25 ms), full-speed PHY, turnaround 9 at 72 MHz
     * AHB (value used by ST's F105/107 USB library). */
    GUSBCFG = (GUSBCFG & ~(GUSBCFG_FHMOD | GUSBCFG_TRDT_MASK)) | GUSBCFG_FDMOD | GUSBCFG_PHYSEL |
              GUSBCFG_TRDT(9);
    spin_ms(50);

    PCGCCTL = 0;
    DCFG = (DCFG & ~0x1FF3u) | 0x3u; /* DSPD = 11 full speed, DAD = 0, PFIVL = 80% */
    DCTL |= DCTL_SDIS;               /* stay disconnected until configured */

    GRXFSIZ = RX_FIFO_WORDS;
    DIEPTXF0 = (TX0_FIFO_WORDS << 16) | RX_FIFO_WORDS;
    DIEPTXF(1) = (TX1_FIFO_WORDS << 16) | (RX_FIFO_WORDS + TX0_FIFO_WORDS);
    DIEPTXF(2) = (TX2_FIFO_WORDS << 16) | (RX_FIFO_WORDS + TX0_FIFO_WORDS + TX1_FIFO_WORDS);
    DIEPTXF(3) = (16u << 16) | (RX_FIFO_WORDS + TX0_FIFO_WORDS + TX1_FIFO_WORDS + TX2_FIFO_WORDS);
    flush_tx_fifo(0x10u);
    flush_rx_fifo();

    DIEPMSK = 0;
    DOEPMSK = 0;
    DIEPEMPMSK = 0;
    DAINT = 0xFFFFFFFFu;
    DAINTMSK = 0;
    for (uint32_t ep = 0; ep < 4u; ep++) {
        if (DIEPCTL(ep) & EPCTL_EPENA) {
            DIEPCTL(ep) = EPCTL_EPDIS | EPCTL_SNAK;
        } else {
            DIEPCTL(ep) = 0;
        }
        DIEPTSIZ(ep) = 0;
        DIEPINT(ep) = 0xFFu;
        if (DOEPCTL(ep) & EPCTL_EPENA) {
            DOEPCTL(ep) = EPCTL_EPDIS | EPCTL_SNAK;
        } else {
            DOEPCTL(ep) = 0;
        }
        DOEPTSIZ(ep) = 0;
        DOEPINT(ep) = 0xFFu;
    }

    GOTGINT = 0xFFFFFFFFu;
    GINTSTS = 0xFFFFFFFFu;
    GINTMSK = GINT_USBRST | GINT_ENUMDNE | GINT_RXFLVL | GINT_IEPINT | GINT_OEPINT | GINT_OTGINT;
    GAHBCFG = 1u; /* GINTMSK: global interrupt enable, no DMA (F1 core has none) */

    NVIC_IPR16 = (NVIC_IPR16 & 0x00FFFFFFu) | (0xC0u << 24); /* IRQ 67: low priority */
    NVIC_ISER2 = 1u << (67u - 64u);

    DCTL &= ~DCTL_SDIS; /* connect: DP pull-up on (once a B-session is valid) */
}

int usb_cdc_read(uint8_t *buf, int cap) {
    int n = 0;
    while (n < cap && rx_tail != rx_head) {
        buf[n++] = rx_ring[rx_tail & (RX_RING - 1u)];
        rx_tail = rx_tail + 1u;
    }
    return n;
}

int usb_cdc_write(const uint8_t *buf, int len) {
    if (!configured) {
        return len; /* nobody listening: discard so callers never block */
    }
    int n = 0;
    while (n < len && (tx_head - tx_tail) < TX_RING) {
        tx_ring[tx_head & (TX_RING - 1u)] = buf[n++];
        tx_head = tx_head + 1u;
    }
    return n;
}

int usb_cdc_configured(void) { return configured; }

int usb_cdc_dtr(void) { return dtr; }

void usb_cdc_poll(void) {
    if (!configured) {
        return;
    }
    uint32_t pm = irq_save();
    if (!tx_busy) {
        tx_kick();
    }
    if (rx_paused && (RX_RING - (rx_head - rx_tail)) >= BULK_MPS) {
        rx_paused = 0;
        ep1_arm_out();
    }
    irq_restore(pm);
}
