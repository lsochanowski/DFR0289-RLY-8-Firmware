# Replacement firmware plan for the RLY-8 (DFR0289)

*Polski: [niżej](#polski)*

*Historical development notes; the current state and user manual are in [README.md](../README.md).*

Goal: 8 relays visible in Home Assistant via MQTT discovery, while keeping compatibility with
the original API (JSON/TCP:2000 and `55 AA` frames on RS485).

## 1. Choice of stack

**Bare-metal: STM32Cube HAL + lwIP + our own codec/MQTT** (`firmware/core/`).

Why not Zephyr/an RTOS:

- the device does one thing — 8 outputs, one TCP socket, one MQTT client;
- memory budget: the C core takes ~2.5 KB of `.text` (measured: `rly_frame.o` 464 B,
  `rly_json.o` 775 B, `rly_ha.o` 1257 B) — lwIP + HAL will fit even in 256 KB of flash
  and 64 KB of RAM, i.e. in the weakest candidate (STM32F107);
- no dependency on `west`/CMake or on the network at build time — important, because the
  sandbox blocks writes outside the workspace.

If it turns out there is an F4 with 1 MB of flash inside, the plan does not change: we will
spend the extra memory on log buffering and OTA, not on an RTOS.

## 2. Directory layout

```
firmware/
  core/        # portable, tested on the host (already done)
    rly_frame.c    # 55 AA frame + sum8 CRC, commands
    rly_json.c     # the original's JSON dialect
    rly_ha.c       # MQTT discovery payloads for HA
    rly_mqtt.c     # MQTT 3.1.1 packets
    rly_session.c  # session logic: CONNACK -> discovery -> states -> commands
  port/        # everything that depends on the specific STM32 (to be written)
    board.h             # pinout: relays, LED, RS485 DE, PHY reset
    clock.c             # HSE/PLL, 72/168 MHz clocking depending on the family
    eth.c               # RMII/MII + PHY init (PHY-dependent!)
    netif.c             # lwIP glue
    config_store.c      # settings in the last flash page (IP, name, MQTT)
    relay.c             # GPIO + state after startup
  app/
    main.c              # init, loop, keepalive
```

`core/` knows nothing about the hardware — that is why it has host tests, and why it can be
handed over for review without a board.

## 3. Entity model in Home Assistant

- 8 × `switch` — `homeassistant/switch/<node_id>/relay<N>/config`
  - `cmd_t`: `<node_id>/relay<N>/set` (`ON`/`OFF`), `stat_t`: `<node_id>/relay<N>/state` (retained)
- availability: `<node_id>/status` = `online`/`offline`, as a **retained will** in CONNECT
  (this way HA shows `unavailable` after a disconnect, without waiting for a timeout)
- one device group (`dev.ids`) — 8 switches as one device, not 8 separate ones

## 4. Backward compatibility

- TCP:2000 stays — the same JSON dialect, so `Fescron/DFRobot-RLY8-python3` and the DFRobot
  application keep working (code: `rly_json.c`).
- RS485: `55 AA` frames with an address (`rly_frame.c`); the missing `cmd` codes (name,
  version, DHCP, restart) will be reconstructed empirically — see `docs/protocol.md` §4.

## 5. What is missing to write `port/`

Table to be filled in after identifying the board:

| Item | Value | Source |
| --- | --- | --- |
| MCU (family, flash/RAM) | ? | marking on the PCB |
| PHY + its MDIO address | ? | chip next to the RJ45 (DP83848 vs LAN8720 differ in interface: RMII + REF_CLK) |
| HSE clock | ? | crystal on the board (typically 25 MHz with LAN8720) |
| Relays: GPIO or expander | ? | drivers next to the relays (ULN2803 / 74HC595) |
| State after reset | ? | whether the coils are active low (important: a relay may "click" at startup) |
| RS485 DE/RE | ? | transceiver next to the connector |
| Flashing access | ? | BOOT0 / SWD pads |

## 6. Flashing paths (to be determined on the hardware)

| Path | Requires | Notes |
| --- | --- | --- |
| ROM DFU | BOOT0 + USB | no extra hardware; check whether BOOT0 is broken out |
| SWD | ST-Link/J-Link/DAPLink | also gives read-out and debugging; `pyOCD` already set up in `work/toolchain/py` |
| UART bootloader | 3V3 adapter | last resort, requires AN3155 |

## 7. Milestones

- **M0** — board identification (MCU/PHY/pins), flashing path chosen. *blocks everything after it*
- **M1** — LED blinks, UART/console works, relay pinout confirmed (one relay clicks)
- **M2** — Ethernet comes up: link, DHCP, ping; verification that the PHY is configured correctly
- **M3** — MQTT: discovery + 8 switches controlled from HA
- **M4** — backward compatibility: TCP:2000 JSON + RS485

## 8. Risks

- **PHY**: the most common cause of "Ethernet does not come up" is the wrong mode (RMII vs MII),
  a missing 50 MHz REF_CLK, or a wrong MDIO address. Without the PHY marking there is no point
  in guessing.
- **Relays at startup**: if the coils are driven active low, a GPIO in the high-impedance state
  may switch them on momentarily. In `port/relay.c` we set the output state **before**
  configuring the pin as an output.
- **RDP**: if the original firmware has readout protection enabled, reading the flash will
  fail. Flashing new firmware is still possible (mass erase), but we lose the reference.
- **PoE**: 44–57 V on the board — measure only with power disconnected, or deliberately and carefully.

---

## Polski

### Plan firmware zastępczego dla RLY-8 (DFR0289)

*Historyczne notatki z rozwoju; aktualny stan i instrukcja są w [README.md](../README.md).*

Cel: 8 przekaźników widocznych w Home Assistant przez MQTT discovery, przy zachowaniu
zgodności z oryginalnym API (JSON/TCP:2000 i ramki `55 AA` na RS485).

### 1. Wybór stosu

**Bare-metal: STM32Cube HAL + lwIP + własny kodek/MQTT** (`firmware/core/`).

Dlaczego nie Zephyr/RTOS:

- urządzenie robi jedną rzecz — 8 wyjść, jeden socket TCP, jeden klient MQTT;
- budżet pamięci: rdzeń w C zajmuje ~2,5 KB `.text` (zmierzone: `rly_frame.o` 464 B,
  `rly_json.o` 775 B, `rly_ha.o` 1257 B) — lwIP + HAL zmieści się nawet w 256 KB flash
  i 64 KB RAM, czyli w najsłabszym kandydacie (STM32F107);
- brak zależności od `west`/CMake i sieci przy budowaniu — ważne, bo sandbox blokuje
  zapis poza workspace.

Jeśli okaże się, że w środku jest F4 z 1 MB flash, plan się nie zmienia: nadmiar pamięci
zużyjemy na buforowanie logów i OTA, nie na RTOS.

### 2. Układ katalogów

```
firmware/
  core/        # przenośne, testowane na hoście (już gotowe)
    rly_frame.c    # ramka 55 AA + CRC suma8, komendy
    rly_json.c     # dialekt JSON oryginału
    rly_ha.c       # payloady MQTT discovery dla HA
    rly_mqtt.c     # pakiety MQTT 3.1.1
    rly_session.c  # logika sesji: CONNACK -> discovery -> stany -> komendy
  port/        # wszystko, co zależy od konkretnego STM32 (do napisania)
    board.h             # pinout: przekaźniki, LED, RS485 DE, PHY reset
    clock.c             # HSE/PLL, taktowanie 72/168 MHz zależnie od rodziny
    eth.c               # RMII/MII + PHY init (zależny od PHY!)
    netif.c             # glue lwIP
    config_store.c      # ustawienia w ostatniej stronie flash (IP, nazwa, MQTT)
    relay.c             # GPIO + stan po starcie
  app/
    main.c              # init, pętla, keepalive
```

`core/` nie zna sprzętu — dlatego ma testy hostowe i dlatego da się go oddać do
weryfikacji bez płytki.

### 3. Model encji w Home Assistant

- 8 × `switch` — `homeassistant/switch/<node_id>/relay<N>/config`
  - `cmd_t`: `<node_id>/relay<N>/set` (`ON`/`OFF`), `stat_t`: `<node_id>/relay<N>/state` (retained)
- dostępność: `<node_id>/status` = `online`/`offline`, jako **retained will** w CONNECT
  (dzięki temu HA pokazuje `unavailable` po zerwaniu, bez czekania na timeout)
- jedna grupa urządzeń (`dev.ids`) — 8 przełączników jako jedno urządzenie, nie 8 osobnych

### 4. Zgodność wsteczna

- TCP:2000 zostaje — ten sam dialekt JSON, więc `Fescron/DFRobot-RLY8-python3` i aplikacja
  DFRobot działają dalej (kod: `rly_json.c`).
- RS485: ramki `55 AA` z adresem (`rly_frame.c`); brakujące kody `cmd` (nazwa, wersja, DHCP,
  restart) odtworzymy empirycznie — patrz `docs/protocol.md` §4.

### 5. Czego brakuje, by pisać `port/`

Tabela do wypełnienia po identyfikacji płytki:

| Element | Wartość | Skąd |
| --- | --- | --- |
| MCU (rodzina, flash/RAM) | ? | oznaczenie z PCB |
| PHY + jego adres MDIO | ? | układ przy RJ45 (DP83848 vs LAN8720 różni się interfejsem: RMII + REF_CLK) |
| Zegar HSE | ? | kwarc na płytce (25 MHz typowo przy LAN8720) |
| Przekaźniki: GPIO czy expander | ? | drivery przy przekaźnikach (ULN2803 / 74HC595) |
| Stan po resecie | ? | czy cewki są aktywne stanem niskim (ważne: przekaźnik może „klapnąć” przy starcie) |
| RS485 DE/RE | ? | transceiver przy złączu |
| Dostęp do flashowania | ? | BOOT0 / pady SWD |

### 6. Drogi wgrania (do ustalenia na sprzęcie)

| Droga | Wymaga | Uwagi |
| --- | --- | --- |
| ROM DFU | BOOT0 + USB | bez dodatkowego sprzętu; sprawdzić, czy BOOT0 jest wyprowadzony |
| SWD | ST-Link/J-Link/DAPLink | daje też odczyt i debug; `pyOCD` już gotowy w `work/toolchain/py` |
| UART bootloader | adapter 3V3 | ostatnia deska ratunku, wymaga AN3155 |

### 7. Kamienie milowe

- **M0** — identyfikacja płytki (MCU/PHY/piny), wybrana droga wgrania. *blokuje wszystko dalej*
- **M1** — miga LED, UART/console działa, potwierdzony pinout przekaźników (jeden przekaźnik klika)
- **M2** — Ethernet wstaje: link, DHCP, ping; weryfikacja, że PHY jest dobrze skonfigurowany
- **M3** — MQTT: discovery + 8 switchy sterowanych z HA
- **M4** — zgodność wsteczna: TCP:2000 JSON + RS485

### 8. Ryzyka

- **PHY**: najczęstsza przyczyna „Ethernet nie wstaje” to zły tryb (RMII vs MII), brak
  REF_CLK 50 MHz albo zły adres MDIO. Bez oznaczenia PHY nie ma sensu zgadywać.
- **Przekaźniki przy starcie**: jeśli cewki są sterowane stanem niskim, GPIO w stanie
  wysokiej impedancji może je załączyć na moment. W `port/relay.c` stan wyjść ustawiamy
  **przed** konfiguracją jako output.
- **RDP**: jeśli oryginalny firmware ma włączoną ochronę odczytu, odczyt flash nie wyjdzie.
  Wgranie nowego firmware nadal jest możliwe (mass erase), ale tracimy referencję.
- **PoE**: 44–57 V na płytce — pomiary tylko przy odłączonym zasilaniu, albo świadomie i ostrożnie.
