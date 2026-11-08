/* Line-oriented service console (USB CDC on the board, any byte stream in tests).
 *
 *   help                         list commands
 *   status                       network, MQTT, relays
 *   show                         configuration (passwords masked)
 *   set <key> <value>            change a key in the pending config (see rly_config.h)
 *   save                         persist the pending config (applied after reboot)
 *   reboot
 *   factory                      factory defaults + reboot
 *   relay <1-8|all> <on|off|toggle>
 */
#ifndef RLY_CONSOLE_H
#define RLY_CONSOLE_H

#include <stddef.h>
#include <stdint.h>

#include "rly_config.h"
#include "rly_web.h"

#define RLY_CONSOLE_LINE_MAX 128

typedef struct {
    void (*write)(void *ctx, const char *text);
    void (*status)(void *ctx, rly_web_status *out);
    void (*set_relays)(void *ctx, uint8_t mask);
    int (*save)(void *ctx, const rly_config *cfg); /* 0 = ok */
    void (*reboot)(void *ctx);
    void (*factory)(void *ctx);
    void *ctx;
} rly_console_ops;

typedef struct {
    rly_console_ops ops;
    rly_config pending;
    int dirty;
    char line[RLY_CONSOLE_LINE_MAX];
    size_t len;
    char last; /* previous byte, to swallow the LF of a CRLF pair */
} rly_console;

void rly_console_init(rly_console *c, const rly_console_ops *ops, const rly_config *live);
void rly_console_banner(rly_console *c);
/* Feed received bytes; echoes input and runs complete lines. */
void rly_console_input(rly_console *c, const uint8_t *data, size_t n);
/* Runs one command line directly (no echo). */
void rly_console_exec(rly_console *c, char *line);

#endif /* RLY_CONSOLE_H */
