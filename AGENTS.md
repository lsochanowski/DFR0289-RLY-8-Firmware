# AGENTS.md — RLY-8 (DFRobot DFR0289) replacement firmware

Everything here was measured on the actual board over SWD, not copied from a reference
design. If you change a fact, change it here too.

Environment-specific values (probe UID, network addresses, local paths) live in
`AGENTS.local.md`, which is git-ignored. The repository is public: never commit such values.

Goal: replace the stock firmware so the 8-relay board works in Home Assistant over MQTT
(later also with the stock JSON protocol on Ethernet TCP:2000).

---

## 1. Hardware — facts established by measurement

| Item | Value | How it was established |
| --- | --- | --- |
| MCU | **STM32F107RCT6** (LQFP64, Cortex-M3 r1p1; package marking) | CPUID `0x411FC231`, DBGMCU_IDCODE `0x10016418` → DEV_ID `0x418` |
| Flash / RAM | 256 KB / 64 KB | `0x1FFFF7E0` = `0x0100`, initial SP `0x20010000` |
| RDP | level 0 (no read protection) | option bytes `0x1FFFF800` = `0x00FF5AA5` |
| HSE | **25 MHz** (4-pad oscillator between the MCU and the PHY) | DWT_CYCCNT with `SW=HSE` = 25.00 MHz (2026-11-07) |
| SYSCLK | **72 MHz**: 25 /5 (PREDIV2) ×8 (PLL2) /5 (PREDIV1) ×9 (PLL) | DWT_CYCCNT = 72.00 MHz. The old "HSE 8 MHz ×9" setting ran at ~139 MHz (PLL saturated) |
| **PHY clock** | **DP83848 X1 comes from MCO = PA8**; the PHY has no crystal of its own | without MCO: MDIO works but nothing on the wire; MCO=HSE → 100M FD link |
| ETH mode | **MII** (PHY strap, `RBR=0x0001`); MCO = HSE 25 MHz | DHCP + ping 300/300 (2026-11-07) |
| **PLL from HSI/2 does not lock** | — | HSI/2 ×16 never set `PLLRDY`; always use HSE |
| Ethernet PHY | **TI DP83848AA**, MDIO address **1** | ID read `0x2000:0x5C90` + `eth_init()` in the firmware |
| Unique ID | 96 bit, different on every unit (the MAC is derived from it) | `0x1FFFF7E8..F0` |

### Relay map (COMPLETE, confirmed by observation)

| Relay | Pin | | Relay | Pin |
| --- | --- | --- | --- | --- |
| 1 | **PC8** | | 5 | **PC12** |
| 2 | **PC9** | | 6 | **PC7** |
| 3 | **PC10** | | 7 | **PC6** |
| 4 | **PC11** | | 8 | **PB15** |

All channels are **active high**. Two quirks: relays 6 and 7 are **swapped** relative to the
natural PC6/PC7 order, and relay 8 sits alone on port B (PB15).
Details and test runs: `docs/relay-mapping.md`.

### User interface and USB (measured 2026-11-07/08)

| Item | Pin | How it was established |
| --- | --- | --- |
| **Blue LED** | **PB7, active low** | bisection by blinking pin groups over SWD + observation |
| **DEFAULT button** | **PB14, pressed = 0** (internal pull-up) | IDR polled over SWD during 3 presses |
| USB-B | native OTG FS, PA11/PA12 | the vendor driver zip = ST VCP (`0483:5740`) |
| **VBUS on PA9** | **none** — PA9 is pulled to ground on the board | with the cable plugged in PA9=0 even with the pull-up; `BSVLD=0` |

USB works only with a workaround: the firmware drives **PA9 as an output, high**
(`USB_NO_VBUS_SENSE=1`, default in `usb_cdc.c`). Consequence: cable removal is not detected
(the host resets the device on re-plug anyway).

### Pins that MUST NOT be driven

- **PA13 / PA14** — SWDIO/SWCLK. Driving them kills the debugger, and every RAM read after
  that is garbage (this already bit us once).
