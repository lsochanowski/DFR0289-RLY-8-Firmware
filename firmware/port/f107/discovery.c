#include "discovery.h"

#include <string.h>

#include "lwip/dns.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"

#include "../../core/rly_config.h"
#include "../../core/rly_mdns.h"

typedef enum {
    D_IDLE = 0,
    D_PTR,     /* waiting for _mqtt._tcp SRV */
    D_SRV_A,   /* SRV known, waiting for the target's A record */
    D_HA,      /* waiting for homeassistant.local */
    D_NAME,    /* waiting for a configured *.local name */
    D_DNS,     /* unicast DNS in flight */
    D_BACKOFF, /* nothing found; retry later */
    D_DONE
} d_state;

static struct netif *nif;
static struct udp_pcb *mdns_pcb;
static char fqdn[RLY_MDNS_NAME_MAX];

static d_state state;
static int autodetect;
static char host[RLY_CFG_HOST_MAX];
static char srv_target[RLY_MDNS_NAME_MAX];
static uint16_t cfg_port;
static uint16_t found_port;
static ip_addr_t found_ip;
static const char *source = "–";
static uint32_t deadline;
static uint32_t now_cache;
static int send_query_pending;
static uint16_t pending_qtype;
static const char *pending_name;

static const char HA_NAME[] = "homeassistant.local";
static const char MQTT_SERVICE[] = "_mqtt._tcp.local";

static void mdns_send(const uint8_t *data, size_t len) {
    if (mdns_pcb == NULL || ip4_addr_isany_val(*netif_ip4_addr(nif))) {
        return;
    }
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
    if (p == NULL) {
        return;
    }
    memcpy(p->payload, data, len);
    ip_addr_t group;
    IP_ADDR4(&group, 224, 0, 0, 251);
    udp_sendto(mdns_pcb, p, &group, RLY_MDNS_PORT);
    pbuf_free(p);
}

static void query(const char *name, uint16_t qtype) {
    uint8_t buf[128];
    size_t n = rly_mdns_build_query(name, qtype, buf, sizeof buf);
    if (n != 0) {
        mdns_send(buf, n);
    }
}

void discovery_announce(void) {
    uint8_t buf[128];
    size_t n = rly_mdns_build_a_answer(fqdn, ip4_addr_get_u32(netif_ip4_addr(nif)), buf, sizeof buf);
    if (n != 0) {
        mdns_send(buf, n);
    }
}

static void found(uint32_t ip4, uint16_t port, const char *src) {
    ip_addr_set_ip4_u32(&found_ip, ip4);
    found_port = port;
    source = src;
    state = D_DONE;
}

static void on_mdns(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr,
                    u16_t port) {
    (void)arg;
    static uint8_t buf[512];
    size_t len = p->tot_len < sizeof buf ? p->tot_len : sizeof buf;
    pbuf_copy_partial(p, buf, (u16_t)len, 0);
    pbuf_free(p);

    int unicast = 0;
    if (rly_mdns_query_wants(buf, len, fqdn, &unicast)) {
        uint8_t ans[128];
        size_t n = rly_mdns_build_a_answer(fqdn, ip4_addr_get_u32(netif_ip4_addr(nif)), ans,
                                           sizeof ans);
        if (n != 0 && (unicast || port != RLY_MDNS_PORT)) {
            struct pbuf *r = pbuf_alloc(PBUF_TRANSPORT, (u16_t)n, PBUF_RAM);
            if (r != NULL) {
                memcpy(r->payload, ans, n);
                udp_sendto(pcb, r, addr, port);
                pbuf_free(r);
            }
        } else if (n != 0) {
            mdns_send(ans, n);
        }
        return;
    }

    if (state != D_PTR && state != D_SRV_A && state != D_HA && state != D_NAME) {
        return;
    }
    static rly_mdns_answers a; /* ~400 B: keep it off the stack */
    if (!rly_mdns_parse_response(buf, len, MQTT_SERVICE, &a)) {
        return;
    }
    if (state == D_PTR && a.have_srv) {
        memcpy(srv_target, a.srv_target, sizeof srv_target);
        found_port = a.srv_port;
        uint32_t ip = rly_mdns_lookup_a(&a, srv_target);
        if (ip != 0u) {
            found(ip, found_port, "mDNS _mqtt._tcp");
            return;
        }
        state = D_SRV_A;
        deadline = now_cache + 1500u;
        query(srv_target, RLY_MDNS_TYPE_A);
        return;
    }
    if (state == D_SRV_A) {
        uint32_t ip = rly_mdns_lookup_a(&a, srv_target);
        if (ip != 0u) {
            found(ip, found_port, "mDNS _mqtt._tcp");
        }
        return;
    }
    if (state == D_HA || state == D_PTR) {
        /* homeassistant.local may also show up in answers to the PTR query */
        uint32_t ip = rly_mdns_lookup_a(&a, HA_NAME);
        if (ip != 0u) {
            found(ip, cfg_port, "homeassistant.local (mDNS)");
        }
        return;
    }
    if (state == D_NAME) {
        uint32_t ip = rly_mdns_lookup_a(&a, host);
        if (ip != 0u) {
            found(ip, cfg_port, ".local name (mDNS)");
        }
    }
}

