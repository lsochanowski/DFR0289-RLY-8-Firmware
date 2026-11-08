#include "rly_http.h"

#include <string.h>

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int prefix_ci(const char *s, size_t n, const char *word) {
    size_t w = strlen(word);
    if (n < w) {
        return 0;
    }
    for (size_t i = 0; i < w; i++) {
        if (lower((unsigned char)s[i]) != lower((unsigned char)word[i])) {
            return 0;
        }
    }
    return 1;
}

static const char *find_seq(const char *buf, size_t len, const char *seq) {
    size_t n = strlen(seq);
    for (size_t i = 0; i + n <= len; i++) {
        if (memcmp(buf + i, seq, n) == 0) {
            return buf + i;
        }
    }
    return NULL;
}

static rly_http_parse_status parse(const char *buf, size_t len, rly_http_request *req,
                                   int need_body) {
    memset(req, 0, sizeof *req);
    const char *end = find_seq(buf, len, "\r\n\r\n");
    if (end == NULL) {
        return RLY_HTTP_INCOMPLETE;
    }
    size_t head_len = (size_t)(end - buf) + 4u;

    /* request line: METHOD SP target SP version */
    const char *line_end = find_seq(buf, head_len, "\r\n");
    const char *sp1 = memchr(buf, ' ', (size_t)(line_end - buf));
    if (sp1 == NULL) {
        return RLY_HTTP_BAD;
    }
    if (sp1 - buf == 3 && memcmp(buf, "GET", 3) == 0) {
        req->method = RLY_HTTP_GET;
    } else if (sp1 - buf == 4 && memcmp(buf, "POST", 4) == 0) {
        req->method = RLY_HTTP_POST;
    } else {
        req->method = RLY_HTTP_OTHER;
    }
    const char *target = sp1 + 1;
    const char *sp2 = memchr(target, ' ', (size_t)(line_end - target));
    if (sp2 == NULL || target[0] != '/') {
        return RLY_HTTP_BAD;
    }
    const char *q = memchr(target, '?', (size_t)(sp2 - target));
    const char *path_end = q ? q : sp2;
    size_t plen = (size_t)(path_end - target);
    if (plen >= sizeof req->path) {
        return RLY_HTTP_BAD;
    }
    memcpy(req->path, target, plen);
    req->path[plen] = '\0';
    if (q != NULL) {
        size_t qlen = (size_t)(sp2 - q - 1);
        if (qlen >= sizeof req->query) {
            qlen = sizeof req->query - 1;
        }
        memcpy(req->query, q + 1, qlen);
        req->query[qlen] = '\0';
    }

    /* headers */
    size_t content_length = 0;
    const char *p = line_end + 2;
    while (p < end) {
        const char *le = find_seq(p, (size_t)(end + 2 - p), "\r\n");
        if (le == NULL) {
            break;
        }
        size_t n = (size_t)(le - p);
        if (prefix_ci(p, n, "content-length:")) {
            const char *v = p + 15;
            while (v < le && *v == ' ') {
                v++;
            }
            size_t cl = 0;
            while (v < le && *v >= '0' && *v <= '9') {
                cl = cl * 10u + (size_t)(*v - '0');
                if (cl > 0x100000u) { /* 1 MB: far above anything this device accepts */
                    return RLY_HTTP_TOO_BIG;
                }
                v++;
            }
            content_length = cl;
        } else if (prefix_ci(p, n, "authorization:")) {
            const char *v = p + 14;
            while (v < le && *v == ' ') {
                v++;
            }
            req->auth = v;
            req->auth_len = (size_t)(le - v);
        }
        p = le + 2;
    }

    req->content_length = content_length;
    req->head_len = head_len;
    req->body = buf + head_len;
    if (!need_body) {
        size_t have = len - head_len;
        req->body_len = have < content_length ? have : content_length;
        return RLY_HTTP_READY;
    }
    if (content_length > 65535u) {
        return RLY_HTTP_TOO_BIG; /* buffered requests are small forms */
    }
    if (len - head_len < content_length) {
        return RLY_HTTP_INCOMPLETE;
    }
    req->body_len = content_length;
    return RLY_HTTP_READY;
}

rly_http_parse_status rly_http_parse(const char *buf, size_t len, rly_http_request *req) {
    return parse(buf, len, req, 1);
}