- **PA2 / PC1** — MDIO / MDC (Ethernet alternate function).
- **PC6–PC12, PB15** — relays; exclude them in diagnostic probes so the coils do not click
  and do not cause supply dips.

---

## 2. Flashing and debugging (SWD)

Probe: ST-Link (your probe UID: `pyocd list`; locally in `AGENTS.local.md`, outside the
repo). **There is no ROM DFU/BOOT0 path — use SWD.**

```bash
cd firmware && make flash STLINK_UID=$STLINK_UID   # bootloader + application (build/full.hex)
```

**Since v2 the application is linked at `0x08002000`** (behind the 8 KB bootloader,
`port/f107/app.ld`). Never flash `app.elf` alone onto a chip without the bootloader; use
`make flash` (full.hex). Flash map and update protocol: `core/rly_ota.h`. Diagnostic probes
(`port/probe/f107.ld`) still link at `0x08000000` and overwrite the bootloader.

Updates without a probe: web panel *Settings → Firmware update* with `dist/*-update.bin`
(or `curl -u admin:<pass> --data-binary @app.bin "http://<ip>/update?t=<token>"`; the token
is in the settings page source).

- `-t stm32f103rc` (not `stm32f107…`) — pyOCD has no built-in F107 target; the F1 flash
  algorithm is shared and the 256 KB size matches.
- **Always `reset` after `flash`** — if the image is identical, pyOCD writes nothing
  ("identical N bytes") and the firmware **does not restart**.
- Reading registers/memory:
  ```bash
  PYTHONPATH=work/toolchain/py python3 -m pyocd cmd -u <UID> -t cortex_m \
    -c "read32 0xE000ED28" -c "savemem 0x20000830 0x54 work/bs.bin"
  ```
- SWD reads during a flash erase fail with "DP wait" (the bus is stalled) — in test scripts
  treat that as "the bootloader is working", not as an error.

### `boot_status` — firmware status report (read it over SWD)

The address **changes with every layout change** — take it from the image:

```bash
cd firmware && arm-none-eabi-nm build/app.elf | grep boot_status   # e.g. 0x20000b0c
```

Field order (`uint32_t`, little-endian):

| # | field | meaning |
| --- | --- | --- |
| 0 | magic | `0x524C5931` ("RLY1") |
| 1 | clock_result | 0 = 72 MHz from HSE OK |
| 2 | relays_mapped | should be 8 |
| 3 | sequence_done | `0x444F4E45` ("DONE") |
| 4 | eth_init_result | 0 = PHY found and configured |
| 5 | phy_address | 1 = DP83848AA |
| 6 | link_up | 1 = physical link |
| 7 | bmsr | PHY register 0x01 |
| 8 | physts | PHY register 0x10 |
| 9 | poll_count | main loop counter |
| 10 | eth_dma_result | 0 = MAC+DMA started |
| 11 | netif_up | 1 = lwIP netif up |
| 12 | dhcp_started | 1 = DHCP started (needs a link) |
| 13 | ip_addr | IPv4 address (0 = none) |
| 14 | rx_frames | received frames |
| 15 | tx_frames | sent frames |
| 16 | up_ms | ms since boot (proves SysTick runs) |
| 17 | mqtt_state | 0 idle, 1 connecting, 2 ready, 3 error |
| 18 | mqtt_commands | MQTT commands applied |
| 19 | relay_mask | current relay state (bit0 = relay 1) |
| 20 | static_ip | 1 = fallback/static address in use after DHCP timeout |
| 21 | bmcr | PHY register 0x00 (bit11 power-down, bit10 isolate) |
| 22 | rbr | PHY register 0x17 (bit5 = RMII mode) |
| 23 | phyctrl | PHY register 0x19 |
| 24 | cfg_loaded | 1 = config from flash, 0 = factory defaults |
| 25 | http_requests | HTTP requests served |
| 26 | usb_configured | 1 = host configured the USB CDC device |
| 27 | broker_ip | MQTT broker address (after autodetect/DNS) |

