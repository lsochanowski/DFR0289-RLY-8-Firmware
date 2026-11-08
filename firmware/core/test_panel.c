/* Host tests: HTTP parsing, web panel, console, mDNS. */
#include <stdio.h>
#include <string.h>

#include "rly_console.h"
#include "rly_http.h"
#include "rly_mdns.h"
#include "rly_web.h"

static int checks, failures;

static void expect_int(const char *name, long got, long want) {
    checks++;
    if (got != want) {
        failures++;
        printf("FAIL %s: got %ld want %ld\n", name, got, want);
    } else {
        printf("ok   %s\n", name);
    }
}

static void expect_str(const char *name, const char *got, const char *want) {
    checks++;
    if (strcmp(got, want) != 0) {
        failures++;
        printf("FAIL %s\n  want: %s\n  got : %s\n", name, want, got);
    } else {
        printf("ok   %s\n", name);
    }
}

static void expect_has(const char *name, const char *hay, const char *needle) {
    checks++;
    if (strstr(hay, needle) == NULL) {
        failures++;
        printf("FAIL %s: missing \"%s\"\n", name, needle);
    } else {
        printf("ok   %s\n", name);
    }
}

/* "admin:admin" and "admin:s3cret" */
#define AUTH_ADMIN "Authorization: Basic YWRtaW46YWRtaW4=\r\n"
#define AUTH_SECRET "Authorization: Basic YWRtaW46czNjcmV0\r\n"

static char page[8192];

static size_t run(const char *raw, rly_config *cfg, rly_web_status *st, rly_web_result *res) {
    rly_http_request req;
    rly_http_parse_status ps = rly_http_parse(raw, strlen(raw), &req);
    if (ps != RLY_HTTP_READY) {
        page[0] = '\0';
        return 0;
    }
    size_t n = rly_web_handle(&req, cfg, st, "tok123", page, sizeof page, res);
    page[n < sizeof page ? n : sizeof page - 1] = '\0';
    return n;
}

static char raw_buf[2048];

/* Builds "POST <path>" with a correct Content-Length. */
static const char *post(const char *path, const char *auth, const char *body) {
    snprintf(raw_buf, sizeof raw_buf, "POST %s HTTP/1.1\r\nContent-Length: %zu\r\n%s\r\n%s", path,
             strlen(body), auth, body);
    return raw_buf;
}

static void test_http(void) {
    rly_http_request r;
    const char *partial = "GET / HTTP/1.1\r\nHost: x\r\n";
    expect_int("incomplete headers", rly_http_parse(partial, strlen(partial), &r),
               RLY_HTTP_INCOMPLETE);
    const char *post = "POST /save?x=1 HTTP/1.1\r\ncontent-length: 7\r\n" AUTH_ADMIN "\r\na=1&b=";
    expect_int("incomplete body", rly_http_parse(post, strlen(post), &r), RLY_HTTP_INCOMPLETE);
    const char *full = "POST /save?x=1 HTTP/1.1\r\ncontent-length: 7\r\n" AUTH_ADMIN "\r\na=1&b=2";
    expect_int("complete", rly_http_parse(full, strlen(full), &r), RLY_HTTP_READY);
    expect_str("path", r.path, "/save");
    expect_str("query", r.query, "x=1");
    expect_int("method", r.method, RLY_HTTP_POST);
    expect_int("body len", (long)r.body_len, 7);
    expect_int("bad request line", rly_http_parse("BLAH\r\n\r\n", 8, &r), RLY_HTTP_BAD);

    char v[32];
    const char *form = "mqtt_host=ha.local&mqtt_pass=p%40ss+w%2Fd&empty=&flag";
    expect_int("form get", rly_http_form_get(form, strlen(form), "mqtt_pass", v, sizeof v), 1);
    expect_str("url decode", v, "p@ss w/d");
    expect_int("form empty", rly_http_form_get(form, strlen(form), "empty", v, sizeof v), 1);
    expect_str("empty value", v, "");
    expect_int("form flag", rly_http_form_get(form, strlen(form), "flag", v, sizeof v), 1);
    expect_int("form missing", rly_http_form_get(form, strlen(form), "nope", v, sizeof v), 0);
    expect_int("bad percent kept", rly_http_form_get("a=%zz%4", 7, "a", v, sizeof v), 1);
    expect_str("bad percent literal", v, "%zz%4");

    const char *ok = "Basic YWRtaW46YWRtaW4=";
    expect_int("basic ok", rly_http_check_basic(ok, strlen(ok), "admin", "admin"), 1);
    expect_int("basic wrong pass", rly_http_check_basic(ok, strlen(ok), "admin", "admin2"), 0);
    expect_int("basic prefix pass", rly_http_check_basic(ok, strlen(ok), "admin", "admi"), 0);
    expect_int("basic wrong user", rly_http_check_basic(ok, strlen(ok), "root", "admin"), 0);
    expect_int("basic garbage", rly_http_check_basic("Basic !!!", 9, "admin", "admin"), 0);
    expect_int("no auth", rly_http_check_basic(NULL, 0, "admin", "admin"), 0);

    char esc[64];
    size_t n = rly_html_escape(esc, sizeof esc, 0, "<a href=\"x\">&'");
    expect_str("html escape", esc, "&lt;a href=&quot;x&quot;&gt;&amp;&#39;");
    expect_int("escape len", (long)n, (long)strlen(esc));
}

