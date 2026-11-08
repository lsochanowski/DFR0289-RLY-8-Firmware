/* Host-side tests for the JSON dialect and the Home Assistant discovery payloads. */
#include <stdio.h>
#include <string.h>

#include "../core/rly_ha.h"
#include "../core/rly_json.h"
#include "../core/rly_version.h"

static int failures;
static int checks;

static void expect_str(const char *what, const char *got, const char *want) {
    checks++;
    if (got == NULL || strcmp(got, want) != 0) {
        failures++;
        printf("FAIL %s\n  want: %s\n  got : %s\n", what, want, got ? got : "(null)");
        return;
    }
    printf("ok   %s\n", what);
}

static void expect_int(const char *what, long got, long want) {
    checks++;
    if (got != want) {
        failures++;
        printf("FAIL %s: want %ld, got %ld\n", what, want, got);
        return;
    }
    printf("ok   %s\n", what);
}

int main(void) {
    rly_json_obj obj;
    char buf[RLY_HA_DISCOVERY_MAX];

    /* --- JSON parsing --- */
    const char *get_status = "{\"get\":\"relayStatus\"}";
    expect_int("parse get relayStatus", rly_json_parse(get_status, strlen(get_status), &obj),
               RLY_OK);
    expect_int("pair count", (long)obj.count, 1);
    expect_str("get value", rly_json_get(&obj, "get"), "relayStatus");

    const char *relay_set = "{\"relay1\":\"on\",\"relay8\":\"off\"}";
    expect_int("parse relay set", rly_json_parse(relay_set, strlen(relay_set), &obj), RLY_OK);
    uint8_t mentioned = 0;
    uint8_t on = 0;
    expect_int("relay bitmaps", rly_json_relays(&obj, &mentioned, &on), RLY_OK);
    expect_int("relays mentioned", mentioned, 0x81);
    expect_int("relays on", on, 0x01);

    const char *spaced = " { \"relay2\" : \"on\" } ";
    expect_int("parse with spaces", rly_json_parse(spaced, strlen(spaced), &obj), RLY_OK);
    expect_int("spaced bitmaps", rly_json_relays(&obj, &mentioned, &on), RLY_OK);
    expect_int("spaced mentioned", mentioned, 0x02);
    expect_int("spaced on", on, 0x02);

    const char *bad_value = "{\"relay1\":\"maybe\"}";
    expect_int("parse bad relay value", rly_json_parse(bad_value, strlen(bad_value), &obj), RLY_OK);
    expect_int("bad relay value rejected", rly_json_relays(&obj, &mentioned, &on), RLY_ERR_ARG);

    const char *top_level = "{\"relayStatus\":\"on\"}";
    expect_int("non-relay key ignored", rly_json_parse(top_level, strlen(top_level), &obj), RLY_OK);
    expect_int("non-relay bitmaps", rly_json_relays(&obj, &mentioned, &on), RLY_OK);
    expect_int("non-relay mentioned", mentioned, 0);

    const char *malformed[] = {"no json", "{\"a\" \"b\"}", "{\"a\":1}", "{\"a\":\"b\"", "{}x"};
    for (size_t i = 0; i < sizeof malformed / sizeof malformed[0]; i++) {
        checks++;
        rly_status st = rly_json_parse(malformed[i], strlen(malformed[i]), &obj);
        if (st == RLY_OK) {
            /* "{}" collapses; only the empty object may parse successfully. */
            if (strcmp(malformed[i], "{}") != 0) {
                failures++;
                printf("FAIL malformed accepted: %s\n", malformed[i]);
                continue;
            }
        }
        printf("ok   malformed rejected: %s\n", malformed[i]);
    }

    /* --- JSON serialisation --- */
    size_t n = rly_json_relay_status(buf, sizeof buf, 0x05);
    checks++;
    if (n == 0) {
        failures++;
        printf("FAIL relay status serialisation returned 0\n");
    } else {
        printf("ok   relay status serialisation\n");
    }
    expect_str("relay status payload", buf,
               "{\"relay1\":\"on\",\"relay2\":\"off\",\"relay3\":\"on\",\"relay4\":\"off\","
               "\"relay5\":\"off\",\"relay6\":\"off\",\"relay7\":\"off\",\"relay8\":\"off\"}");

    rly_json_resp(buf, sizeof buf, "ok");
    expect_str("resp ok", buf, "{\"resp\":\"ok\"}");
    rly_json_resp(buf, sizeof buf, "unsupported");
    expect_str("resp unsupported", buf, "{\"resp\":\"unsupported\"}");

    /* Too small a buffer must fail rather than truncate. */
    expect_int("small buffer refused", (long)rly_json_resp(buf, 8, "ok"), 0);

    /* --- Home Assistant discovery --- */
    rly_ha_config cfg;
    rly_ha_defaults(&cfg, "rly8test");
    expect_str("node id", cfg.node_id, "rly8test");
    expect_str("fw version", cfg.sw_version, RLY_FW_VERSION);

    char discovery[RLY_TOPIC_MAX];
    char command[RLY_TOPIC_MAX];
    char state[RLY_TOPIC_MAX];
    char availability[RLY_TOPIC_MAX];
    rly_ha_topics(&cfg, 3, discovery, command, state, availability, sizeof state);
    expect_str("discovery topic", discovery, "homeassistant/switch/rly8test/relay3/config");
    expect_str("command topic", command, "rly8test/relay3/set");
    expect_str("state topic", state, "rly8test/relay3/state");
    expect_str("availability topic", availability, "rly8test/status");

    n = rly_ha_discovery(&cfg, 1, buf, sizeof buf);
    checks++;
    if (n == 0) {
        failures++;
        printf("FAIL discovery payload returned 0\n");
    } else {
        printf("ok   discovery payload built (%zu bytes)\n", n);
    }
    expect_str("discovery payload", buf,
               "{\"name\":\"RLY-8 Relay 1\",\"uniq_id\":\"rly8test_relay1\","
               "\"stat_t\":\"rly8test/relay1/state\",\"cmd_t\":\"rly8test/relay1/set\","
               "\"avty_t\":\"rly8test/status\",\"pl_avail\":\"online\","
               "\"pl_not_avail\":\"offline\",\"dev\":{\"ids\":[\"rly8test\"],"
               "\"name\":\"RLY-8\",\"mf\":\"DFRobot\","
               "\"mdl\":\"DFR0289 (custom fw)\",\"sw\":\"" RLY_FW_VERSION "\"}}");
    expect_int("discovery rejects relay 0", (long)rly_ha_discovery(&cfg, 0, buf, sizeof buf), 0);
    expect_int("discovery rejects relay 9", (long)rly_ha_discovery(&cfg, 9, buf, sizeof buf), 0);
    expect_int("discovery refuses tiny buffer", (long)rly_ha_discovery(&cfg, 1, buf, 16), 0);

    /* custom discovery prefix + configuration URL on the device */
    memcpy(cfg.prefix, "ha", 3);
    memcpy(cfg.config_url, "http://10.0.0.5/", 17);
    rly_ha_topics(&cfg, 2, discovery, NULL, NULL, NULL, sizeof discovery);
    expect_str("custom prefix topic", discovery, "ha/switch/rly8test/relay2/config");
    n = rly_ha_discovery(&cfg, 2, buf, sizeof buf);
    expect_int("payload has cu", strstr(buf, "\"sw\":\"" RLY_FW_VERSION "\",\"cu\":\"http://10.0.0.5/\"}}") != NULL, 1);

    expect_str("state ON", rly_ha_state_payload(1), "ON");
    expect_str("state OFF", rly_ha_state_payload(0), "OFF");

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