static void on_dns(const char *name, const ip_addr_t *ip, void *arg) {
    (void)name;
    (void)arg;
    if (state != D_DNS) {
        return;
    }
    if (ip != NULL && IP_IS_V4(ip)) {
        found(ip4_addr_get_u32(ip_2_ip4(ip)), cfg_port, "DNS");
    } else {
        state = D_BACKOFF;
        deadline = now_cache + 10000u;
    }
}

void discovery_init(struct netif *netif, const char *hostname) {
    nif = netif;
    size_t n = strlen(hostname);
    if (n > sizeof fqdn - 7) {
        n = sizeof fqdn - 7;
    }
    memcpy(fqdn, hostname, n);
    memcpy(fqdn + n, ".local", 7);
    mdns_pcb = udp_new();
    if (mdns_pcb != NULL) {
        udp_bind(mdns_pcb, IP_ADDR_ANY, RLY_MDNS_PORT);
        udp_recv(mdns_pcb, on_mdns, NULL);
    }
}

static void begin(uint32_t now) {
    uint32_t ip;
    deadline = now + 1500u;
    if (autodetect) {
        state = D_PTR;
        source = "autodetect…";
        pending_name = MQTT_SERVICE;
        pending_qtype = RLY_MDNS_TYPE_PTR;
        send_query_pending = 1;
    } else if (rly_parse_ipv4(host, &ip)) {
        found(ip, cfg_port, "manual (IP)");
    } else {
        size_t n = strlen(host);
        if (n > 6 && strcmp(host + n - 6, ".local") == 0) {
            state = D_NAME;
            source = "mDNS…";
            pending_name = host;
            pending_qtype = RLY_MDNS_TYPE_A;
            send_query_pending = 1;
        } else {
            ip_addr_t addr;
            state = D_DNS;
            source = "DNS…";
            err_t e = dns_gethostbyname(host, &addr, on_dns, NULL);
            if (e == ERR_OK) {
                found(ip4_addr_get_u32(ip_2_ip4(&addr)), cfg_port, "DNS");
            } else if (e != ERR_INPROGRESS) {
                state = D_BACKOFF;
                deadline = now + 10000u;
            } else {
                deadline = now + 15000u;
            }
        }
    }
}

void discovery_broker_start(const char *h, uint16_t port) {
    size_t n = strlen(h);
    if (n >= sizeof host) {
        n = sizeof host - 1;
    }
    memcpy(host, h, n);
    host[n] = '\0';
    cfg_port = port;
    autodetect = host[0] == '\0';
    state = D_BACKOFF;
    deadline = now_cache; /* start on the next poll */
}

void discovery_poll(uint32_t now) {
    now_cache = now;
    if (send_query_pending && !ip4_addr_isany_val(*netif_ip4_addr(nif))) {
        send_query_pending = 0;
        query(pending_name, pending_qtype);
    }
    if (state == D_IDLE || state == D_DONE || (int32_t)(now - deadline) < 0) {
        return;
    }
    if (ip4_addr_isany_val(*netif_ip4_addr(nif))) {
        deadline = now + 500u; /* no address yet */
        return;
    }
    switch (state) {
    case D_PTR:
        /* no service record: try the Home Assistant host name */
        state = D_HA;
        deadline = now + 1500u;
        query(HA_NAME, RLY_MDNS_TYPE_A);
        break;
    case D_SRV_A:
    case D_HA:
    case D_NAME:
    case D_DNS:
        state = D_BACKOFF;
        source = autodetect ? "autodetect: no broker found" : "name not resolved";
        deadline = now + 10000u;
        break;
    case D_BACKOFF:
        begin(now);
        break;
    default:
        break;
    }
}

int discovery_broker(ip_addr_t *ip, uint16_t *port) {
    if (state != D_DONE) {
        return 0;
    }
    *ip = found_ip;
    *port = found_port;
    return 1;
}

const char *discovery_source(void) { return source; }
