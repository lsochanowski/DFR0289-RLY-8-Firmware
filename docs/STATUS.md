# RLY-8 (DFRobot DFR0289) — project status

*Polski: [niżej](#polski)*

*Historical development notes; the current state and user manual are in [README.md](../README.md).*

A one-page summary of what has been **measured and confirmed**, plus what is still open.
Details: `docs/hardware-findings.md`, `docs/relay-mapping.md`,
`docs/protocol.md`, `docs/firmware-plan.md`.

Goal: replace the original firmware with our own, with Home Assistant support over MQTT (8 relays).

## 1. Hardware — determined by measurement over SWD

| Item | Value | How |
| --- | --- | --- |
| MCU | **STM32F107VC** (Cortex-M3 r1p1) | CPUID `0x411FC231`, DBGMCU_IDCODE `0x10016418` → DEV_ID `0x418` |
| Flash / RAM | 256 KB / 64 KB | `0x1FFFF7E0` = `0x0100`, initial SP `0x20010000` |
| RDP | level 0 (no readout protection) | option bytes `0x1FFFF800` = `0x00FF5AA5` |
| HSE | **25 MHz** (measured with DWT_CYCCNT) | shared with the PHY via MCO/PA8 |
| SYSCLK | **72 MHz** via PLL2 (25/5×8/5×9) | DWT_CYCCNT = 72.00 MHz |
| Ethernet PHY | **TI DP83848AA** | single ID read `0x2000:0x5C90` + chip marking |
| Unique ID | 96-bit, different on every unit (the MAC is derived from it) | `0x1FFFF7E8..F0` |

Note: **the PLL from HSI/2 did not lock** on this unit — the firmware must start from HSE.

## 2. Flashing

**SWD via ST-Link + pyOCD** (tested on the live chip). Probe UID: `pyocd list`.

```bash
PYTHONPATH=work/toolchain/py python3 -m pyocd flash -t stm32f103rc \
  -u $STLINK_UID firmware/build/<image>.elf
```

`stm32f103rc` instead of `stm32f107…`, because pyOCD has no built-in F107 target, and the F1
flashing algorithm is shared (256 KB matches). ROM DFU/BOOT0 is not needed.

## 3. Flash dump

`work/orig-fw/current-flash.bin` (262 144 B, sha256 `f5bfb3e2…1bc37`). Contents: the DFRobot
bootloader at `0x08000000`–`0x080014FF` and the **user's test firmware** from `0x08001500`
(strings `STM32F107 Relay Controller`, `Ethernet dziala!`). From `0x08002000` upwards it is all
`0xFF` — the original DFRobot firmware had been erased, so the pinout cannot be recovered from it.

## 4. Original protocol (to be reproduced in the new firmware)

- **JSON over TCP:2000**: `{"get":"relayStatus"}`, `{"relayN":"on"}`, `{"get":"netconfig"}`,
  `{"dhcp":"on"}`, `{"reboot":"1"}`, responses `{"resp":"ok"|"error"|"unsupported"}`.
- **Binary frames (USB CDC / RS485)**: `55 AA | addr | len | cmd | data[len] | crc`,
  where **crc = 8-bit sum of all preceding bytes** (verified against five examples from the
  datasheet).
- Both formats implemented and tested: `firmware/core/rly_frame.c`, `rly_json.c`.

## 5. Relays — map (COMPLETE)

| Relay | Pin | Status |
| --- | --- | --- |
| 1 | **PC8** | ✅ confirmed |
| 2 | **PC9** | ✅ confirmed |
| 3 | **PC10** | ✅ confirmed |
| 4 | **PC11** | ✅ confirmed |
| 5 | **PC12** | ✅ confirmed |
| 6 | **PC7** | ✅ confirmed |
| 7 | **PC6** | ✅ confirmed |
| 8 | **PB15** | ✅ confirmed |

All channels are **active high**. Two quirks of the board: relays 6/7 are swapped relative to
the natural PC6/PC7 order, and relay 8 is not in the port C block but sits alone on PB15.
Details and runs: `docs/relay-mapping.md`. The map is written into
`firmware/port/f107/relay.c`.

## 6. Ethernet — WORKS (2026-11-07)

**Link 100 Mb/s FD, DHCP, ping 300/300 with no loss.** Causes of the earlier failure:
the PHY takes its X1 clock from **MCO/PA8** (which was not enabled), **HSE = 25 MHz** (not
8 MHz — the MCU was running at ~139 MHz), the PHY is strapped to **MII** (the firmware was
forcing RMII), lwIP without `netif_set_link_up()`, DMA descriptors without TCH/RCH.
Details: `AGENTS.md` §1, §5.

History (before the fix):

**The DP83848AA PHY responds at MDIO address = 1.** Confirmed by reading `boot_status` of the
target firmware (`0x20000578`) after 20 s of operation:

```
eth_init = 0        eth_dma = 0        netif_up = 1
phy_address = 1     up_ms = 19842     link_up = 0 (no cable)
```

The earlier probes (1000-round soak, 12 800 reads) got zero responses because they **did not
run the full `eth_init()` sequence** (clocks → MAC reset → pin configuration →
autonegotiation). Conclusion: for MDIO it is not enough to "enable the clock and write to MACMIIAR".

### What is done

- lwIP 2.2.0 **vendored** in `firmware/third_party/lwip` (configuration: `lwipopts.h`,
  12 KB heap, 4 TCP PCBs, DHCP enabled, `NO_SYS=1`)
- architecture port: `port/f107/lwip/arch/cc.h`, `sys_arch.h`, `sys_arch.c`
  (critical sections on PRIMASK), `sys_now()` from the 1 ms SysTick
- MAC+DMA driver: `port/f107/eth_dma.c` (RX×4/TX×2 rings, descriptors, AAB, store&forward)
- lwIP glue: `port/f107/ethernetif.c` (polling from the main loop, static buffers)
- image: `text 36 017 B`, `data 1 400 B`, `bss 39 932 B` (~41 KB of 64 KB RAM)

### To be closed

1. **Ethernet cable** → `link_up=1` → DHCP → ping.
2. Connect the MQTT client (`core/rly_mqtt_client.c`, ready and tested) to the lwIP
   socket — the `rly_stream` implementation over TCP is missing.
3. JSON server on port 2000 (compatibility with the original).

Plan B, if the PHY turns out to be damaged: the firmware exposes the protocol over **USB CDC**,
and a USB→MQTT bridge runs on the host (HA / Raspberry Pi).

## 7. Firmware — what is in the repo

```
firmware/
  core/                 # portable, tested on the host (169 checks, 0 failures)
    rly_frame.c         # 55 AA frame + sum8 CRC
    rly_json.c          # the original's JSON dialect
    rly_ha.c            # MQTT discovery payloads for HA
    rly_mqtt.c          # MQTT 3.1.1 packets (CONNECT/PUBLISH/SUBSCRIBE + parser)
    rly_session.c       # session: CONNACK → discovery → states → commands, injected transport
  port/f107/            # hardware layer: clock (HSE→72 MHz), relay, eth
  port/probe/           # identification probes + linker script + startup
  app/                  # main.c + startup.c (M1: clock, relays, ETH bring-up)
  Makefile              # make test / port-layer / app / probe-* 
```

Tests: `make -C firmware test` → 6 suites, **189 checks, 0 failures**
(`test_frame` 31, `test_json` 39, `test_mqtt` 41, `test_session` 38, `test_relay` 15,
`test_mqtt_client` 25).

## 8. Next steps

1. Finish the map: bisection of banks C and D for relays 6/7 (`docs/relay-mapping.md` §"Co jeszcze").
2. Write the pins found into `firmware/port/f107/relay.c`.
3. Resolve the PHY (measurements) or switch to plan B (USB CDC + MQTT bridge).
4. Connect the MQTT layer (`rly_session`) to the lwIP transport and expose discovery in HA.

---

## Polski

### RLY-8 (DFRobot DFR0289) — stan projektu

*Historyczne notatki z rozwoju; aktualny stan i instrukcja są w [README.md](../README.md).*

Jednostronicowe podsumowanie tego, co zostało **zmierzone i potwierdzone**, plus co jest
jeszcze otwarte. Szczegóły: `docs/hardware-findings.md`, `docs/relay-mapping.md`,
`docs/protocol.md`, `docs/firmware-plan.md`.

Cel: zastąpić oryginalny firmware własnym, z obsługą Home Assistant po MQTT (8 przekaźników).

### 1. Sprzęt — ustalony pomiarami przez SWD

| Element | Wartość | Jak |
| --- | --- | --- |
| MCU | **STM32F107VC** (Cortex-M3 r1p1) | CPUID `0x411FC231`, DBGMCU_IDCODE `0x10016418` → DEV_ID `0x418` |
| Flash / RAM | 256 KB / 64 KB | `0x1FFFF7E0` = `0x0100`, initial SP `0x20010000` |
| RDP | poziom 0 (brak ochrony odczytu) | option bytes `0x1FFFF800` = `0x00FF5AA5` |
| HSE | **25 MHz** (zmierzone DWT_CYCCNT) | wspólny z PHY przez MCO/PA8 |
| SYSCLK | **72 MHz** przez PLL2 (25/5×8/5×9) | DWT_CYCCNT = 72,00 MHz |
| PHY Ethernet | **TI DP83848AA** | jednokrotny odczyt ID `0x2000:0x5C90` + oznaczenie układu |
| Unikalny ID | 96 bit, inny w każdym egzemplarzu (z niego liczony jest MAC) | `0x1FFFF7E8..F0` |

Uwaga: **PLL z HSI/2 nie zablokował się** na tym egzemplarzu — firmware musi startować z HSE.

### 2. Wgrywanie

**SWD przez ST-Link + pyOCD** (sprawdzone na żywym układzie). UID sondy: `pyocd list`.

```bash
PYTHONPATH=work/toolchain/py python3 -m pyocd flash -t stm32f103rc \
  -u $STLINK_UID firmware/build/<obraz>.elf
```

`stm32f103rc` zamiast `stm32f107…`, bo pyOCD nie ma wbudowanego targetu F107, a algorytm
flashowania F1 jest wspólny (256 KB się zgadza). ROM DFU/BOOT0 nie jest potrzebny.

### 3. Zrzut flash

`work/orig-fw/current-flash.bin` (262 144 B, sha256 `f5bfb3e2…1bc37`). Zawartość: bootloader
DFRobot w `0x08000000`–`0x080014FF` i **firmware testowy użytkownika** od `0x08001500`
(string `STM32F107 Relay Controller`, `Ethernet dziala!`). Od `0x08002000` w górę czysto
`0xFF` — oryginalny firmware DFRobot został wymazany, więc pinoutu nie da się z niego odzyskać.

### 4. Protokół oryginału (do odtworzenia w nowym firmware)

- **JSON po TCP:2000**: `{"get":"relayStatus"}`, `{"relayN":"on"}`, `{"get":"netconfig"}`,
  `{"dhcp":"on"}`, `{"reboot":"1"}`, odpowiedzi `{"resp":"ok"|"error"|"unsupported"}`.
- **Ramki binarne (USB CDC / RS485)**: `55 AA | addr | len | cmd | data[len] | crc`,
  gdzie **crc = suma 8-bitowa wszystkich poprzedzających bajtów** (zweryfikowane na pięciu
  przykładach z datasheetu).
- Oba formaty zaimplementowane i przetestowane: `firmware/core/rly_frame.c`, `rly_json.c`.

### 5. Przekaźniki — mapa (KOMPLETNA)

| Przekaźnik | Pin | Status |
| --- | --- | --- |
| 1 | **PC8** | ✅ potwierdzone |
| 2 | **PC9** | ✅ potwierdzone |
| 3 | **PC10** | ✅ potwierdzone |
| 4 | **PC11** | ✅ potwierdzone |
| 5 | **PC12** | ✅ potwierdzone |
| 6 | **PC7** | ✅ potwierdzone |
| 7 | **PC6** | ✅ potwierdzone |
| 8 | **PB15** | ✅ potwierdzone |

Wszystkie kanały są **aktywne stanem wysokim**. Dwie osobliwości płytki: relay 6/7 są
zamienione względem naturalnej kolejności PC6/PC7, a relay 8 nie leży w bloku portu C, tylko
samotnie na PB15. Szczegóły i przebiegi: `docs/relay-mapping.md`. Mapa jest wpisana do
`firmware/port/f107/relay.c`.

### 6. Ethernet — DZIAŁA (2026-11-07)

**Link 100 Mb/s FD, DHCP, ping 300/300 bez strat.** Przyczyny wcześniejszej awarii:
PHY bierze zegar X1 z **MCO/PA8** (nie był włączony), **HSE = 25 MHz** (nie 8 MHz — MCU
chodził na ~139 MHz), PHY strapowany na **MII** (firmware wymuszał RMII), lwIP bez
`netif_set_link_up()`, deskryptory DMA bez TCH/RCH. Szczegóły: `AGENTS.md` §1, §5.

Historia (przed naprawą):

**PHY DP83848AA odpowiada pod adresem MDIO = 1.** Potwierdzone odczytem `boot_status`
docelowego firmware (`0x20000578`) po 20 s pracy:

```
eth_init = 0        eth_dma = 0        netif_up = 1
phy_address = 1     up_ms = 19842     link_up = 0 (brak kabla)
```

Wcześniejsze sondy (soak 1000 rund, 12 800 odczytów) dawały zero odpowiedzi, bo **nie
wykonywały pełnej sekwencji `eth_init()`** (zegary → reset MAC → konfiguracja pinów →
autonegotacja). Wniosek: do MDIO nie wystarczy „włączyć zegar i pisać do MACMIIAR".

#### Co jest zrobione

- lwIP 2.2.0 **vendored** w `firmware/third_party/lwip` (konfiguracja: `lwipopts.h`,
  12 KB heap, 4 TCP PCB, DHCP włączone, `NO_SYS=1`)
- port architektury: `port/f107/lwip/arch/cc.h`, `sys_arch.h`, `sys_arch.c`
  (critical sections na PRIMASK), `sys_now()` z SysTick 1 ms
- sterownik MAC+DMA: `port/f107/eth_dma.c` (ringi RX×4/TX×2, deskryptory, AAB, store&forward)
- glue lwIP: `port/f107/ethernetif.c` (polling z głównej pętli, bufory statyczne)
- obraz: `text 36 017 B`, `data 1 400 B`, `bss 39 932 B` (~41 KB z 64 KB RAM)

#### Do domknięcia

1. **Kabel Ethernet** → `link_up=1` → DHCP → ping.
2. Podłączyć klienta MQTT (`core/rly_mqtt_client.c`, gotowy i przetestowany) do gniazda
   lwIP — brakuje implementacji `rly_stream` na TCP.
3. Serwer JSON na porcie 2000 (zgodność z oryginałem).

Plan B, jeśli PHY okaże się uszkodzony: firmware wystawia protokół po **USB CDC**, a mostek
USB→MQTT działa na hoście (HA / Raspberry Pi).

### 7. Firmware — co jest w repo

```
firmware/
  core/                 # przenośne, testowane na hoście (169 sprawdzeń, 0 porażek)
    rly_frame.c         # ramka 55 AA + CRC suma8
    rly_json.c          # dialekt JSON oryginału
    rly_ha.c            # payloady MQTT discovery dla HA
    rly_mqtt.c          # pakiety MQTT 3.1.1 (CONNECT/PUBLISH/SUBSCRIBE + parser)
    rly_session.c       # sesja: CONNACK → discovery → stany → komendy, transport wstrzykiwany
  port/f107/            # warstwa sprzętowa: clock (HSE→72 MHz), relay, eth
  port/probe/           # sondy identyfikacyjne + linker script + startup
  app/                  # main.c + startup.c (M1: zegar, przekaźniki, bring-up ETH)
  Makefile              # make test / port-layer / app / probe-* 
```

Testy: `make -C firmware test` → 6 zestawów, **189 sprawdzeń, 0 porażek**
(`test_frame` 31, `test_json` 39, `test_mqtt` 41, `test_session` 38, `test_relay` 15,
`test_mqtt_client` 25).

### 8. Następne kroki

1. Dokończyć mapę: bisekcja banku C i D dla relay 6/7 (`docs/relay-mapping.md` §„Co jeszcze").
2. Wpisać znalezione piny do `firmware/port/f107/relay.c`.
3. Rozwiązać PHY (pomiary) albo przejść na plan B (USB CDC + mostek MQTT).
4. Podłączyć warstwę MQTT (`rly_session`) do transportu lwIP i wystawić discovery w HA.
