#include "rly_console.h"

#include <string.h>

static void out(rly_console *c, const char *s) {
    if (c->ops.write != NULL) {
        c->ops.write(c->ops.ctx, s);
    }
}

static void out_kv(rly_console *c, const char *k, const char *v) {
    out(c, "  ");
    out(c, k);
    size_t n = strlen(k);
    while (n++ < 14) {
        out(c, " ");
    }
    out(c, v);
    out(c, "\r\n");
}

static void prompt(rly_console *c) { out(c, c->dirty ? "rly8*> " : "rly8> "); }

void rly_console_init(rly_console *c, const rly_console_ops *ops, const rly_config *live) {
    memset(c, 0, sizeof *c);
    if (ops != NULL) {
        c->ops = *ops;
    }
    if (live != NULL) {
        c->pending = *live;
    } else {
        rly_config_defaults(&c->pending);
    }
}

void rly_console_banner(rly_console *c) {
    out(c, "\r\nRLY-8 console. Type 'help'.\r\n");
    prompt(c);
}

static void cmd_help(rly_console *c) {
    out(c, "Commands:\r\n"
           "  status                       network, MQTT, relays\r\n"
           "  show                         configuration\r\n"
           "  set <key> <value>            keys: dhcp ip mask gw dns hostname name\r\n"
           "                               mqtt mqtt_host mqtt_port mqtt_user mqtt_pass\r\n"
           "                               mqtt_prefix admin_pass\r\n"
           "                               (empty mqtt_host = autodetect via mDNS)\r\n"
           "  save                         store config (applied after reboot)\r\n"
           "  reboot\r\n"
           "  factory                      factory defaults (DHCP, admin/admin) + reboot\r\n"
           "  relay <1-8|all> <on|off|toggle>\r\n");
}

static void put_mask(char *s, uint8_t mask) {
    for (int i = 0; i < 8; i++) {
        s[i] = ((mask >> i) & 1) ? '1' : '0';
    }
    s[8] = '\0';
}

static void cmd_status(rly_console *c) {
    rly_web_status st;
    memset(&st, 0, sizeof st);
    st.mqtt_state = "";
    st.mqtt_broker = "";
    st.mqtt_source = "";
    st.node_id = "";
    st.fw_version = "";
    if (c->ops.status != NULL) {
        c->ops.status(c->ops.ctx, &st);
    }
    char tmp[48];
    out_kv(c, "firmware", st.fw_version);
    out_kv(c, "node", st.node_id);
    out_kv(c, "link", st.link_up ? (st.speed_100 ? "up 100M" : "up 10M") : "down");
    rly_format_ipv4(st.ip, tmp, sizeof tmp);
    out_kv(c, "ip", tmp);
    rly_format_ipv4(st.mask, tmp, sizeof tmp);
    out_kv(c, "mask", tmp);
    rly_format_ipv4(st.gw, tmp, sizeof tmp);
    out_kv(c, "gw", tmp);
    out_kv(c, "addressing", c->pending.dhcp ? (st.dhcp_bound ? "dhcp"
                                                             : (st.static_fallback ? "dhcp (fallback)"
                                                                                   : "dhcp (waiting)"))
                                            : "static");
    out_kv(c, "mqtt", st.mqtt_state);
    out_kv(c, "broker", st.mqtt_broker[0] ? st.mqtt_broker : "-");
    out_kv(c, "broker source", st.mqtt_source);
    put_mask(tmp, st.relay_mask);
    out_kv(c, "relays 1..8", tmp);
    size_t u = rly_buf_append_uint(tmp, sizeof tmp, 0, st.uptime_s);
    (void)rly_buf_append(tmp, sizeof tmp, u, " s");
    out_kv(c, "uptime", tmp);
}

static void cmd_show(rly_console *c) {
    char v[RLY_CFG_HOST_MAX];
    for (const char *const *k = rly_config_keys; *k != NULL; k++) {
        rly_config_get(&c->pending, *k, v, sizeof v, 0);
        if (strcmp(*k, "mqtt_host") == 0 && v[0] == '\0') {
            out_kv(c, *k, "(autodetect)");
        } else if (strcmp(*k, "hostname") == 0 && v[0] == '\0') {
            out_kv(c, *k, "(auto)");
        } else {
            out_kv(c, *k, v);
        }
    }
    if (c->dirty) {
        out(c, "  (unsaved changes - type 'save')\r\n");
    }
}

static char *next_token(char **p) {
    while (**p == ' ') {
        (*p)++;
    }
    if (**p == '\0') {
        return NULL;
    }
    char *start = *p;
    while (**p != '\0' && **p != ' ') {
        (*p)++;
    }
    if (**p == ' ') {
        **p = '\0';
        (*p)++;
    }
    return start;
}

