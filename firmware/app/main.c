/* RLY-8 application firmware.
 *
 * Boot: config (flash) → clock → relays (all off) → SysTick → LED/button → USB console →
 * PHY → MAC/DMA → lwIP (DHCP or static) → web panel, mDNS, MQTT (HA discovery).
 * Everything important lands in boot_status so it can be read over SWD even when the
 * network is down (field order documented in AGENTS.md §2).
 */
#include <stdint.h>
#include <string.h>

#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/init.h"
#include "lwip/ip4_addr.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"

#include "../core/rly_console.h"
#include "../core/rly_mqtt_client.h"
#include "../core/rly_version.h"
#include "../core/rly_web.h"
#include "../port/f107/board.h"
#include "../port/f107/clock.h"
#include "../port/f107/discovery.h"
#include "../port/f107/eth.h"
#include "../port/f107/eth_dma.h"
#include "../port/f107/flash.h"
#include "../port/f107/ethernetif.h"
#include "../port/f107/flash_cfg.h"
#include "../port/f107/httpd.h"
#include "../port/f107/relay.h"
#include "../port/f107/systick.h"
#include "../port/f107/tcp_stream.h"
#include "../port/f107/ui.h"
#include "../port/f107/usb_cdc.h"

#define FW_VERSION RLY_FW_VERSION
#define DHCP_FALLBACK_MS 15000u

typedef struct {
    uint32_t magic;
    int32_t clock_result;
    uint32_t relays_mapped;
    uint32_t sequence_done;
    int32_t eth_init_result;
    uint32_t phy_address;
    uint32_t link_up;
    uint32_t bmsr;
    uint32_t physts;
    uint32_t poll_count;
    int32_t eth_dma_result;
    uint32_t netif_up;
    uint32_t dhcp_started;
    uint32_t ip_addr;
    uint32_t rx_frames;
    uint32_t tx_frames;
    uint32_t up_ms;
    uint32_t mqtt_state;
    uint32_t mqtt_commands;
    uint32_t relay_mask;
    uint32_t static_ip;
    uint32_t bmcr;
    uint32_t rbr;
    uint32_t phyctrl;
    uint32_t cfg_loaded;     /* 1 = config from flash, 0 = factory defaults */
    uint32_t http_requests;
    uint32_t usb_configured;
    uint32_t broker_ip;
} boot_report;

volatile boot_report boot_status;

static rly_config cfg;
static struct netif netif;
static uint8_t mac_addr[6];
static char node_id[24];
static char token[12];

static lwip_tcp_stream stream;
static rly_mqtt_client mqtt;
static rly_ha_config ha;
static int mqtt_started;
static uint8_t relay_applied_mask;
static char broker_text[24];

static rly_console console;
static int usb_was_open;
static uint32_t reboot_at;
static int factory_pending;

/* ---------------------------------------------------------------- helpers */

static void system_reset(void) {
    *(volatile uint32_t *)0xE000ED0Cu = 0x05FA0004u; /* SCB_AIRCR: SYSRESETREQ */
    for (;;) {
    }
}

static void schedule_reboot(uint32_t delay_ms) {
    reboot_at = systick_ms() + delay_ms;
    if (reboot_at == 0u) {
        reboot_at = 1u;
    }
}

static void mac_from_uid(void) {
    const volatile uint32_t *uid = (const volatile uint32_t *)0x1FFFF7E8u;
    mac_addr[0] = 0x02u; /* locally administered, unicast */
    mac_addr[1] = (uint8_t)(uid[0] >> 24);
    mac_addr[2] = (uint8_t)(uid[0] >> 16);
    mac_addr[3] = (uint8_t)(uid[0] >> 8);
    mac_addr[4] = (uint8_t)(uid[1] >> 24);
    mac_addr[5] = (uint8_t)(uid[1] >> 16);
}

static void make_ids(void) {
    static const char hex[] = "0123456789abcdef";
    if (cfg.hostname[0] != '\0') {
        size_t n = strlen(cfg.hostname);
        memcpy(node_id, cfg.hostname, n + 1);
        return;
    }
    memcpy(node_id, "rly8-", 5);
    size_t p = 5;
    for (uint32_t i = 3; i < 6; i++) {
        node_id[p++] = hex[(mac_addr[i] >> 4) & 0xFu];
        node_id[p++] = hex[mac_addr[i] & 0xFu];
    }
    node_id[p] = '\0';
}

