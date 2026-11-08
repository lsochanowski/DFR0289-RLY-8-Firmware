/* rly_stream implementation over a lwIP raw TCP PCB.
 *
 * The MQTT client expects a synchronous open(): it calls open() and immediately writes
 * CONNECT. lwIP's tcp_connect() is asynchronous, so open() pumps lwIP (via the hook the app
 * provides) until the connection is established or a short deadline passes.
 */
#ifndef TCP_STREAM_H
#define TCP_STREAM_H

#include <stdint.h>

#include "lwip/ip_addr.h"
#include "lwip/tcp.h"

#include "../core/rly_mqtt_client.h"

typedef struct {
    struct tcp_pcb *pcb;
    ip_addr_t broker;
    uint16_t port;
    uint8_t rx[RLY_MC_RX_MAX];
    size_t rx_len;
    volatile int connected;
    volatile int failed;
    void (*pump)(void); /* drains Ethernet + lwIP timers; supplied by the app */
    uint32_t connect_timeout_ms;
} lwip_tcp_stream;

/* Wires the struct into the rly_stream interface the MQTT client consumes. */
void lwip_tcp_stream_init(lwip_tcp_stream *s, const ip_addr_t *broker, uint16_t port,
                          void (*pump)(void));
rly_stream lwip_tcp_stream_interface(lwip_tcp_stream *s);

#endif /* TCP_STREAM_H */