static void cmd_relay(rly_console *c, char *args) {
    char *n = next_token(&args);
    char *s = next_token(&args);
    if (n == NULL || s == NULL) {
        out(c, "usage: relay <1-8|all> <on|off|toggle>\r\n");
        return;
    }
    uint8_t bits;
    if (strcmp(n, "all") == 0) {
        bits = 0xFFu;
    } else if (n[0] >= '1' && n[0] <= '8' && n[1] == '\0') {
        bits = (uint8_t)(1u << (n[0] - '1'));
    } else {
        out(c, "error: relay must be 1-8 or all\r\n");
        return;
    }
    rly_web_status st;
    memset(&st, 0, sizeof st);
    if (c->ops.status != NULL) {
        c->ops.status(c->ops.ctx, &st);
    }
    uint8_t mask = st.relay_mask;
    if (strcmp(s, "on") == 0) {
        mask |= bits;
    } else if (strcmp(s, "off") == 0) {
        mask &= (uint8_t)~bits;
    } else if (strcmp(s, "toggle") == 0) {
        mask ^= bits;
    } else {
        out(c, "error: state must be on, off or toggle\r\n");
        return;
    }
    if (c->ops.set_relays != NULL) {
        c->ops.set_relays(c->ops.ctx, mask);
    }
    char m[9];
    put_mask(m, mask);
    out(c, "relays 1..8: ");
    out(c, m);
    out(c, "\r\n");
}

static void cmd_set(rly_console *c, char *args) {
    char *key = next_token(&args);
    if (key == NULL) {
        out(c, "usage: set <key> <value>   (value may be empty for mqtt_host, hostname, "
               "mqtt_user, mqtt_pass)\r\n");
        return;
    }
    rly_cfg_status s = rly_config_set(&c->pending, key, args);
    if (s == RLY_CFG_OK) {
        c->dirty = 1;
        char v[RLY_CFG_HOST_MAX];
        rly_config_get(&c->pending, key, v, sizeof v, 0);
        out(c, key);
        out(c, " = ");
        out(c, v);
        out(c, "\r\n");
    } else if (s == RLY_CFG_UNKNOWN_KEY) {
        out(c, "error: unknown key (see 'help')\r\n");
    } else if (s == RLY_CFG_TOO_LONG) {
        out(c, "error: value too long\r\n");
    } else {
        out(c, "error: invalid value\r\n");
    }
}

void rly_console_exec(rly_console *c, char *line) {
    char *p = line;
    char *cmd = next_token(&p);
    if (cmd == NULL) {
        return;
    }
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        cmd_help(c);
    } else if (strcmp(cmd, "status") == 0) {
        cmd_status(c);
    } else if (strcmp(cmd, "show") == 0) {
        cmd_show(c);
    } else if (strcmp(cmd, "set") == 0) {
        cmd_set(c, p);
    } else if (strcmp(cmd, "relay") == 0) {
        cmd_relay(c, p);
    } else if (strcmp(cmd, "save") == 0) {
        rly_config_seal(&c->pending);
        if (c->ops.save != NULL && c->ops.save(c->ops.ctx, &c->pending) == 0) {
            c->dirty = 0;
            out(c, "saved; type 'reboot' to apply\r\n");
        } else {
            out(c, "error: flash write failed\r\n");
        }
    } else if (strcmp(cmd, "reboot") == 0) {
        out(c, "rebooting...\r\n");
        if (c->ops.reboot != NULL) {
            c->ops.reboot(c->ops.ctx);
        }
    } else if (strcmp(cmd, "factory") == 0) {
        out(c, "factory defaults, rebooting...\r\n");
        if (c->ops.factory != NULL) {
            c->ops.factory(c->ops.ctx);
        }
    } else {
        out(c, "unknown command, type 'help'\r\n");
    }
}

void rly_console_input(rly_console *c, const uint8_t *data, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char ch = (char)data[i];
        char prev = c->last;
        c->last = ch;
        if (ch == '\n' && prev == '\r') {
            continue;
        }
        if (ch == '\r' || ch == '\n') {
            out(c, "\r\n");
            c->line[c->len] = '\0';
            rly_console_exec(c, c->line);
            c->len = 0;
            prompt(c);
        } else if (ch == 0x08 || ch == 0x7F) {
            if (c->len > 0) {
                c->len--;
                out(c, "\b \b");
            }
        } else if (ch == 0x03) { /* Ctrl-C: drop the line */
            c->len = 0;
            out(c, "^C\r\n");
            prompt(c);
        } else if ((unsigned char)ch >= 0x20 && (unsigned char)ch < 0x7F) {
            if (c->len + 1 < sizeof c->line) {
                c->line[c->len++] = ch;
                char echo[2] = {ch, '\0'};
                out(c, echo);
            }
        }
    }
}