/* Per-boot form token: mixes the UID with timing jitter of the boot. */
static void make_token(void) {
    static const char hex[] = "0123456789abcdef";
    uint32_t seed[4] = {*(const volatile uint32_t *)0x1FFFF7E8u,
                        *(volatile uint32_t *)0xE000E018u, /* SysTick VAL */
                        eth_stats.rx_frames, systick_ms()};
    uint32_t h = rly_crc32(seed, sizeof seed) ^ rly_crc32(&cfg, sizeof cfg);
    for (int i = 0; i < 8; i++) {
        token[i] = hex[(h >> (28 - 4 * i)) & 0xFu];
    }
    token[8] = '\0';
}

static void pump_netif(void) {
    ethernetif_input(&netif);
    sys_check_timeouts();
}

/* Single place that changes relays: GPIO, MQTT state and the LED. */
static void apply_relays(uint8_t mask) {
    uint8_t changed = (uint8_t)(mask ^ relay_applied_mask);
    for (uint8_t relay = 1; relay <= BOARD_RELAY_COUNT; relay++) {
        uint8_t bit = (uint8_t)(1u << (relay - 1u));
        if (changed & bit) {
            relay_set(relay, (mask & bit) != 0u);
            if (mqtt_started && mqtt.state == RLY_MC_READY) {
                (void)rly_session_set_relay(&mqtt.session, relay, (mask & bit) != 0u);
            }
        }
    }
    relay_applied_mask = mask;
    if (mqtt_started) {
        mqtt.session.relay_mask = mask;
    }
    if (changed) {
        ui_activity();
    }
}

/* MQTT commands only change session.relay_mask; push the difference to the GPIO driver. */
static void sync_relays(void) {
    if (mqtt.session.relay_mask != relay_applied_mask) {
        uint8_t want = mqtt.session.relay_mask;
        uint8_t changed = (uint8_t)(want ^ relay_applied_mask);
        for (uint8_t relay = 1; relay <= BOARD_RELAY_COUNT; relay++) {
            uint8_t bit = (uint8_t)(1u << (relay - 1u));
            if (changed & bit) {
                relay_set(relay, (want & bit) != 0u);
            }
        }
        relay_applied_mask = want;
        ui_activity();
    }
}

static const char *mqtt_state_text(void) {
    if (!cfg.mqtt_enabled) {
        return "disabled";
    }
    if (!mqtt_started) {
        return "waiting for broker";
    }
    switch (mqtt.state) {
    case RLY_MC_READY:
        return "connected";
    case RLY_MC_CONNECTING:
        return "connecting";
    case RLY_MC_ERROR:
        return "connection failed (retrying)";
    default:
        return "idle";
    }
}

static void fill_status(rly_web_status *st) {
    memset(st, 0, sizeof *st);
    st->fw_version = FW_VERSION;
    st->node_id = node_id;
    memcpy(st->mac, mac_addr, 6);
    st->ip = ip4_addr_get_u32(netif_ip4_addr(&netif));
    st->mask = ip4_addr_get_u32(netif_ip4_netmask(&netif));
    st->gw = ip4_addr_get_u32(netif_ip4_gw(&netif));
    st->link_up = eth_last_status.link_up;
    st->speed_100 = eth_last_status.speed_100;
    st->full_duplex = eth_last_status.full_duplex;
    st->dhcp_bound = cfg.dhcp && dhcp_supplied_address(&netif);
    st->static_fallback = boot_status.static_ip != 0u;
    st->uptime_s = systick_ms() / 1000u;
    st->mqtt_state = mqtt_state_text();
    st->mqtt_broker = broker_text;
    st->mqtt_source = cfg.mqtt_enabled ? discovery_source() : "–";
    st->relay_mask = relay_applied_mask;
    st->usb_connected = usb_cdc_configured();
}

/* ---------------------------------------------------------------- web panel */

