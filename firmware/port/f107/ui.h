/* Blue status LED (PB7, active low) and the DEFAULT button (PB14, active low).
 * Both pins were found on the board by probing over SWD (docs/hardware-findings.md).
 *
 * LED patterns:
 *   no Ethernet link                 short flash every 2 s
 *   link, no IP address yet          2 Hz blink
 *   IP, MQTT enabled but not ready   double flash every second
 *   IP and MQTT ready (or disabled)  steady on, brief off-flicker on activity
 *   button held                      fast flicker; steady on once the 5 s mark is reached
 */
#ifndef UI_H
#define UI_H

#include <stdint.h>

typedef enum {
    UI_NO_LINK = 0,
    UI_NO_IP,
    UI_MQTT_WAIT,
    UI_READY
} ui_state;

void ui_init(void);
void ui_set_state(ui_state s);
void ui_activity(void); /* relay change / web request: short blink */
/* Call every main-loop pass. Returns 1 exactly once when the button has been held 5 s. */
int ui_poll(uint32_t now_ms);
int ui_button_pressed(void);

#endif /* UI_H */