static void test_web(void) {
    rly_config cfg;
    rly_config_defaults(&cfg);
    rly_web_status st;
    memset(&st, 0, sizeof st);
    st.fw_version = "0.2.0";
    st.node_id = "rly8-abcdef";
    st.mqtt_state = "połączony";
    st.mqtt_broker = "10.0.0.2:1883";
    st.mqtt_source = "mDNS";
    st.relay_mask = 0x05;
    st.link_up = 1;
    st.ip = 10u | 0u << 8 | 0u << 16 | 7u << 24;
    rly_web_result res;

    run("GET / HTTP/1.1\r\n\r\n", &cfg, &st, &res);
    expect_has("401 without auth", page, "401 Unauthorized");
    expect_has("realm", page, "WWW-Authenticate: Basic");

    size_t n = run("GET / HTTP/1.1\r\n" AUTH_ADMIN "\r\n", &cfg, &st, &res);
    expect_has("status 200", page, "200 OK");
    expect_has("relay 1 on", page, "value=1 class=on>1 · ON");
    expect_has("relay 2 off", page, "value=2>2 · OFF");
    expect_has("ip shown", page, "10.0.0.7");
    expect_has("token embedded", page, "name=t value=tok123");
    expect_int("status page fits", n < 6144, 1);
    printf("     status page %zu bytes\n", n);

    n = run("GET /settings HTTP/1.1\r\n" AUTH_ADMIN "\r\n", &cfg, &st, &res);
    expect_has("settings form", page, "action=/save");
    expect_has("password not echoed", page, "name=admin_pass value=\"\"");
    expect_int("settings page fits", n < 6144, 1);
    printf("     settings page %zu bytes\n", n);

    run("GET /api/status HTTP/1.1\r\n" AUTH_ADMIN "\r\n", &cfg, &st, &res);
    expect_has("json relays", page, "\"relays\":[1,0,1,0,0,0,0,0]");

    run(post("/relay", AUTH_ADMIN, "t=bad&n=2&s=on&x="),
        &cfg, &st, &res);
    expect_has("token checked", page, "403");
    expect_int("no action on bad token", res.action, RLY_WEB_ACT_NONE);

    run(post("/relay", AUTH_ADMIN, "t=tok123&n=2&s=on&"),
        &cfg, &st, &res);
    expect_int("relay action", res.action, RLY_WEB_ACT_RELAYS);
    expect_int("relay mask", res.relay_mask, 0x07);
    expect_has("redirect", page, "303 See Other");

    run(post("/relay", AUTH_ADMIN, "t=tok123&n=all&s=off&"),
        &cfg, &st, &res);
    expect_int("all off", res.relay_mask, 0);

    const char *bad_save = post("/save", AUTH_ADMIN, "t=tok123&dhcp=off&ip=1.2.3&mqtt_port=1");
    run(bad_save, &cfg, &st, &res);
    expect_has("save error 400", page, "400 Bad Request");
    expect_has("error names field", page, "Błędne pole: ip");
    expect_int("config untouched on error", cfg.dhcp, 1);

    const char *mismatch = post("/save", AUTH_ADMIN, "t=tok123&admin_pass=s3cret&admin_pass2=other");
    run(mismatch, &cfg, &st, &res);
    expect_has("password mismatch", page, "Hasła panelu się różnią");

    const char *good = post("/save", AUTH_ADMIN, "t=tok123&dhcp=off&ip=10.0.0.9&mask=255.255.255.0&mqtt_host=&"
                       "mqtt_user=ha&mqtt_pass=&admin_pass=s3cret&admin_pass2=s3cret&mqtt=on");
    run(good, &cfg, &st, &res);
    expect_int("save action", res.action, RLY_WEB_ACT_SAVE_REBOOT);
    expect_int("dhcp off saved", cfg.dhcp, 0);
    expect_str("user saved", cfg.mqtt_user, "ha");
    expect_str("admin changed", cfg.admin_pass, "s3cret");
    expect_has("redirect to static ip", page, "http://10.0.0.9/");

    run("GET / HTTP/1.1\r\n" AUTH_ADMIN "\r\n", &cfg, &st, &res);
    expect_has("old password rejected", page, "401");
    run("GET / HTTP/1.1\r\n" AUTH_SECRET "\r\n", &cfg, &st, &res);
    expect_has("new password works", page, "200 OK");

    strcpy(cfg.mqtt_pass, "keep");
    run(post("/save", AUTH_SECRET, "t=tok123&mqtt_pass=&"),
        &cfg, &st, &res);
    expect_str("empty secret keeps old", cfg.mqtt_pass, "keep");
    run(post("/save", AUTH_SECRET, "t=tok123&mqtt_pass_clear=1"),
        &cfg, &st, &res);
    expect_str("clear secret", cfg.mqtt_pass, "");

    {
        rly_http_request rq;
        const char *raw = post("/relay", AUTH_SECRET, "t=&n=1&s=on");
        rly_http_parse(raw, strlen(raw), &rq);
        rly_web_handle(&rq, &cfg, &st, "", page, sizeof page, &res);
        expect_int("empty token never matches", res.action, RLY_WEB_ACT_NONE);
    }
    run(post("/factory", AUTH_SECRET, "t=tok123"), &cfg, &st,
        &res);
    expect_int("factory action", res.action, RLY_WEB_ACT_FACTORY);
}