USB counters: symbol `usb_cdc_stats` (irqs, resets, enum_done, setups, stalls, …).

---

## 3. Firmware — architecture and build

```
firmware/
  core/            # portable, unit-tested on the host (no hardware)
    rly_frame.c    # 55 AA frame + CRC = 8-bit sum (stock RS485/USB protocol)
    rly_json.c     # stock JSON dialect (TCP:2000)
    rly_ha.c       # Home Assistant MQTT discovery payloads
    rly_mqtt.c     # MQTT 3.1.1 packets (CONNECT/PUBLISH/SUBSCRIBE + parser)
    rly_session.c  # session: CONNACK → discovery → states → commands
    rly_mqtt_client.c # non-blocking client: back-off, keepalive, reconnect
    rly_config.c   # config: struct, CRC32, key=value validation (web + console)
    rly_http.c     # HTTP parser, forms, Basic auth, HTML escaping
    rly_web.c      # web panel: pages, /save, /relay, /factory, /update, /api/status, CSRF token
    rly_console.c  # text console (help/status/show/set/save/relay/reboot/factory)
    rly_mdns.c     # mDNS: PTR/A queries, response parser, A answer for <host>.local
    rly_ota.c      # OTA: image validation, streaming writer, bootloader decision and install
    rly_crc.c      # CRC-32
    rly_version.h  # single source of the version number
    test_*.c       # host tests
  port/f107/       # hardware layer
    board.h        # MCU, clocks, GPIO
    clock.c        # HSE 25 MHz → 72 MHz, MCO for the PHY
    relay.c        # relay driver (pin map in one place)
    eth.c          # PHY: pins, MDIO, autoneg, link, MII/RMII (AFIO_MAPR bit 23)
    eth_dma.c      # MAC + DMA, RX×4 / TX×2 rings, descriptors, zero-copy helpers
    ethernetif.c   # lwIP glue (polling)
    tcp_stream.c   # rly_stream on an lwIP TCP socket (for the MQTT client)
    flash.c        # flash erase/program (shared: config, OTA, bootloader)
    flash_cfg.c    # config in the last 2 flash pages (A/B, 0x0803F000/F800)
    app.ld         # application linker script: 0x08002000, image header at +0x200
    httpd.c        # HTTP server on the lwIP raw API (2 slots + queue of 4, streamed upload)
    discovery.c    # mDNS responder + broker autodetect (_mqtt._tcp, homeassistant.local), DNS
    usb_cdc.c      # USB OTG FS device, CDC-ACM (console), IRQ 67
    ui.c           # LED PB7 (state patterns) + button PB14 (5 s = factory reset)
    systick.c      # 1 ms tick + sys_now() for lwIP
    lwip/arch/     # cc.h, sys_arch.h, sys_arch.c (critical sections via PRIMASK, LWIP_RAND)
    ../probe/      # diagnostic programs (see §6)
  app/
    startup.c      # vector table (designated indices!), image header, VTOR, reset handler
    main.c         # boot: config → clock → relays → systick → LED/USB → PHY → MAC/DMA → lwIP
                   #       → web, mDNS, MQTT; actions (save, restart, factory, OTA upload)
  boot/            # bootloader (8 KB; boot.c + boot.ld), logic in core/rly_ota.c
  third_party/
    lwip/          # lwIP 2.2.0 (only the needed directories) + the project's lwipopts.h
    cmsis/         # stm32f107xc.h (register definitions, source of bit names)
```

### Commands

```bash
cd firmware

make test          # 9 host test suites (~372 checks) — must be green
make app           # application: app + port + core + lwIP → build/app.elf
make full          # bootloader + application → build/full.bin/.hex
make release       # dist/: -full.bin/-full.hex (SWD), -update.bin (web panel), .elf, SHA256SUMS
make flash         # pyOCD, flashes build/full.hex (STLINK_UID=<uid> picks the probe)
make port-layer    # compile the port/f107 layer (no link)
make probe-*       # diagnostic programs (see §6)
```

