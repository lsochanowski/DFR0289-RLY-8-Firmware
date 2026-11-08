# DFR0289 RLY-8 open firmware

Replacement firmware for the **DFRobot RLY-8 (DFR0289) 8-channel relay controller**
(STM32F107 + DP83848 Ethernet, PoE / USB). It turns the board into a **Home Assistant
friendly MQTT device** with a built-in **web panel**, a **USB serial console** and a
**factory-reset button**. You can still switch relays without Home Assistant, from the
panel, the console or plain MQTT.

> Polish user manual: [docs/MANUAL.pl.md](docs/MANUAL.pl.md)

> **Not affiliated with DFRobot.** "DFRobot" and "RLY-8" are used only to say which
> hardware this runs on. Flashing this firmware **replaces the stock firmware**. This
> project does not distribute the original firmware, so back it up first if you may want it
> back (see [Backup](#1-back-up-the-stock-firmware-optional-but-recommended)).
>
> **Relays may switch mains voltage.** Work on the board only when it is disconnected from
> mains. Use at your own risk.

![RLY-8 board with the SWD header, USB-B, Ethernet and DEFAULT button marked](docs/img/board-overview.jpg)

## Why

The stock firmware only speaks DFRobot's own JSON-over-TCP and `55 AA` binary protocols.
It has no Home Assistant integration and no MQTT. This firmware speaks MQTT with Home
Assistant discovery out of the box. It is configured from a browser or over USB. It finds the
Home Assistant broker on its own.

## Features

| | |
| --- | --- |
| **Home Assistant** | MQTT discovery: 8 `switch` entities appear automatically, with availability (online/offline) and a link to the web panel on the device page |
| **Broker autodetection** | empty broker field → mDNS `_mqtt._tcp.local`, then `homeassistant.local`; or set an IP / DNS name / `*.local` name |
| **MQTT** | 3.1.1, username/password, retained state, last-will availability, reconnect with back-off |
| **Web panel** | status, 8 relay buttons, network (DHCP/static), MQTT, device name, admin password, factory reset, JSON status API; HTTP Basic auth + per-boot CSRF token |
| **USB console** | USB-B port enumerates as a CDC serial port: `status`, `show`, `set`, `save`, `relay`, `reboot`, `factory` |
| **mDNS** | reachable as `http://rly8-xxxxxx.local/` (last 3 MAC bytes) |
| **DEFAULT button** | hold 5 s → factory defaults (DHCP, password `admin`, MQTT autodetect) |
| **Blue status LED** | shows link / IP / MQTT state (it does nothing on the stock firmware) |
| **Persistent config** | two flash pages (A/B copies with CRC32), so a power cut while saving cannot brick the settings |
| **Safe boot** | all relays OFF at power-up, no click sequence |

## Hardware notes (measured on a real board)

| Item | Value |
| --- | --- |
| MCU | STM32F107RCT6 (Cortex-M3, 256 KB flash, 64 KB RAM) |
| Clock | HSE **25 MHz** oscillator → 72 MHz via PLL2 |
| Ethernet PHY | TI DP83848, MDIO address 1, **MII**; the PHY has no crystal: its clock comes from the MCU's **MCO pin (PA8)** |
| Relays 1–8 | PC8, PC9, PC10, PC11, PC12, PC7, PC6, PB15 (active high) |
| Blue LED | PB7 (active low) |
| DEFAULT button | PB14 (pressed = low) |
| USB-B | native OTG FS (PA11/PA12). PA9 sees no VBUS on this board, so the firmware forces it high |
| Debug | 4-pin SWD header: **SWDIO, SWCLK, GND, +3.3V** |

More detail: [docs/hardware-findings.md](docs/hardware-findings.md),
[docs/relay-mapping.md](docs/relay-mapping.md).

## Installing

### What you need

- an **ST-Link** (V2 clone is fine) or any other SWD probe, 4 jumper wires
- [pyOCD](https://pyocd.io) (`pip install pyocd`). OpenOCD or STM32CubeProgrammer also work
  with any SWD probe, but we only tested pyOCD with an ST-Link
- the release file `rly8-fw-vX.Y.Z.bin` (or `.hex`) from
  [Releases](https://github.com/lsochanowski/DFR0289-RLY-8-Firmware/releases)

### Wiring

![SWD header close-up](docs/img/swd-header.jpg)

| Board header | ST-Link |
| --- | --- |
| SWDIO | SWDIO |
| SWCLK | SWCLK |
| GND | GND |
| +3.3V | 3.3V (optional if the board is powered by PoE/DC; then connect only GND + signals) |

Power the board normally (PoE or DC). Do not feed 3.3 V from the probe and from the board
regulator at the same time unless your probe is designed for it.

### 1. Back up the stock firmware (optional but recommended)

```bash
pyocd cmd -t stm32f103rc -c "savemem 0x08000000 0x40000 rly8-stock.bin"
```

If the read returns errors or only zeros, the chip may have read-out protection. Then the
stock firmware cannot be saved, and flashing needs a mass erase first (`pyocd erase --chip`).

### 2. Flash

```bash
pyocd flash -t stm32f103rc rly8-fw-v1.0.0.bin --base-address 0x08000000
pyocd reset -t stm32f103rc
```

`-t stm32f103rc` is intentional: pyOCD has no built-in STM32F107 target, and the flash
algorithm of the F1 family is the same for this 256 KB part. With more than one probe
connected, add `-u <probe UID>` (list them with `pyocd list`). With the `.hex` file,
`--base-address` is not needed.

### 3. First boot

1. Plug in Ethernet. The board asks for an address over DHCP.
2. Open `http://rly8-xxxxxx.local/`, or look up the address in your router. You can also
   type `status` in the USB console.
3. Log in as **admin / admin** and **change the password** under *Ustawienia* (Settings).

If no DHCP server answers within 15 s, the board uses the fallback address
**192.168.9.50/24**. You can change it in the panel.

## Using it

### Web panel

The panel UI is in Polish:

- **Status**: link, IP, MQTT state and broker, 8 relay toggles, all-on/all-off, restart
- **Ustawienia** (Settings): DHCP or static IP, host name, MQTT on/off, broker (empty =
  autodetect), port, user, password, discovery prefix, device name, admin password,
  factory reset
- `GET /api/status`: JSON `{"node","name","fw","ip","link","uptime","mqtt","relays":[…]}`

Saving settings writes flash and reboots the device.

### Home Assistant

1. In Home Assistant you need a broker, for example the **Mosquitto** add-on, and the
   **MQTT** integration.
2. In the board's panel, leave **Broker** empty (autodetect). Enter the **MQTT user and
   password**: the Mosquitto add-on rejects anonymous clients.
3. The device appears under *Settings → Devices → MQTT* with 8 switches.

Topics (`<node>` = host name, default `rly8-xxxxxx`):

| Topic | Direction | Payload |
| --- | --- | --- |
| `homeassistant/switch/<node>/relay<N>/config` | device → HA (retained) | discovery JSON |
| `<node>/relay<N>/set` | HA → device | `ON` / `OFF` |
| `<node>/relay<N>/state` | device → HA (retained) | `ON` / `OFF` |
| `<node>/status` | device → HA (retained, last will) | `online` / `offline` |

The `homeassistant` prefix can be changed in the settings.

### USB console

Connect the USB-B port to a computer. It shows up as a serial port: `/dev/cu.usbmodem*`
on macOS, `/dev/ttyACM*` on Linux, a COM port on Windows (VID:PID `0483:5740`). The baud
rate does not matter.

```bash
screen /dev/cu.usbmodem* 115200
```

```text
help                             list commands
status                           network, MQTT, relays
show                             configuration (passwords masked)
set <key> <value>                change a setting (pending until 'save')
save                             store settings (applied after reboot)
reboot
factory                          factory defaults + reboot
relay <1-8|all> <on|off|toggle>
```

Setting keys:

| Key | Meaning | Default |
| --- | --- | --- |
| `dhcp` | `on` / `off` | on |
| `ip`, `mask`, `gw`, `dns` | static address; also the fallback when DHCP fails | 192.168.9.50 / 255.255.255.0 / 192.168.9.1 / 192.168.9.1 |
| `hostname` | DHCP / mDNS name, empty = `rly8-xxxxxx` | empty |
| `name` | device name in Home Assistant | RLY-8 |
| `mqtt` | `on` / `off` | on |
| `mqtt_host` | IP, DNS name, `*.local`; empty = autodetect | empty |
| `mqtt_port` | | 1883 |
| `mqtt_user`, `mqtt_pass` | broker credentials | empty |
| `mqtt_prefix` | HA discovery prefix | homeassistant |
| `admin_pass` | web panel password (user `admin`) | admin |

### DEFAULT button and status LED

Hold **DEFAULT** for 5 seconds. The blue LED flickers fast while you hold it, then lights
steadily and the board restarts with factory settings: DHCP, password `admin`, MQTT
autodetect.

| Blue LED | Meaning |
| --- | --- |
| short flash every 2 s | no Ethernet link |
| blinking 2× per second | link, waiting for an IP address |
| double flash every second | has IP, MQTT not connected yet (or wrong credentials) |
| steady on, short blink on activity | everything up |

## Building from source

Requirements: `arm-none-eabi-gcc` (tested with Arm GNU Toolchain 13.3), `make`, a host C
compiler for the unit tests.

```bash
cd firmware
make test       # host unit tests (protocol, MQTT, config, HTTP, panel, console, mDNS)
make app        # build/app.elf
make release    # dist/rly8-fw-v<version>.{bin,hex,elf} + SHA256SUMS
make flash      # pyOCD; add STLINK_UID=<uid> to pick a probe
```

Layout:

```text
firmware/
  core/        portable logic, unit-tested on the host (MQTT, HA discovery, config,
               HTTP parser, web pages, console, mDNS, stock protocol codecs)
  port/f107/   STM32F107 drivers: clock, GPIO/relays, Ethernet MAC+DMA, lwIP glue,
               HTTP server, mDNS, flash config, USB CDC, LED/button
  app/         startup + main loop
  third_party/ lwIP 2.2.0, ST CMSIS device header
docs/          hardware findings, relay mapping, stock protocol notes
```

## Security notes

- The panel uses **HTTP Basic auth without TLS**. Use it only on a trusted network, and
  change the default password.
- Passwords (panel, MQTT) are stored **unencrypted** in the MCU flash.
- The **USB console has no password**: anyone with physical access can change settings.
  They can also reset everything with the button anyway.
- State-changing requests are POST-only and carry a per-boot CSRF token.

## Limitations / roadmap

- Not implemented yet: the stock protocols (JSON on TCP port 2000, `55 AA` frames over RS485).
  Codecs exist in `core/` and are tested; they are not wired to TCP/RS485.
- Relay states are not restored after a power cut; all relays start OFF.
- Firmware update over the web panel is planned.

## License

MIT, see [LICENSE](LICENSE). Third-party components keep their own licenses: lwIP (BSD-3-Clause,
in each source file) and the ST CMSIS device header (Apache-2.0). See
[THIRD_PARTY.md](THIRD_PARTY.md).
