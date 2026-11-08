#include "ui.h"

#include "board.h"

#define REG(addr) (*(volatile uint32_t *)(addr))
#define RCC_APB2ENR REG(0x40021018u)
#define GPIOB_CRL REG(GPIOB_BASE + 0x00u)
#define GPIOB_CRH REG(GPIOB_BASE + 0x04u)
#define GPIOB_IDR REG(GPIOB_BASE + 0x08u)
#define GPIOB_BSRR REG(GPIOB_BASE + 0x10u)

#define LED_PIN 7u     /* PB7, LED to 3V3: low = on */
#define BUTTON_PIN 14u /* PB14, to GND when pressed */
#define FACTORY_HOLD_MS 5000u

static ui_state state;
static uint32_t activity_until;
static uint32_t pressed_since;
static int pressed;
static int fired;
static uint32_t debounce_ms;
static uint32_t last_now;

static void led(int on) { GPIOB_BSRR = on ? (1u << (LED_PIN + 16u)) : (1u << LED_PIN); }

void ui_init(void) {
    RCC_APB2ENR |= (1u << 3); /* IOPBEN */
    led(0);
    GPIOB_CRL = (GPIOB_CRL & ~(0xFu << (LED_PIN * 4u))) | (0x2u << (LED_PIN * 4u)); /* PP 2 MHz */
    /* input with pull-up: CNF=10, MODE=00, ODR=1 */
    GPIOB_CRH = (GPIOB_CRH & ~(0xFu << ((BUTTON_PIN - 8u) * 4u))) |
                (0x8u << ((BUTTON_PIN - 8u) * 4u));
    GPIOB_BSRR = 1u << BUTTON_PIN;
}

void ui_set_state(ui_state s) { state = s; }

void ui_activity(void) { activity_until = last_now + 80u; }

int ui_button_pressed(void) { return pressed; }

static int pattern(uint32_t now) {
    switch (state) {
    case UI_NO_LINK:
        return (now % 2000u) < 60u;
    case UI_NO_IP:
        return (now % 500u) < 250u;
    case UI_MQTT_WAIT: {
        uint32_t t = now % 1000u;
        return t < 80u || (t >= 200u && t < 280u);
    }
    default:
        return (int32_t)(now - activity_until) >= 0; /* on, off while activity is fresh */
    }
}

int ui_poll(uint32_t now) {
    last_now = now;
    int raw = (GPIOB_IDR & (1u << BUTTON_PIN)) == 0u;

    /* 30 ms debounce */
    if (raw != pressed) {
        if (debounce_ms == 0u) {
            debounce_ms = now;
        } else if (now - debounce_ms >= 30u) {
            pressed = raw;
            debounce_ms = 0u;
            if (pressed) {
                pressed_since = now;
                fired = 0;
            }
        }
    } else {
        debounce_ms = 0u;
    }

    int trigger = 0;
    if (pressed) {
        uint32_t held = now - pressed_since;
        if (held >= FACTORY_HOLD_MS) {
            led(1);
            if (!fired) {
                fired = 1;
                trigger = 1;
            }
        } else if (held >= 300u) {
            led((now / 60u) & 1u);
        } else {
            led(pattern(now));
        }
    } else {
        led(pattern(now));
    }
    return trigger;
}