Image (v2.1.0): **app text 67 137 B, data 20 B, bss 55 276 B** (RAM ~55 KB of 64 KB, ~9 KB
for the stack); bootloader 1.3 KB. Linked with `--gc-sections`, map in `build/app.map`.
pyOCD erases only the sectors of the image, so the config pages at the end of flash survive.
Toolchain: `work/toolchain/arm-gnu-toolchain-13.3.rel1-.../bin/arm-none-eabi-gcc` (13.3.1;
otherwise `arm-none-eabi-gcc` from PATH), pyOCD 0.46.1 in `work/toolchain/py`. `brew` is
useless here — the sandbox blocks writes outside the workspace.

### Device configuration (runtime, not build time)

Stored in flash (A/B + CRC32, the newer valid copy wins). Changed through:
- the **web panel** `http://<ip>/` or `http://rly8-xxxxxx.local/` — login **admin**, password
  **admin** by default; Status (relays), Settings (network, MQTT, password, firmware update),
  `/api/status` (JSON). The UI is in English (since 2.1.0).
- the **USB console** (`/dev/cu.usbmodem*`, any baud rate): `help`, `status`, `show`,
  `set <key> <value>`, `save`, `reboot`, `factory`, `relay <1-8|all> <on|off|toggle>`.

Keys: `dhcp ip mask gw dns hostname name mqtt mqtt_host mqtt_port mqtt_user mqtt_pass
mqtt_prefix admin_pass`. Factory defaults: DHCP, fallback/static `192.168.9.50/24`,
MQTT on, **empty `mqtt_host` = autodetect**, port 1883, prefix `homeassistant`, password
`admin`. **Factory reset**: DEFAULT button for 5 s, `factory` in the console, or the panel.

Broker autodetect (empty `mqtt_host`): mDNS PTR `_mqtt._tcp.local` (SRV → host+port), then
`homeassistant.local` (A) with the configured port; retried every 10 s. A `*.local` name →
mDNS A; any other name → DNS (server from DHCP or `dns`). Measured on a home network:
`homeassistant.local` found, the broker requires a login (CONNACK rc=5 without credentials).

LED: no link = short flash every 2 s; link without IP = 2 Hz; IP but MQTT not ready = double
flash every second; all good = on, briefly off on activity; button held = fast flicker, after
5 s steady and restart. Bootloader: steady while installing; ~8 Hz forever = no valid app.

Topics (HA MQTT discovery): `<prefix>/switch/<node>/relay<N>/config`,
`<node>/relay<N>/set` (`ON`/`OFF`), `<node>/relay<N>/state` (retained),
`<node>/status` (`online`/`offline`, retained will). `node` = `hostname` or
`rly8-<last 3 MAC bytes>`. Discovery includes `cu` (link to the panel on the HA device page).

---

## 4. Stock protocol (to be reproduced — `docs/protocol.md`)

- **JSON over TCP:2000**: `{"get":"relayStatus"}`, `{"relayN":"on"}`, `{"get":"netconfig"}`,
  `{"dhcp":"on"}`, `{"reboot":"1"}`; responses `{"resp":"ok"|"error"|"unsupported"}`.
- **Binary frames (USB CDC / RS485)**: `55 AA | addr | len | cmd | data[len] | crc`,
  **crc = 8-bit sum** of all preceding bytes (verified on 5 datasheet examples). Some `cmd`
  codes are not documented in hex: name, version, DHCP, restart.
- Implemented and tested, not wired to TCP/RS485 yet: `core/rly_frame.c`, `core/rly_json.c`.

---

## 5. Status: what works, what does not

**Works (measured 2026-11-07):** 72 MHz clock (DWT), 8/8 relays, PHY at MDIO=1,
**100 Mb/s full-duplex link** (`physts=0x0715`), MAC+DMA in MII, lwIP, **DHCP** (address on
the switch network), **ping 300/300 without loss** (also 1400 B).