rly_http_parse_status rly_http_parse_head(const char *buf, size_t len, rly_http_request *req) {
    return parse(buf, len, req, 0);
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    c = (char)lower((unsigned char)c);
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

/* Decodes data[from..to) into out; truncates to cap-1. */
static void url_decode(const char *data, size_t from, size_t to, char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = from; i < to; i++) {
        char c = data[i];
        if (c == '+') {
            c = ' ';
        } else if (c == '%' && i + 2 < to) {
            int h = hexval(data[i + 1]);
            int l = hexval(data[i + 2]);
            if (h >= 0 && l >= 0) {
                c = (char)(h * 16 + l);
                i += 2;
            }
        }
        if (o + 1 < cap) {
            out[o++] = c;
        }
    }
    if (cap > 0) {
        out[o] = '\0';
    }
}

int rly_http_form_next(const char *data, size_t len, size_t *pos, char *key, size_t key_cap,
                       char *value, size_t value_cap) {
    while (*pos < len) {
        size_t start = *pos;
        size_t amp = start;
        while (amp < len && data[amp] != '&') {
            amp++;
        }
        *pos = amp < len ? amp + 1 : len;
        if (amp == start) {
            continue; /* empty pair "&&" */
        }
        size_t eq = start;
        while (eq < amp && data[eq] != '=') {
            eq++;
        }
        url_decode(data, start, eq, key, key_cap);
        if (eq < amp) {
            url_decode(data, eq + 1, amp, value, value_cap);
        } else if (value_cap > 0) {
            value[0] = '\0';
        }
        return 1;
    }
    return 0;
}

int rly_http_form_get(const char *data, size_t len, const char *name, char *value, size_t cap) {
    size_t pos = 0;
    char key[32];
    while (rly_http_form_next(data, len, &pos, key, sizeof key, value, cap)) {
        if (strcmp(key, name) == 0) {
            return 1;
        }
    }
    if (cap > 0) {
        value[0] = '\0';
    }
    return 0;
}

static int b64val(char c) {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

int rly_http_check_basic(const char *auth, size_t auth_len, const char *user, const char *pass) {
    if (auth == NULL || auth_len < 6 || !prefix_ci(auth, auth_len, "basic ")) {
        return 0;
    }
    const char *p = auth + 6;
    size_t n = auth_len - 6;
    while (n > 0 && *p == ' ') {
        p++;
        n--;
    }
    char decoded[112];
    size_t o = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == '=') {
            break;
        }
        int v = b64val(p[i]);
        if (v < 0) {
            return 0;
        }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o + 1 >= sizeof decoded) {
                return 0;
            }
            decoded[o++] = (char)((acc >> bits) & 0xFFu);
        }
    }
    decoded[o] = '\0';

    size_t ul = strlen(user);
    if (o < ul + 1 || memcmp(decoded, user, ul) != 0 || decoded[ul] != ':') {
        return 0;
    }
    /* constant-time compare of the password part */
    const char *got = decoded + ul + 1;
    size_t gl = o - ul - 1;
    size_t pl = strlen(pass);
    unsigned diff = (unsigned)(gl ^ pl);
    for (size_t i = 0; i < gl && i < pl; i++) {
        diff |= (unsigned)(got[i] ^ pass[i]);
    }
    return diff == 0u;
}

size_t rly_buf_append(char *out, size_t cap, size_t used, const char *text) {
    if (used >= cap) {
        return cap;
    }
    size_t n = strlen(text);
    if (used + n >= cap) {
        out[used] = '\0';
        return cap;
    }
    memcpy(out + used, text, n + 1);
    return used + n;
}

size_t rly_buf_append_uint(char *out, size_t cap, size_t used, uint32_t v) {
    char d[11];
    int n = 0;
    do {
        d[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v != 0u);
    char s[11];
    for (int i = 0; i < n; i++) {
        s[i] = d[n - 1 - i];
    }
    s[n] = '\0';
    return rly_buf_append(out, cap, used, s);
}

size_t rly_html_escape(char *out, size_t cap, size_t used, const char *text) {
    char one[2] = {0, 0};
    for (; *text && used < cap; text++) {
        switch (*text) {
        case '&':
            used = rly_buf_append(out, cap, used, "&amp;");
            break;
        case '<':
            used = rly_buf_append(out, cap, used, "&lt;");
            break;
        case '>':
            used = rly_buf_append(out, cap, used, "&gt;");
            break;
        case '"':
            used = rly_buf_append(out, cap, used, "&quot;");
            break;
        case '\'':
            used = rly_buf_append(out, cap, used, "&#39;");
            break;
        default:
            one[0] = *text;
            used = rly_buf_append(out, cap, used, one);
            break;
        }
    }
    return used;
}
