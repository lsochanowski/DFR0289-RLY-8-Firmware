#include <stdio.h>
#include <string.h>

#include "rly_config.h"

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

int main(void) {
    rly_config c;
    char out[80];
    rly_config_defaults(&c);
    expect_int("defaults valid", rly_config_valid(&c), 1);
    expect_int("dhcp default on", c.dhcp, 1);
    expect_str("admin default", c.admin_pass, "admin");
    expect_str("prefix default", c.mqtt_prefix, "homeassistant");
    expect_int("mqtt port default", c.mqtt_port, 1883);
    expect_str("broker empty = autodetect", c.mqtt_host, "");

    /* CRC: known vector "123456789" -> 0xCBF43926 */
    expect_int("crc32 vector", (long)rly_crc32("123456789", 9), (long)0xCBF43926u);

    expect_int("tamper detected", (c.mqtt_port = 1884, rly_config_valid(&c)), 0);
    rly_config_seal(&c);
    expect_int("reseal valid", rly_config_valid(&c), 1);

    uint32_t ip;
    expect_int("parse ip ok", rly_parse_ipv4("10.1.2.254", &ip), 1);
    expect_int("ip byte order", (long)ip, (long)(10u | 1u << 8 | 2u << 16 | 254u << 24));
    expect_int("reject 256", rly_parse_ipv4("10.1.2.256", &ip), 0);
    expect_int("reject 3 parts", rly_parse_ipv4("10.1.2", &ip), 0);
    expect_int("reject trailing", rly_parse_ipv4("10.1.2.3x", &ip), 0);
    expect_int("reject empty octet", rly_parse_ipv4("10..2.3", &ip), 0);
    rly_format_ipv4(ip, out, sizeof out);
    expect_str("format ip", out, "10.1.2.254");

    expect_int("set ip", rly_config_set(&c, "ip", "192.168.1.77"), RLY_CFG_OK);
    rly_config_get(&c, "ip", out, sizeof out, 0);
    expect_str("get ip", out, "192.168.1.77");
    expect_int("set bad ip", rly_config_set(&c, "gw", "1.2.3"), RLY_CFG_BAD_VALUE);
    expect_int("set dhcp off", rly_config_set(&c, "dhcp", "off"), RLY_CFG_OK);
    expect_int("dhcp now off", c.dhcp, 0);
    expect_int("set dhcp junk", rly_config_set(&c, "dhcp", "maybe"), RLY_CFG_BAD_VALUE);
    expect_int("set port", rly_config_set(&c, "mqtt_port", " 8883 "), RLY_CFG_OK);
    expect_int("port value", c.mqtt_port, 8883);
    expect_int("port 0 rejected", rly_config_set(&c, "mqtt_port", "0"), RLY_CFG_BAD_VALUE);
    expect_int("port 70000 rejected", rly_config_set(&c, "mqtt_port", "70000"), RLY_CFG_BAD_VALUE);
    expect_int("broker name", rly_config_set(&c, "mqtt_host", "homeassistant.local"), RLY_CFG_OK);
    expect_int("broker bad char", rly_config_set(&c, "mqtt_host", "a b"), RLY_CFG_BAD_VALUE);
    expect_int("broker empty ok", rly_config_set(&c, "mqtt_host", ""), RLY_CFG_OK);
    expect_int("hostname no dots", rly_config_set(&c, "hostname", "a.b"), RLY_CFG_BAD_VALUE);
    expect_int("prefix no wildcard", rly_config_set(&c, "mqtt_prefix", "ha/#"), RLY_CFG_BAD_VALUE);
    expect_int("admin empty rejected", rly_config_set(&c, "admin_pass", ""), RLY_CFG_BAD_VALUE);
    expect_int("admin set", rly_config_set(&c, "admin_pass", "s3cret pass"), RLY_CFG_OK);
    expect_int("pass with quote rejected", rly_config_set(&c, "mqtt_pass", "a\"b"), RLY_CFG_BAD_VALUE);
    expect_int("pass too long", rly_config_set(&c, "mqtt_pass",
                                               "0123456789012345678901234567890123456789012345678"),
               RLY_CFG_TOO_LONG);
    rly_config_get(&c, "admin_pass", out, sizeof out, 0);
    expect_str("secret masked", out, "***");
    rly_config_get(&c, "admin_pass", out, sizeof out, 1);
    expect_str("secret revealed", out, "s3cret pass");
    rly_config_get(&c, "mqtt_pass", out, sizeof out, 0);
    expect_str("empty secret stays empty", out, "");
    expect_int("unknown key", rly_config_set(&c, "nope", "1"), RLY_CFG_UNKNOWN_KEY);
    expect_int("name with space", rly_config_set(&c, "name", "Garage relays"), RLY_CFG_OK);
    expect_int("name utf-8", rly_config_set(&c, "name", "RLY-8 Gara\xc5\xbc"), RLY_CFG_OK);
    expect_int("prefix ascii only", rly_config_set(&c, "mqtt_prefix", "ha\xc5\xbc"), RLY_CFG_BAD_VALUE);
    expect_int("name control char", rly_config_set(&c, "name", "a\x01b"), RLY_CFG_BAD_VALUE);

    int keys = 0;
    for (const char *const *k = rly_config_keys; *k; k++) {
        keys++;
        if (rly_config_get(&c, *k, out, sizeof out, 0) != RLY_CFG_OK) {
            failures++;
            printf("FAIL get %s\n", *k);
        }
    }
    expect_int("every key readable", keys, 14);

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