What blocked Ethernet (fixed):
1. **No PHY clock** — DP83848 X1 is wired to PA8/MCO; the firmware never enabled MCO.
   The PHY answered over MDIO but did not transmit (`physts` without "signal detect", the
   link partner stayed `inactive`). Fix: `clock_phy_mco()` in `port/f107/clock.c`, called
   from `eth_init()`.
2. **Wrong MCU clock** — HSE is 25 MHz, not 8 MHz; PLL ×9 → ~139 MHz. Fix: PLL2 chain.
3. **RMII forced on a PHY strapped for MII** — now `ETH_MODE_MII=1` by default (MII inputs
   CRS/COL/RX_ER/TX_CLK as inputs). RMII as an alternative: `-DETH_MODE_MII=0` (MCO = PLL3
   50 MHz; the link also comes up in RMII, the RMII data path is untested).
4. **lwIP sent nothing** — `netif_set_link_up()` was missing; `main.c` now tracks the PHY link.
5. **DMA descriptors** — added `TCH`/`RCH` (chained mode), RX read in order.

**Web panel, USB console, flash config, mDNS, button, LED — working (measured
2026-11-08):** 6 parallel HTTP requests OK (max 0.48 s), save → restart → `cfg_loaded=1`,
factory reset by button → flash pages `0xFF`, the USB console enumerates as
`RLY-8 open firmware` (`0483:5740`), autodetect found the HA broker.

**OTA (v2.0.0, measured 2026-11-08):** 67 KB upload in 2.8 s, back with the new version after
~24 s; rejected: v1 image, `full.bin`, random file, bad token (403), bad password (401); a
dropped upload changes nothing; MCU reset halfway through the copy → the bootloader repeats
the install (the `OTAP` record stays, the app slot was already erased) → the new version
boots; settings survive OTA.

**v2.1.0 (2026-11-08):** web panel and messages in English; README in English with the
Polish translation below. Installed through the web panel (OTA 2.0.0 → 2.1.0, 24 s), no
ST-Link involved.

**Open:** MQTT `mqtt_state=3` — the HA broker requires a login; set `mqtt_user`/`mqtt_pass`
in the panel and check the 8 switches in HA.

---

## 6. Diagnostic programs (`firmware/port/probe/`)

| purpose | command |
| --- | --- |
| MDIO scan (32 addresses, diagnostics) | `make probe` → `build/probe.elf` |
| MDIO soak (1000 rounds) | `make probe-soak` |
| does the PHY come up late | `make probe-wait` |
| whole pin bank at once + mask | `make probe-bank EXTRA="-DBANK=1 -DMASK=0x7FFF"` |
| **all pins except relays and SWD** | `make probe-all EXTRA=-DDRIVE_LOW=1` |
| all pins + simultaneous MDIO scan | `make probe-all-mdio` |
| identify a relay by the number of clicks | `make probe-count EXTRA="-DPORT_INDEX=2 -DSTART_PIN=8 -DEND_PIN=15"` |
| one pin at a time, no counting ("hold" run) | `make probe-hold EXTRA="-DPORT_INDEX=2 -DSTART_PIN=0 -DEND_PIN=7"` |
| sweep giving each pin a different click count | `make probe-multi` |
| light a whole bank (old variant) | `make probe-relay` |
| GPIO pin scan with MDIO (older probe) | `make probe-gpio` |

All probes store their result in RAM at `0x20008000` (magic `0x524C5931`) and a boot counter
at `0x2000F000` (pattern `0xB007`) — the latter tells "loop" from "the board keeps resetting".
Probes link at `0x08000000` and overwrite the bootloader: run `make flash` afterwards.

---

## 7. Next steps (in this order)

1. ~~PHY / link diagnostics~~ — **done** (see §5). Without DHCP the firmware switches to
   `192.168.9.50/24` after 15 s (`static_ip=1`).
2. **MQTT in HA** — enter the MQTT login in the panel, check the 8 switches in HA
   (discovery), HA → relay switching (`relay_mask` in `boot_status`).
