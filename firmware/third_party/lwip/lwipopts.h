/* lwIP configuration for the RLY-8 replacement firmware (STM32F107VC, 64 KB RAM, no OS).
 *
 * Deliberately conservative on buffers: web panel (port 80, 2 slots), one MQTT client,
 * UDP for DHCP, DNS and mDNS; a single netif.
 */
#ifndef LWIPOPTS_H
#define LWIPOPTS_H

/* --- core mode --- */
#define NO_SYS 1
#define SYS_LIGHTWEIGHT_PROT 1
#define LWIP_NETCONN 0
#define LWIP_SOCKET 0
#define LWIP_TIMERS 1
#define LWIP_SINGLE_NETIF 1

/* --- memory --- */
#define MEM_ALIGNMENT 4
#define MEM_SIZE (12 * 1024)
#define MEMP_NUM_PBUF 8
#define MEMP_NUM_TCP_PCB 8 /* MQTT + 2 HTTP slots + 4 queued HTTP + TIME_WAIT */
#define MEMP_NUM_TCP_PCB_LISTEN 2
#define MEMP_NUM_TCP_SEG 16
#define MEMP_NUM_ARP_QUEUE 4
#define PBUF_POOL_SIZE 8
#define PBUF_POOL_BUFSIZE 1524

/* --- protocols --- */
#define LWIP_ARP 1
#define LWIP_ETHERNET 1
#define LWIP_IPV4 1
#define LWIP_IPV6 0
#define LWIP_ICMP 1
#define LWIP_UDP 1
#define LWIP_TCP 1
#define LWIP_DHCP 1
#define LWIP_DNS 1
#define DNS_TABLE_SIZE 1
#define DNS_MAX_NAME_LENGTH 64
#define LWIP_AUTOIP 0
#define LWIP_RAW 0
#define LWIP_NETIF_STATUS_CALLBACK 1
#define LWIP_NETIF_LINK_CALLBACK 1
#define LWIP_NETIF_HOSTNAME 1

/* --- TCP tuning --- */
#define TCP_MSS 1460
#define TCP_WND (4 * TCP_MSS)
#define TCP_SND_BUF (4 * TCP_MSS)
#define TCP_SND_QUEUELEN 8

/* --- diagnostics (off by default; enable for bring-up) --- */
#define LWIP_STATS 0
#define LWIP_DEBUG 0
#define LWIP_PLATFORM_ASSERT(x)                                                                   \
    do {                                                                                          \
        (void)(x);                                                                                \
    } while (0)

#endif /* LWIPOPTS_H */
