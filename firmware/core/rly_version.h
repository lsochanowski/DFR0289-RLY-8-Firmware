/* Single source of the firmware version (web panel, console, HA discovery "sw", image
 * header). Overridable for test builds: make EXTRA_CFLAGS='-DRLY_FW_VERSION=\"2.0.0-test\"' */
#ifndef RLY_VERSION_H
#define RLY_VERSION_H

#ifndef RLY_FW_VERSION
#define RLY_FW_VERSION "2.1.0"
#endif

#endif /* RLY_VERSION_H */
