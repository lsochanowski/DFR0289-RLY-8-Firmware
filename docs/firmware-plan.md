# Plan firmware zastępczego dla RLY-8 (DFR0289)

Cel: 8 przekaźników widocznych w Home Assistant przez MQTT discovery, przy zachowaniu
zgodności z oryginalnym API (JSON/TCP:2000 i ramki `55 AA` na RS485).

## 1. Wybór stosu

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

## 2. Układ katalogów

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

## 3. Model encji w Home Assistant

- 8 × `switch` — `homeassistant/switch/<node_id>/relay<N>/config`
  - `cmd_t`: `<node_id>/relay<N>/set` (`ON`/`OFF`), `stat_t`: `<node_id>/relay<N>/state` (retained)
- dostępność: `<node_id>/status` = `online`/`offline`, jako **retained will** w CONNECT
  (dzięki temu HA pokazuje `unavailable` po zerwaniu, bez czekania na timeout)
- jedna grupa urządzeń (`dev.ids`) — 8 przełączników jako jedno urządzenie, nie 8 osobnych

## 4. Zgodność wsteczna

- TCP:2000 zostaje — ten sam dialekt JSON, więc `Fescron/DFRobot-RLY8-python3` i aplikacja
  DFRobot działają dalej (kod: `rly_json.c`).
- RS485: ramki `55 AA` z adresem (`rly_frame.c`); brakujące kody `cmd` (nazwa, wersja, DHCP,
  restart) odtworzymy empirycznie — patrz `docs/protocol.md` §4.

## 5. Czego brakuje, by pisać `port/`

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

## 6. Drogi wgrania (do ustalenia na sprzęcie)

| Droga | Wymaga | Uwagi |
| --- | --- | --- |
| ROM DFU | BOOT0 + USB | bez dodatkowego sprzętu; sprawdzić, czy BOOT0 jest wyprowadzony |
| SWD | ST-Link/J-Link/DAPLink | daje też odczyt i debug; `pyOCD` już gotowy w `work/toolchain/py` |
| UART bootloader | adapter 3V3 | ostatnia deska ratunku, wymaga AN3155 |

## 7. Kamienie milowe

- **M0** — identyfikacja płytki (MCU/PHY/piny), wybrana droga wgrania. *blokuje wszystko dalej*
- **M1** — miga LED, UART/console działa, potwierdzony pinout przekaźników (jeden przekaźnik klika)
- **M2** — Ethernet wstaje: link, DHCP, ping; weryfikacja, że PHY jest dobrze skonfigurowany
- **M3** — MQTT: discovery + 8 switchy sterowanych z HA
- **M4** — zgodność wsteczna: TCP:2000 JSON + RS485

## 8. Ryzyka

- **PHY**: najczęstsza przyczyna „Ethernet nie wstaje” to zły tryb (RMII vs MII), brak
  REF_CLK 50 MHz albo zły adres MDIO. Bez oznaczenia PHY nie ma sensu zgadywać.
- **Przekaźniki przy starcie**: jeśli cewki są sterowane stanem niskim, GPIO w stanie
  wysokiej impedancji może je załączyć na moment. W `port/relay.c` stan wyjść ustawiamy
  **przed** konfiguracją jako output.
- **RDP**: jeśli oryginalny firmware ma włączoną ochronę odczytu, odczyt flash nie wyjdzie.
  Wgranie nowego firmware nadal jest możliwe (mass erase), ale tracimy referencję.
- **PoE**: 44–57 V na płytce — pomiary tylko przy odłączonym zasilaniu, albo świadomie i ostrożnie.
