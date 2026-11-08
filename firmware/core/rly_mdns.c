#include "rly_mdns.h"

#include <string.h>

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int eq_ci(const char *a, const char *b) {
    while (*a && *b) {
        if (lower((unsigned char)*a) != lower((unsigned char)*b)) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == *b;
}

static int ends_with_ci(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && eq_ci(s + n - m, suffix);
}

/* Writes "a.b.c" as DNS labels. Returns bytes or 0. */
static size_t write_name(const char *name, uint8_t *out, size_t cap) {
    size_t used = 0;
    while (*name) {
        const char *dot = strchr(name, '.');
        size_t n = dot ? (size_t)(dot - name) : strlen(name);
        if (n == 0 || n > 63 || used + 1 + n + 1 > cap) {
            return 0;
        }
        out[used++] = (uint8_t)n;
        memcpy(out + used, name, n);
        used += n;
        name += n;
        if (*name == '.') {
            name++;
        }
    }
    if (used + 1 > cap) {
        return 0;
    }
    out[used++] = 0;
    return used;
}

/* Reads a possibly compressed name at *off; advances *off past it. */
static int read_name(const uint8_t *buf, size_t len, size_t *off, char *out, size_t cap) {
    size_t pos = *off;
    size_t o = 0;
    int jumped = 0;
    int hops = 0;
    if (cap == 0) {
        return 0;
    }
    out[0] = '\0';
    for (;;) {
        if (pos >= len) {
            return 0;
        }
        uint8_t l = buf[pos];
        if ((l & 0xC0u) == 0xC0u) {
            if (pos + 1 >= len || ++hops > 16) {
                return 0;
            }
            size_t ptr = ((size_t)(l & 0x3Fu) << 8) | buf[pos + 1];
            if (!jumped) {
                *off = pos + 2;
            }
            jumped = 1;
            pos = ptr;
            continue;
        }
        if (l == 0) {
            if (!jumped) {
                *off = pos + 1;
            }
            break;
        }
        if (l > 63 || pos + 1 + l > len) {
            return 0;
        }
        if (o != 0) {
            if (o + 1 >= cap) {
                return 0;
            }
            out[o++] = '.';
        }
        if (o + l >= cap) {
            return 0;
        }
        memcpy(out + o, buf + pos + 1, l);
        o += l;
        pos += 1u + l;
    }
    out[o] = '\0';
    return 1;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

size_t rly_mdns_build_query(const char *name, uint16_t qtype, uint8_t *out, size_t cap) {
    if (cap < 12) {
        return 0;
    }
    memset(out, 0, 12);
    wr16(out + 4, 1); /* QDCOUNT */
    size_t n = write_name(name, out + 12, cap - 12);
    if (n == 0 || 12 + n + 4 > cap) {
        return 0;
    }
    size_t used = 12 + n;
    wr16(out + used, qtype);
    wr16(out + used + 2, 1); /* class IN, QM (multicast reply) */
    return used + 4;
}

int rly_mdns_parse_response(const uint8_t *buf, size_t len, const char *srv_service,
                            rly_mdns_answers *out) {
    memset(out, 0, sizeof *out);
    if (len < 12 || (buf[2] & 0x80u) == 0) {
        return 0; /* not a response */
    }
    uint16_t qd = rd16(buf + 4);
    uint32_t rr = (uint32_t)rd16(buf + 6) + rd16(buf + 8) + rd16(buf + 10);
    size_t off = 12;
    char name[RLY_MDNS_NAME_MAX];
    for (uint16_t i = 0; i < qd; i++) {
        if (!read_name(buf, len, &off, name, sizeof name) || off + 4 > len) {
            return 0;
        }
        off += 4;
    }
    for (uint32_t i = 0; i < rr; i++) {
        if (!read_name(buf, len, &off, name, sizeof name) || off + 10 > len) {
            return 0;
        }
        uint16_t type = rd16(buf + off);
        uint16_t rdlen = rd16(buf + off + 8);
        size_t rdata = off + 10;
        if (rdata + rdlen > len) {
            return 0;
        }
        if (type == RLY_MDNS_TYPE_A && rdlen == 4 && out->a_count < RLY_MDNS_MAX_A) {
            const uint8_t *p = buf + rdata;
            memcpy(out->a_name[out->a_count], name, strlen(name) + 1);
            out->a_ip[out->a_count] = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                                      ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
            out->a_count++;
        } else if (type == RLY_MDNS_TYPE_SRV && rdlen >= 7 && srv_service != NULL &&
                   !out->have_srv && ends_with_ci(name, srv_service)) {
            size_t t = rdata + 6;
            if (read_name(buf, len, &t, out->srv_target, sizeof out->srv_target)) {
                out->srv_port = rd16(buf + rdata + 4);
                out->have_srv = 1;
            }
        }
        off = rdata + rdlen;
    }
    return 1;
}

uint32_t rly_mdns_lookup_a(const rly_mdns_answers *a, const char *name) {
    for (int i = 0; i < a->a_count; i++) {
        if (eq_ci(a->a_name[i], name)) {
            return a->a_ip[i];
        }
    }
    return 0;
}

int rly_mdns_query_wants(const uint8_t *buf, size_t len, const char *fqdn, int *unicast) {
    if (len < 12 || (buf[2] & 0x80u) != 0) {
        return 0; /* responses are not queries */
    }
    uint16_t qd = rd16(buf + 4);
    size_t off = 12;
    char name[RLY_MDNS_NAME_MAX];
    for (uint16_t i = 0; i < qd; i++) {
        if (!read_name(buf, len, &off, name, sizeof name) || off + 4 > len) {
            return 0;
        }
        uint16_t type = rd16(buf + off);
        uint16_t klass = rd16(buf + off + 2);
        off += 4;
        if ((type == RLY_MDNS_TYPE_A || type == RLY_MDNS_TYPE_ANY) && eq_ci(name, fqdn)) {
            if (unicast != NULL) {
                *unicast = (klass & 0x8000u) != 0;
            }
            return 1;
        }
    }
    return 0;
}

size_t rly_mdns_build_a_answer(const char *fqdn, uint32_t ip, uint8_t *out, size_t cap) {
    if (cap < 12) {
        return 0;
    }
    memset(out, 0, 12);
    out[2] = 0x84; /* QR + AA */
    wr16(out + 6, 1); /* ANCOUNT */
    size_t n = write_name(fqdn, out + 12, cap - 12);
    size_t used = 12 + n;
    if (n == 0 || used + 14 > cap) {
        return 0;
    }
    wr16(out + used, RLY_MDNS_TYPE_A);
    wr16(out + used + 2, 0x8001u); /* cache-flush + IN */
    out[used + 4] = 0;
    out[used + 5] = 0;
    wr16(out + used + 6, 120); /* TTL */
    wr16(out + used + 8, 4);
    out[used + 10] = (uint8_t)ip;
    out[used + 11] = (uint8_t)(ip >> 8);
    out[used + 12] = (uint8_t)(ip >> 16);
    out[used + 13] = (uint8_t)(ip >> 24);
    return used + 14;
}
