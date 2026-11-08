/* Relay driver for the RLY-8 board.
 *
 * The physical pin map is data, not code: it gets filled from the click-count sweep in
 * docs/relay-mapping.md. Until then every entry is marked unmapped (port 0xFF) and the
 * driver refuses to touch anything, so a half-identified board can never energise a coil
 * by accident.
 */
#ifndef RELAY_H
#define RELAY_H

#include <stddef.h>
#include <stdint.h>

#include "board.h"

#define RELAY_UNMAPPED_PORT 0xFFu

typedef struct {
    uint8_t port;       /* 0 = A, 1 = B, 2 = C, 3 = D, 4 = E, 0xFF = not identified */
    uint8_t pin;        /* 0..15 */
    uint8_t active_low; /* 1 when the coil is energised by driving the pin low */
} relay_pin;

/* Number of relays whose pins are actually known. */
size_t relay_mapped_count(void);

/* Copies the identified pin for one relay. Returns 1 when mapped, 0 when the relay is
 * out of range or still unidentified. Keeps the map inspectable without touching
 * hardware registers, so it can be asserted in host tests. */
int relay_pin_of(uint8_t relay, relay_pin *out);

/* Safe bring-up: every mapped pin is driven to its INACTIVE level before it becomes an
 * output, so no relay closes during start-up. Unmapped pins are left as inputs. */
void relay_init(void);

void relay_set(uint8_t relay, int on); /* relay: 1..8 */
int relay_get(uint8_t relay);

#endif /* RELAY_H */
