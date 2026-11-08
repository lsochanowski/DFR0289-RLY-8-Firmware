#include "relay.h"

#define REG(addr) (*(volatile uint32_t *)(addr))
#define RCC_APB2ENR REG(0x40021018u)

#define MODE_OUT_PP_2MHZ 0x2u
#define MODE_FLOAT_INPUT 0x4u

/* Relay map, fully identified by bisection on the real board (docs/relay-mapping.md).
 *
 *   relay 1 = PC8    relay 2 = PC9    relay 3 = PC10   relay 4 = PC11
 *   relay 5 = PC12   relay 6 = PC7    relay 7 = PC6    relay 8 = PB15
 *
 * Two quirks worth remembering: relays 6 and 7 are swapped relative to the natural
 * PC6/PC7 order, and relay 8 sits alone on port B (PB15) instead of continuing the
 * port C block. Coils energise with the pin HIGH on every channel, so active_low = 0.
 *
 * Port index: 0 = A, 1 = B, 2 = C, 3 = D, 4 = E. */
static const relay_pin relay_map[BOARD_RELAY_COUNT] = {
    {2u, 8u, 0u},  {2u, 9u, 0u},  {2u, 10u, 0u}, {2u, 11u, 0u},
    {2u, 12u, 0u}, {2u, 7u, 0u},  {2u, 6u, 0u},  {1u, 15u, 0u},
};

static uint8_t relay_state; /* bit0 = relay 1 */

static const uint32_t port_bases[5] = {GPIOA_BASE, GPIOB_BASE, GPIOC_BASE, GPIOD_BASE, GPIOE_BASE};

static volatile uint32_t *port_base(uint8_t port) {
    if (port >= 5u) {
        return 0;
    }
    /* Via uintptr_t: a direct uint32_t -> pointer cast truncates on a 64-bit host, which
     * would make this file untestable outside the target. */
    return (volatile uint32_t *)(uintptr_t)port_bases[port];
}

static void pin_mode(volatile uint32_t *base, uint8_t pin, uint32_t mode) {
    volatile uint32_t *cr = base + (pin < 8u ? 0x00u : 0x01u);
    uint32_t shift = (uint32_t)(pin & 7u) * 4u;
    uint32_t value = *cr;
    value &= ~(0xFu << shift);
    value |= (mode & 0xFu) << shift;
    *cr = value;
}

static void pin_level(volatile uint32_t *base, uint8_t pin, int high) {
    volatile uint32_t *odr = base + 0x03u;
    if (high) {
        *odr |= (1u << pin);
    } else {
        *odr &= ~(1u << pin);
    }
}

size_t relay_mapped_count(void) {
    size_t mapped = 0;
    for (size_t i = 0; i < BOARD_RELAY_COUNT; i++) {
        if (relay_map[i].port != RELAY_UNMAPPED_PORT) {
            mapped++;
        }
    }
    return mapped;
}

int relay_pin_of(uint8_t relay, relay_pin *out) {
    if (relay < 1u || relay > BOARD_RELAY_COUNT || out == 0) {
        return 0;
    }
    const relay_pin *mapping = &relay_map[relay - 1u];
    if (mapping->port == RELAY_UNMAPPED_PORT) {
        return 0;
    }
    *out = *mapping;
    return 1;
}

void relay_init(void) {
    RCC_APB2ENR |= (0x1Fu << 2); /* IOPA..IOPE */
    relay_state = 0;

    for (size_t i = 0; i < BOARD_RELAY_COUNT; i++) {
        const relay_pin *mapping = &relay_map[i];
        volatile uint32_t *base = port_base(mapping->port);
        if (base == 0) {
            continue; /* unmapped: leave the pin alone */
        }
        /* Set the ODR bit to the INACTIVE level first, then switch to output: with an
         * active-low driver an output latch holding 0 would close the relay instantly. */
        pin_level(base, mapping->pin, mapping->active_low ? 1 : 0);
        pin_mode(base, mapping->pin, MODE_OUT_PP_2MHZ);
    }
}

void relay_set(uint8_t relay, int on) {
    if (relay < 1u || relay > BOARD_RELAY_COUNT) {
        return;
    }
    const relay_pin *mapping = &relay_map[relay - 1u];
    volatile uint32_t *base = port_base(mapping->port);
    if (base == 0) {
        return;
    }
    uint8_t bit = (uint8_t)(1u << (relay - 1u));
    int level;
    if (on) {
        relay_state |= bit;
        level = mapping->active_low ? 0 : 1;
    } else {
        relay_state &= (uint8_t)~bit;
        level = mapping->active_low ? 1 : 0;
    }
    pin_level(base, mapping->pin, level);
    pin_mode(base, mapping->pin, MODE_OUT_PP_2MHZ);
}

int relay_get(uint8_t relay) {
    if (relay < 1u || relay > BOARD_RELAY_COUNT) {
        return 0;
    }
    return (relay_state >> (relay - 1u)) & 1u;
}