/* --- console --- */
static char con_out[4096];
static size_t con_len;
static uint8_t con_mask;
static int con_saved, con_reboots;

static void con_write(void *ctx, const char *s) {
    (void)ctx;
    size_t n = strlen(s);
    if (con_len + n < sizeof con_out) {
        memcpy(con_out + con_len, s, n + 1);
        con_len += n;
    }
}
static void con_status(void *ctx, rly_web_status *st) {
    (void)ctx;
    st->relay_mask = con_mask;
    st->mqtt_state = "off";
    st->mqtt_broker = "";
    st->mqtt_source = "";
    st->node_id = "rly8-x";
    st->fw_version = "t";
}
static void con_relays(void *ctx, uint8_t m) {
    (void)ctx;
    con_mask = m;
}
static int con_save(void *ctx, const rly_config *c) {
    (void)ctx;
    con_saved = rly_config_valid(c) ? 1 : -1;
    return 0;
}
static void con_reboot(void *ctx) {
    (void)ctx;
    con_reboots++;
}

static void feed(rly_console *c, const char *s) {
    con_len = 0;
    con_out[0] = '\0';
    rly_console_input(c, (const uint8_t *)s, strlen(s));
}

static void test_console(void) {
    rly_config live;
    rly_config_defaults(&live);
    rly_console_ops ops = {con_write, con_status, con_relays, con_save, con_reboot, con_reboot,
                           NULL};
    rly_console c;
    rly_console_init(&c, &ops, &live);

    feed(&c, "help\r\n");
    expect_has("help lists set", con_out, "set <key> <value>");
    feed(&c, "relay 3 on\r");
    expect_int("relay 3 on", con_mask, 0x04);
    feed(&c, "relay all toggle\n");
    expect_int("toggle all", con_mask, 0xFB);
    feed(&c, "relay 9 on\r\n");
    expect_has("relay 9 rejected", con_out, "error");
    feed(&c, "set ip 10.1.1.1\r\n");
    expect_has("set echo", con_out, "ip = 10.1.1.1");
    expect_has("dirty prompt", con_out, "rly8*> ");
    feed(&c, "set ip nope\r\n");
    expect_has("bad ip", con_out, "invalid value");
    feed(&c, "set mqtt_host\r\n");
    expect_has("empty host = autodetect", con_out, "mqtt_host = ");
    feed(&c, "set mqtt_pass top secret\r\n");
    expect_has("secret masked on echo", con_out, "mqtt_pass = ***");
    expect_str("secret with space stored", c.pending.mqtt_pass, "top secret");
    feed(&c, "show\r\n");
    expect_has("show autodetect", con_out, "(autodetect)");
    feed(&c, "save\r\n");
    expect_int("save sealed", con_saved, 1);
    expect_has("clean prompt", con_out, "rly8> ");
    feed(&c, "sat\x7f\x7ftatus\r\n");
    expect_has("backspace edits", con_out, "relays 1..8");
    feed(&c, "reboot\r\n");
    expect_int("reboot called", con_reboots, 1);
    feed(&c, "bogus\r\n");
    expect_has("unknown", con_out, "unknown command");
}