3. **JSON server on TCP:2000** — compatibility with the stock DFRobot firmware (use
   `core/rly_json.c`).
4. **RS485 / `55 AA` frames** — bus compatibility (`core/rly_frame.c`).
5. Housekeeping: watchdog (IWDG), signed OTA images, optionally restoring relay states after
   a restart (today: all OFF at boot, no click sequence), USB console authentication (today:
   none — physical access is assumed).

---

## 8. Lessons (so nobody repeats them)

- **Do not trust "HSERDY = X MHz crystal"** — measure the frequency: enable DWT_CYCCNT
  (`DEMCR` bit 24, `DWT_CTRL` bit 0), switch to `SW=HSE` and count cycles against host time.
- **A PHY that answers over MDIO does not necessarily have a clock** — MDIO is clocked by
  MDC. No "signal detect" (PHYSTS bit 10) + silence on the other end of the cable = check X1/MCO.
- lwIP 2.x: without `netif_set_link_up()` no frame leaves (`tx_frames=0`).
- **64 KB RAM is tight**: `--gc-sections` drops newlib locale/malloc (2 KB data); frames are
  copied DMA↔pbuf directly (`eth_dma_rx_peek/tx_acquire`), without bounce buffers.
- **Main loop without delays** → read the PHY state every 20 ms (MDIO is slow, and the
  autoneg retry counter counts calls).
- **mDNS without IGMP**: lwIP accepts multicast with `LWIP_IGMP=0`, but the MAC needs `PAM`
  (`MACFFR` bit 4), otherwise frames to 224.0.0.251 are dropped by the filter.
- **USB OTG FS on the F107 has no `NOVBUSSENS`** — without VBUS on PA9 the only way is to
  force PA9 high.
- **`#` inside `$(shell …)` in a Makefile starts a comment** — in the sed expression use
  `.define` or `\#`.
- **`MACMIIAR` is `MAC_BASE + 0x10`**, `MACMIIDR` is `+0x14`. A wrong address looks like
  "MB hangs forever" but is really reading back your own write.
- **Vector table with designated indices only** (`[15] = SysTick_Handler`). A positional list
  once shifted SysTick onto the PendSV slot → jump to address 0 → `INVSTATE` + HardFault
  (`CFSR=0x00020000`, `HFSR=0x40000000`).
- **lwIP `NO_SYS=1` + `SYS_LIGHTWEIGHT_PROT=1`**: `lwip/sys.h` does not pull in
  `arch/sys_arch.h` in the NO_SYS branch, so `sys_prot_t` has to be defined from `cc.h`.
- **newlib**: link with `-nostartfiles -specs=nosys.specs` (own startup), and add `end`,
  `_end`, `__bss_start__`, `__bss_end__` to the linker script (newlib `sbrk`).
- **STM32F1: the ETH mode is selected by `AFIO_MAPR` bit 23** (`MII_RMII_SEL`), not by the MAC.
- **Buffers on the stack**: a 1524 B frame + 512 B MQTT packets blow the small stack — keep
  them `static`.
- **`make` and the `port/` directory**: a target named like a directory is always "up to
  date" — hence `make port-layer`, not `make port`.
- **Cast `uint32_t` → pointer through `uintptr_t`**, otherwise the host build (tests) fails
  with `-Werror`.
- **Phone photos carry GPS in EXIF** — publish only re-encoded copies without metadata.

---

## 9. Documentation in the repo

| file | content |
| --- | --- |
| `README.md` | user manual (English, Polish translation below) |
| `docs/hardware-findings.md` | all hardware measurements + PHY pinout for the multimeter |
| `docs/relay-mapping.md` | relay map, method (mask bisection) and test runs |
| `docs/protocol.md` | stock protocol: `55 AA` frames + JSON |
| `docs/STATUS.md` | historical project status notes |
| `docs/firmware-plan.md` | historical plan, milestones, risks |
| `tools/dump-original-fw.md` | procedure to dump the stock firmware (read-only) |