static size_t web_handler(const rly_http_request *req, char *out, size_t cap) {
    rly_web_status st;
    rly_web_result res;
    fill_status(&st);
    size_t n = rly_web_handle(req, &cfg, &st, token, out, cap, &res);
    ui_activity();
    switch (res.action) {
    case RLY_WEB_ACT_RELAYS:
        apply_relays(res.relay_mask);
        break;
    case RLY_WEB_ACT_SAVE_REBOOT:
        (void)flash_cfg_save(&cfg);
        schedule_reboot(1500u);
        break;
    case RLY_WEB_ACT_REBOOT:
        schedule_reboot(1500u);
        break;
    case RLY_WEB_ACT_FACTORY:
        factory_pending = 1;
        schedule_reboot(1500u);
        break;
    default:
        break;
    }
    return n;
}

/* ---------------------------------------------------------------- firmware upload */

static rly_ota_writer ota;
static int ota_led;

static int ota_begin(const rly_http_request *req, char *out, size_t cap, size_t *len) {
    if (!rly_web_upload_allowed(req, &cfg, token, out, cap, len)) {
        return 0;
    }
    (void)flash_erase_page(RLY_OTA_CTRL_ADDR); /* no stale "pending" record while we write */
    (void)rly_ota_begin(&ota, &flash_ops, RLY_STAGING_BASE, RLY_SLOT_SIZE,
                        (uint32_t)req->content_length);
    return 1;
}

static void ota_data(const uint8_t *data, size_t n) {
    (void)rly_ota_write(&ota, data, (uint32_t)n);
    if ((++ota_led & 7) == 0) {
        ui_activity(); /* LED flickers while the image streams in */
    }
}

static size_t ota_end(char *out, size_t cap) {
    rly_ota_ctrl ctrl;
    rly_img_status st = rly_ota_finish(&ota, RLY_APP_BASE, &ctrl);
    char version[16];
    rly_image_version(ota.head, sizeof ota.head, version, sizeof version);
    if (st == RLY_IMG_OK) {
        if (flash_erase_page(RLY_OTA_CTRL_ADDR) != 0 ||
            flash_program(RLY_OTA_CTRL_ADDR, (const uint8_t *)&ctrl, sizeof ctrl) != 0) {
            st = RLY_IMG_IO;
        } else {
            schedule_reboot(1500u); /* the bootloader installs it */
        }
    }
    return rly_web_upload_result(st, version, out, cap);
}

static void ota_abort(void) {
    /* Nothing to undo: without the control record the bootloader ignores the staging slot. */
}

static const httpd_upload ota_upload = {"/update", ota_begin, ota_data, ota_end, ota_abort};

/* ---------------------------------------------------------------- USB console */

static void con_write(void *ctx, const char *text) {
    (void)ctx;
    if (!usb_cdc_configured()) {
        return;
    }
    int len = (int)strlen(text);
    /* The TX ring is 1 KB; give the IN endpoint a moment when a long listing fills it. */
    for (uint32_t tries = 0; len > 0 && tries < 20000u; tries++) {
        int n = usb_cdc_write((const uint8_t *)text, len);
        text += n;
        len -= n;
        if (len > 0) {
            usb_cdc_poll();
        }
    }
}

static void con_status(void *ctx, rly_web_status *st) {
    (void)ctx;
    fill_status(st);
}

static void con_relays(void *ctx, uint8_t mask) {
    (void)ctx;
    apply_relays(mask);
}

static int con_save(void *ctx, const rly_config *c) {
    (void)ctx;
    static rly_config tmp;
    tmp = *c;
    if (flash_cfg_save(&tmp) != 0) {
        return -1;
    }
    cfg = tmp;
    return 0;
}

static void con_reboot(void *ctx) {
    (void)ctx;
    schedule_reboot(300u);
}

static void con_factory(void *ctx) {
    (void)ctx;
    factory_pending = 1;
    schedule_reboot(300u);
}

static void console_poll(void) {
    usb_cdc_poll();
    int open = usb_cdc_configured() && usb_cdc_dtr();
    if (open && !usb_was_open) {
        rly_console_banner(&console);
    }
    usb_was_open = open;
    uint8_t buf[64];
    int n = usb_cdc_read(buf, sizeof buf);
    if (n > 0) {
        rly_console_input(&console, buf, (size_t)n);
    }
}

/* ---------------------------------------------------------------- network */