static void test_mdns(void) {
    uint8_t q[64];
    size_t n = rly_mdns_build_query("_mqtt._tcp.local", RLY_MDNS_TYPE_PTR, q, sizeof q);
    static const uint8_t want[] = {0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 5, '_', 'm', 'q', 't',
                                   't', 4, '_', 't', 'c', 'p', 5, 'l', 'o', 'c', 'a', 'l', 0, 0,
                                   12, 0, 1};
    expect_int("query len", (long)n, (long)sizeof want);
    expect_int("query bytes", memcmp(q, want, sizeof want), 0);

    /* Response: PTR _mqtt._tcp.local -> Mosquitto._mqtt._tcp.local, SRV (port 1883, target
     * ha.local), A ha.local = 192.168.1.20. Uses compression pointers. */
    static const uint8_t resp[] = {
        0, 0, 0x84, 0, 0, 0, 0, 1, 0, 0, 0, 2,
        /* 12: answer PTR, owner _mqtt._tcp.local */
        5, '_', 'm', 'q', 't', 't', 4, '_', 't', 'c', 'p', 5, 'l', 'o', 'c', 'a', 'l', 0,
        0, 12, 0, 1, 0, 0, 0, 120, 0, 12,
        /* 40: rdata: Mosquitto + ptr->12 */
        9, 'M', 'o', 's', 'q', 'u', 'i', 't', 't', 'o', 0xC0, 12,
        /* 52: additional SRV, owner ptr->40 */
        0xC0, 40, 0, 33, 0x80, 1, 0, 0, 0, 120, 0, 11,
        0, 0, 0, 0, 0x07, 0x5B, 2, 'h', 'a', 0xC0, 23,
        /* A ha.local, owner ptr -> (offset of "ha" label = 70) */
        0xC0, 70, 0, 1, 0x80, 1, 0, 0, 0, 120, 0, 4, 192, 168, 1, 20};
    rly_mdns_answers a;
    expect_int("parse ok", rly_mdns_parse_response(resp, sizeof resp, "_mqtt._tcp.local", &a), 1);
    expect_int("srv found", a.have_srv, 1);
    expect_int("srv port", a.srv_port, 1883);
    expect_str("srv target", a.srv_target, "ha.local");
    expect_int("a record", (long)rly_mdns_lookup_a(&a, "HA.local"),
               (long)(192u | 168u << 8 | 1u << 16 | 20u << 24));
    expect_int("truncated rejected",
               rly_mdns_parse_response(resp, sizeof resp - 3, "_mqtt._tcp.local", &a), 0);
    static const uint8_t loop[] = {0, 0, 0x84, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0xC0, 12};
    expect_int("pointer loop rejected", rly_mdns_parse_response(loop, sizeof loop, NULL, &a), 0);

    uint8_t aq[64];
    n = rly_mdns_build_query("rly8-abc.local", RLY_MDNS_TYPE_A, aq, sizeof aq);
    int uni = 1;
    expect_int("query for us", rly_mdns_query_wants(aq, n, "RLY8-ABC.local", &uni), 1);
    expect_int("multicast reply", uni, 0);
    expect_int("not for us", rly_mdns_query_wants(aq, n, "other.local", &uni), 0);
    expect_int("response is not a query", rly_mdns_query_wants(resp, sizeof resp, "ha.local", &uni),
               0);

    uint8_t ans[64];
    n = rly_mdns_build_a_answer("rly8-abc.local", 10u | 1u << 24, ans, sizeof ans);
    rly_mdns_answers back;
    expect_int("answer parses", rly_mdns_parse_response(ans, n, NULL, &back), 1);
    expect_int("answer ip", (long)rly_mdns_lookup_a(&back, "rly8-abc.local"), (long)(10u | 1u << 24));
}

int main(void) {
    test_http();
    test_web();
    test_console();
    test_mdns();
    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
