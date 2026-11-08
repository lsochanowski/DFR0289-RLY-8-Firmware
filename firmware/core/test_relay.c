/* Host-side test for the relay map and the driver's safety contract.
 *
 * The map itself is data: assert it here so a typo cannot reach the board. Register
 * writes are not exercised (they target absolute peripheral addresses), except for the
 * out-of-range paths, which must not touch anything at all.
 */
#include <stdio.h>
#include <string.h>

#include "../port/f107/relay.h"

static int failures;
static int checks;

static void expect_int(const char *what, long got, long want) {
    checks++;
    if (got != want) {
        failures++;
        printf("FAIL %s: want %ld, got %ld\n", what, got, want);
        return;
    }
    printf("ok   %s\n", what);
}

static void expect_pin(const char *what, uint8_t relay, uint8_t port, uint8_t pin) {
    relay_pin mapping;
    memset(&mapping, 0xEE, sizeof mapping);
    int mapped = relay_pin_of(relay, &mapping);
    checks++;
    if (!mapped || mapping.port != port || mapping.pin != pin || mapping.active_low != 0) {
        failures++;
        printf("FAIL %s: want port %u pin %u active_low 0, got mapped=%d port=%u pin=%u al=%u\n",
               what, port, pin, mapped, mapping.port, mapping.pin, mapping.active_low);
        return;
    }
    printf("ok   %s\n", what);
}

int main(void) {
    /* Full map, established by bisection on the board (docs/relay-mapping.md).
     * Port index: 0 = A, 1 = B, 2 = C. */
    expect_pin("relay 1 = PC8", 1, 2, 8);
    expect_pin("relay 2 = PC9", 2, 2, 9);
    expect_pin("relay 3 = PC10", 3, 2, 10);
    expect_pin("relay 4 = PC11", 4, 2, 11);
    expect_pin("relay 5 = PC12", 5, 2, 12);
    expect_pin("relay 6 = PC7 (swapped)", 6, 2, 7);
    expect_pin("relay 7 = PC6 (swapped)", 7, 2, 6);
    expect_pin("relay 8 = PB15 (port B)", 8, 1, 15);

    /* All eight are identified, so nothing is refused any more. */
    expect_int("mapped relays = 8", (long)relay_mapped_count(), 8);
    expect_int("board relay count", BOARD_RELAY_COUNT, 8);

    /* Out-of-range requests must be refused without touching the map. */
    relay_pin scratch;
    expect_int("relay 0 not mapped", relay_pin_of(0, &scratch), 0);
    expect_int("relay 9 not mapped", relay_pin_of(9, &scratch), 0);
    expect_int("null out rejected", relay_pin_of(1, 0), 0);

    /* relay_get must not fabricate state for unknown relays. */
    expect_int("relay_get(0) is 0", relay_get(0), 0);
    expect_int("relay_get(9) is 0", relay_get(9), 0);

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