static void start_mqtt(const ip_addr_t *broker, uint16_t port, uint32_t now) {
    char ip[16];
    rly_format_ipv4(ip4_addr_get_u32(ip_2_ip4(broker)), ip, sizeof ip);
    size_t u = rly_buf_append(broker_text, sizeof broker_text, 0, ip);
    u = rly_buf_append(broker_text, sizeof broker_text, u, ":");
    (void)rly_buf_append_uint(broker_text, sizeof broker_text, u, port);
    boot_status.broker_ip = ip4_addr_get_u32(ip_2_ip4(broker));

    rly_ha_defaults(&ha, node_id);
    memcpy(ha.device_name, cfg.device_name, sizeof ha.device_name);
    ha.device_name[sizeof ha.device_name - 1] = '\0';
    memcpy(ha.prefix, cfg.mqtt_prefix, sizeof ha.prefix);
    ha.prefix[sizeof ha.prefix - 1] = '\0';
    char self[16];
    rly_format_ipv4(ip4_addr_get_u32(netif_ip4_addr(&netif)), self, sizeof self);
    u = rly_buf_append(ha.config_url, sizeof ha.config_url, 0, "http://");
    u = rly_buf_append(ha.config_url, sizeof ha.config_url, u, self);
    (void)rly_buf_append(ha.config_url, sizeof ha.config_url, u, "/");

    lwip_tcp_stream_init(&stream, broker, port, pump_netif);
    rly_stream io = lwip_tcp_stream_interface(&stream);
    rly_mc_init(&mqtt, &io, &ha, node_id, 30);
    mqtt.session.username = cfg.mqtt_user;
    mqtt.session.password = cfg.mqtt_pass;
    mqtt.session.relay_mask = relay_applied_mask; /* keep relays as they are */
    mqtt_started = 1;
    (void)rly_mc_start(&mqtt, now);
}

static void net_init(void) {
    mac_from_uid();
    make_ids();
    boot_status.eth_dma_result = eth_dma_init(mac_addr);

    ip4_addr_t ip, mask, gw;
    if (cfg.dhcp) {
        IP4_ADDR(&ip, 0, 0, 0, 0);
        IP4_ADDR(&mask, 0, 0, 0, 0);
        IP4_ADDR(&gw, 0, 0, 0, 0);
    } else {
        ip4_addr_set_u32(&ip, cfg.ip);
        ip4_addr_set_u32(&mask, cfg.mask);
        ip4_addr_set_u32(&gw, cfg.gw);
    }
    lwip_init();
    if (netif_add(&netif, &ip, &mask, &gw, mac_addr, ethernetif_init, ethernet_input) == NULL) {
        return;
    }
    netif_set_hostname(&netif, node_id);
    netif_set_default(&netif);
    netif_set_up(&netif);
    boot_status.netif_up = 1;

    ip_addr_t dns;
    ip_addr_set_ip4_u32(&dns, cfg.dns);
    dns_setserver(cfg.dhcp ? 1 : 0, &dns); /* with DHCP: our DNS is the second choice */

    httpd_set_upload(&ota_upload);
    (void)httpd_start(web_handler);
    discovery_init(&netif, node_id);
    if (cfg.mqtt_enabled) {
        discovery_broker_start(cfg.mqtt_host, cfg.mqtt_port);
    }
}

/* ---------------------------------------------------------------- main */

int main(void) {
    boot_status.magic = 0x524C5931u;
    boot_status.clock_result = -99;
    boot_status.eth_init_result = -99;
    boot_status.eth_dma_result = -99;
    boot_status.phy_address = 0xFFFFFFFFu;

    boot_status.cfg_loaded = (uint32_t)flash_cfg_load(&cfg);
    boot_status.clock_result = clock_init_72mhz();
    relay_init(); /* all relays off: no click sequence at boot */
    boot_status.relays_mapped = (uint32_t)relay_mapped_count();
    boot_status.sequence_done = 0x444F4E45u; /* DONE */

    systick_init();
    ui_init();
    usb_cdc_init();

    static const rly_console_ops ops = {con_write, con_status, con_relays, con_save,
                                        con_reboot, con_factory, NULL};
    rly_console_init(&console, &ops, &cfg);

    boot_status.eth_init_result = eth_init();
    boot_status.phy_address = eth_last_status.phy_address;
    if (boot_status.eth_init_result == 0) {
        net_init();
    }

    uint32_t dhcp_start_ms = 0;
    uint32_t last_ip = 0;
    int token_made = 0;

    for (;;) {
        uint32_t now = systick_ms();
        boot_status.up_ms = now;

        if (ui_poll(now)) {
            factory_pending = 1;
            schedule_reboot(200u);
        }
        console_poll();

        if (reboot_at != 0u && (int32_t)(now - reboot_at) >= 0) {
            if (factory_pending) {
                flash_cfg_erase();
            }
            system_reset();
        }

        if (boot_status.netif_up != 0u) {
            pump_netif();

            static uint32_t link_poll_at;
            static int link;
            if ((int32_t)(now - link_poll_at) >= 0) { /* MDIO is slow; 50 Hz is plenty */
                link_poll_at = now + 20u;
                link = eth_poll_link();
            }
            boot_status.link_up = (uint32_t)link;
            boot_status.bmsr = eth_last_status.bmsr;
            boot_status.physts = eth_last_status.physts;
            boot_status.bmcr = eth_last_status.bmcr;
            boot_status.rbr = eth_last_status.rbr;
            boot_status.phyctrl = eth_last_status.phyctrl;

            /* lwIP drops every outgoing frame while the netif link flag is down. */
            if (link != 0 && !netif_is_link_up(&netif)) {
                netif_set_link_up(&netif);
            } else if (link == 0 && netif_is_link_up(&netif)) {
                netif_set_link_down(&netif);
            }

            if (cfg.dhcp && link != 0 && boot_status.dhcp_started == 0u) {
                if (dhcp_start(&netif) == ERR_OK) {
                    boot_status.dhcp_started = 1;
                    dhcp_start_ms = now;
                }
            }

            uint32_t ip = ip4_addr_get_u32(netif_ip4_addr(&netif));

            /* No DHCP server (board straight on a PC): use the configured static address
             * so the panel stays reachable. DHCP keeps trying in the background. */
            if (cfg.dhcp && boot_status.dhcp_started != 0u && ip == 0u &&
                boot_status.static_ip == 0u && (now - dhcp_start_ms) > DHCP_FALLBACK_MS) {
                ip4_addr_t s_ip, s_mask, s_gw;
                ip4_addr_set_u32(&s_ip, cfg.ip);
                ip4_addr_set_u32(&s_mask, cfg.mask);
                ip4_addr_set_u32(&s_gw, cfg.gw);
                netif_set_addr(&netif, &s_ip, &s_mask, &s_gw);
                boot_status.static_ip = 1;
                ip = cfg.ip;
            }
            boot_status.ip_addr = ip;

            if (ip != 0u && ip != last_ip && link != 0) {
                discovery_announce();
                if (!token_made) {
                    make_token();
                    token_made = 1;
                }
            }
            last_ip = link ? ip : 0u;
            if (!token_made && now > 3000u) {
                make_token();
                token_made = 1;
            }

            discovery_poll(now);

            ip_addr_t broker;
            uint16_t port;
            if (cfg.mqtt_enabled && !mqtt_started && ip != 0u && link != 0 &&
                discovery_broker(&broker, &port)) {
                start_mqtt(&broker, port, now);
            }
            if (mqtt_started) {
                (void)rly_mc_poll(&mqtt, now);
                sync_relays();
            }

            boot_status.rx_frames = eth_stats.rx_frames;
            boot_status.tx_frames = eth_stats.tx_frames;
            boot_status.mqtt_state = (uint32_t)mqtt.state;
            boot_status.mqtt_commands = mqtt.session.commands_applied;
            boot_status.http_requests = httpd_requests();

            if (!link) {
                ui_set_state(UI_NO_LINK);
            } else if (ip == 0u) {
                ui_set_state(UI_NO_IP);
            } else if (cfg.mqtt_enabled && !(mqtt_started && mqtt.state == RLY_MC_READY)) {
                ui_set_state(UI_MQTT_WAIT);
            } else {
                ui_set_state(UI_READY);
            }
        } else {
            (void)eth_poll_link();
            boot_status.link_up = (uint32_t)eth_last_status.link_up;
            ui_set_state(UI_NO_LINK);
        }

        boot_status.relay_mask = relay_applied_mask;
        boot_status.usb_configured = (uint32_t)usb_cdc_configured();
        boot_status.poll_count++;
    }
}
