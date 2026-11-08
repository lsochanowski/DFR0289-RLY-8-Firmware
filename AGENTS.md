# AGENTS.md — RLY-8 (DFRobot DFR0289) replacement firmware

Everything here was measured on the actual board over SWD, not copied from a reference
design. If you change a fact, change it here too.

Wartości konkretnego środowiska (UID sondy, adresy w sieci, ścieżki) trzymamy lokalnie w
`AGENTS.local.md` — plik jest w `.gitignore`, repo jest publiczne.

Cel projektu: zastąpić oryginalny firmware własnym tak, żeby 8-przekaźnikowy moduł działał
w Home Assistant po MQTT (docelowo także po Ethernet/TCP:2000 jak oryginał).

---

## 1. Sprzęt — fakty ustalone pomiarami

| Element | Wartość | Jak ustalone |
| --- | --- | --- |
| MCU | **STM32F107RCT6** (LQFP64, Cortex-M3 r1p1; oznaczenie z obudowy) | CPUID `0x411FC231`, DBGMCU_IDCODE `0x10016418` → DEV_ID `0x418` |
| Flash / RAM | 256 KB / 64 KB | `0x1FFFF7E0` = `0x0100`, initial SP `0x20010000` |
| RDP | poziom 0 (brak ochrony) | option bytes `0x1FFFF800` = `0x00FF5AA5` |
| HSE | **25 MHz** (oscylator 4-pad między MCU a PHY) | DWT_CYCCNT przy `SW=HSE` = 25,00 MHz (2026-11-07) |
| SYSCLK | **72 MHz**: 25 /5 (PREDIV2) ×8 (PLL2) /5 (PREDIV1) ×9 (PLL) | DWT_CYCCNT = 72,00 MHz. Stare „HSE 8 MHz ×9” dawało ~139 MHz (PLL w saturacji) |
| **Zegar PHY** | **X1 DP83848 z MCO = PA8**, PHY nie ma własnego kwarcu | bez MCO: MDIO działa, ale zero sygnału na linii; MCO=HSE → link 100M FD |
| Tryb ETH | **MII** (strap PHY, `RBR=0x0001`); MCO = HSE 25 MHz | DHCP + ping 300/300 (2026-11-07) |
| **PLL z HSI/2 nie lockuje** | — | próba HSI/2 ×16 nie dała `PLLRDY`; zawsze używać HSE |
| PHY Ethernet | **TI DP83848AA**, adres MDIO **1** | odczyt ID `0x2000:0x5C90` + `eth_init()` w firmware |
| Unikalny ID | 96 bit, inny w każdym egzemplarzu (z niego liczony jest MAC) | `0x1FFFF7E8..F0` |

### Mapa przekaźników (KOMPLETNA, potwierdzona obserwacją)

| Przekaźnik | Pin | | Przekaźnik | Pin |
| --- | --- | --- | --- | --- |
| 1 | **PC8** | | 5 | **PC12** |
| 2 | **PC9** | | 6 | **PC7** |
| 3 | **PC10** | | 7 | **PC6** |
| 4 | **PC11** | | 8 | **PB15** |

Wszystkie kanały **aktywne stanem wysokim**. Dwie osobliwości: 6 i 7 są **zamienione**
względem naturalnej kolejności PC6/PC7, a relay 8 siedzi samotnie na porcie B (PB15).
Szczegóły i przebiegi: `docs/relay-mapping.md`.

### Interfejs użytkownika i USB (zmierzone 2026-11-07/08)

| Element | Pin | Jak ustalone |
| --- | --- | --- |
| **Niebieska dioda** | **PB7, aktywna stanem niskim** | bisekcja miganiem grup pinów przez SWD + obserwacja |
| **Przycisk DEFAULT** | **PB14, wciśnięty = 0** (pull-up wewnętrzny) | podsłuch IDR przez SWD przy 3 wciśnięciach |
| USB-B | natywne OTG FS, PA11/PA12 | sterownik w zipie = ST VCP (`0483:5740`) |
| **VBUS na PA9** | **brak** — PA9 ściągnięty do masy na płytce | z kablem PA9=0 nawet z pull-upem; `BSVLD=0` |

USB działa tylko z obejściem: firmware ustawia **PA9 jako wyjście w stanie wysokim**
(`USB_NO_VBUS_SENSE=1`, domyślne w `usb_cdc.c`). Skutek: brak wykrywania odłączenia kabla
(host i tak robi reset przy ponownym wpięciu).

### Piny, których NIE WOLNO sterować

- **PA13 / PA14** — SWDIO/SWCLK. Sterowanie nimi zrywa debugger i wtedy każdy odczyt RAM
  jest śmieciem (na tym się już raz przewróciliśmy).
- **PA2 / PC1** — MDIO / MDC (alternate function dla ETH).
- **PC6–PC12, PB15** — przekaźniki; w sondach diagnostycznych wykluczać, żeby nie klikać
  cewkami i nie wywoływać zapadów zasilania.

---

## 2. Wgrywanie i debug (SWD)

Probe: ST-Link (UID własnej sondy: `pyocd list`; lokalnie w `AGENTS.local.md`, poza repo). **Nie ma ROM DFU/BOOT0 — działa SWD.**

```bash
cd <katalog repozytorium>
PYTHONPATH=work/toolchain/py python3 -m pyocd flash -t stm32f103rc \
  -u $STLINK_UID firmware/build/app.elf
```

- `-t stm32f103rc` (nie `stm32f107…`) — pyOCD nie ma wbudowanego targetu F107, a algorytm
  flashowania F1 jest wspólny i rozmiar 256 KB się zgadza.
- **Po każdym `flash` zrób `reset`** — jeśli obraz był identyczny, pyOCD nic nie zapisze
  („identical N bytes") i firmware **nie ruszy od nowa**.
- Odczyt rejestrów/pamięci:
  ```bash
  PYTHONPATH=work/toolchain/py python3 -m pyocd cmd -u <UID> -t cortex_m \
    -c "read32 0xE000ED28" -c "savemem 0x20000830 0x54 work/bs.bin"
  ```

### `boot_status` — raport startowy firmware (czytaj po SWD)

Adres **zmienia się przy każdej zmianie layoutu** — bierz go z obrazu:

```bash
cd firmware && arm-none-eabi-nm build/app.elf | grep boot_status   # np. 0x20000830
```

Kolejność pól (`uint32_t`, little-endian):

| # | pole | znaczenie |
| --- | --- | --- |
| 0 | magic | `0x524C5931` („RLY1") |
| 1 | clock_result | 0 = HSE 72 MHz OK |
| 2 | relays_mapped | powinno być 8 |
| 3 | sequence_done | `0x444F4E45` („DONE") |
| 4 | eth_init_result | 0 = PHY znaleziony i skonfigurowany |
| 5 | phy_address | 1 = DP83848AA |
| 6 | link_up | 1 = link fizyczny |
| 7 | bmsr | rejestr 0x01 PHY |
| 8 | physts | rejestr 0x10 PHY |
| 9 | poll_count | licznik pętli |
| 10 | eth_dma_result | 0 = MAC+DMA wystartowały |
| 11 | netif_up | 1 = lwIP netif podniesiony |
| 12 | dhcp_started | 1 = DHCP uruchomiony (wymaga linku) |
| 13 | ip_addr | adres IPv4 (0 = brak) |
| 14 | rx_frames | licznik odebranych ramek |
| 15 | tx_frames | licznik wysłanych ramek |
| 16 | up_ms | ms od startu (dowód, że SysTick działa) |
| 17 | mqtt_state | 0 idle, 1 connecting, 2 ready, 3 error |
| 18 | mqtt_commands | ile komend z MQTT zastosowano |
| 19 | relay_mask | aktualny stan przekaźników (bit0 = relay 1) |
| 20 | static_ip | 1 = użyto adresu zapasowego `192.168.9.50/24` |
| 21 | bmcr | rejestr 0x00 PHY (bit11 power-down, bit10 isolate) |
| 22 | rbr | rejestr 0x17 PHY (bit5 = tryb RMII) |
| 23 | phyctrl | rejestr 0x19 PHY |
| 24 | cfg_loaded | 1 = konfiguracja z flash, 0 = fabryczna |
| 25 | http_requests | liczba obsłużonych żądań HTTP |
| 26 | usb_configured | 1 = host skonfigurował USB CDC |
| 27 | broker_ip | adres brokera MQTT (po autodetekcji/DNS) |

Liczniki USB: symbol `usb_cdc_stats` (irqs, resets, enum_done, setups, stalls, …).

---

## 3. Firmware — architektura i budowanie

```
firmware/
  core/            # przenośne, testowane na hoście (bez sprzętu)
    rly_frame.c    # ramka 55 AA + CRC = suma 8-bit (protokół RS485/USB oryginału)
    rly_json.c     # dialekt JSON oryginału (TCP:2000)
    rly_ha.c       # payloady MQTT discovery dla Home Assistant
    rly_mqtt.c     # pakiety MQTT 3.1.1 (CONNECT/PUBLISH/SUBSCRIBE + parser)
    rly_session.c  # sesja: CONNACK → discovery → stany → komendy
    rly_mqtt_client.c # klient nieblokujący: backoff, keepalive, reconnect
    rly_config.c   # konfiguracja: struktura, CRC32, walidacja klucz=wartość (WWW + konsola)
    rly_http.c     # parser HTTP, formularze, Basic Auth, escapowanie HTML
    rly_web.c      # panel WWW: strony, /save, /relay, /factory, /api/status, token CSRF
    rly_console.c  # konsola tekstowa (help/status/show/set/save/relay/reboot/factory)
    rly_mdns.c     # mDNS: zapytania PTR/A, parser odpowiedzi, odpowiedź A dla <host>.local
    test_*.c       # testy hostowe
  port/f107/       # warstwa sprzętowa
    board.h        # MCU, zegary, GPIO
    clock.c        # HSE → 72 MHz
    relay.c        # driver przekaźników (mapa pinów w jednym miejscu)
    eth.c          # PHY: piny, MDIO, autoneg, link, RMII (AFIO_MAPR bit23)
    eth_dma.c      # MAC + DMA, ringi RX×4 / TX×2, deskryptory
    ethernetif.c   # glue lwIP (polling, bufory statyczne)
    tcp_stream.c   # rly_stream na gnieździe TCP lwIP (dla klienta MQTT)
    flash_cfg.c    # konfiguracja w 2 ostatnich stronach flash (A/B, 0x0803F000/F800)
    httpd.c        # serwer HTTP na raw API lwIP (2 sloty + kolejka 4 połączeń)
    discovery.c    # mDNS responder + autodetekcja brokera (_mqtt._tcp, homeassistant.local), DNS
    usb_cdc.c      # USB OTG FS device, CDC-ACM (konsola), przerwanie IRQ 67
    ui.c           # dioda PB7 (wzorce stanu) + przycisk PB14 (5 s = factory)
    systick.c      # 1 ms tick + sys_now() dla lwIP
    lwip/arch/     # cc.h, sys_arch.h, sys_arch.c (critical sections na PRIMASK)
    ../probe/      # programy diagnostyczne (patrz §6)
  app/
    startup.c      # tabela wektorów (desygnowane indeksy!) + reset handler
    main.c         # boot: config → clock → relays → systick → LED/USB → PHY → MAC/DMA → lwIP
                   #       → WWW, mDNS, MQTT; obsługa akcji (zapis, restart, factory)
  third_party/
    lwip/          # lwIP 2.2.0 (tylko potrzebne katalogi) + lwipopts.h projektu
    cmsis/         # stm32f107xc.h (definicje rejestrów, źródło nazw bitów)
```

### Komendy

```bash
cd firmware

make test          # 8 zestawów testów hostowych (~325 sprawdzeń) — musi być zielone
make port-layer    # kompilacja warstwy port/f107 (bez linkowania)
make app           # pełny firmware: app + port + core + lwIP  → build/app.elf
make probe-*       # programy diagnostyczne (patrz §6)
```

Aktualny obraz: **text 63 833 B, data 20 B, bss 54 336 B** (RAM ~54 KB z 64 KB, ~10 KB na
stos). Link z `--gc-sections`, mapa w `build/app.map`. Flash: `LENGTH = 252K` — ostatnie
4 KB to strony konfiguracji (pyOCD flash ich nie rusza, bo kasuje tylko sektory obrazu).
Toolchain: `work/toolchain/arm-gnu-toolchain-13.3.rel1-.../bin/arm-none-eabi-gcc` (13.3.1),
pyOCD 0.46.1 w `work/toolchain/py`. `brew` jest bezużyteczny — sandbox blokuje zapis poza
workspace.

### Konfiguracja urządzenia (runtime, nie w buildzie)

Trzymana we flash (A/B + CRC32, nowsza ważna kopia wygrywa). Zmieniana przez:
- **panel WWW** `http://<ip>/` albo `http://rly8-xxxxxx.local/` — login **admin**, hasło
  domyślnie **admin**; Status (przekaźniki), Ustawienia (sieć, MQTT, hasło), `/api/status` (JSON),
- **konsolę USB** (`/dev/cu.usbmodem*`, dowolna prędkość): `help`, `status`, `show`,
  `set <klucz> <wartość>`, `save`, `reboot`, `factory`, `relay <1-8|all> <on|off|toggle>`.

Klucze: `dhcp ip mask gw dns hostname name mqtt mqtt_host mqtt_port mqtt_user mqtt_pass
mqtt_prefix admin_pass`. Wartości fabryczne: DHCP, zapasowy/statyczny `192.168.9.50/24`,
MQTT włączone, **`mqtt_host` pusty = autodetekcja**, port 1883, prefiks `homeassistant`,
hasło `admin`. **Factory reset**: przycisk DEFAULT 5 s, `factory` w konsoli albo przycisk w panelu.

Autodetekcja brokera (pusty `mqtt_host`): mDNS PTR `_mqtt._tcp.local` (SRV → host+port),
potem `homeassistant.local` (A) z portem z konfiguracji; ponowienie co 10 s. Nazwa `*.local`
→ mDNS A; inna nazwa → DNS (serwer z DHCP lub `dns`). Zmierzone w sieci domowej:
`homeassistant.local` znaleziony, broker wymaga loginu (CONNACK rc=5 bez danych).

Dioda: brak linku = krótki błysk co 2 s; link bez IP = 2 Hz; IP, MQTT nie gotowe = podwójny
błysk co 1 s; wszystko OK = świeci, gaśnie na chwilę przy aktywności; przycisk trzymany =
szybkie miganie, po 5 s ciągłe światło i restart.

Tematy (HA MQTT discovery): `<prefix>/switch/<node>/relay<N>/config`,
`<node>/relay<N>/set` (`ON`/`OFF`), `<node>/relay<N>/state` (retained),
`<node>/status` (`online`/`offline`, retained will). `node` = `hostname` albo
`rly8-<3 ostatnie bajty MAC>`. Discovery zawiera `cu` (link do panelu na stronie urządzenia w HA).

---

## 4. Protokół oryginału (do odtworzenia — `docs/protocol.md`)

- **JSON po TCP:2000**: `{"get":"relayStatus"}`, `{"relayN":"on"}`, `{"get":"netconfig"}`,
  `{"dhcp":"on"}`, `{"reboot":"1"}`; odpowiedzi `{"resp":"ok"|"error"|"unsupported"}`.
- **Ramki binarne (USB CDC / RS485)**: `55 AA | addr | len | cmd | data[len] | crc`,
  **crc = suma 8-bit** wszystkich poprzedzających bajtów (zweryfikowane na 5 przykładach
  z datasheetu). Kody `cmd` nieudokumentowane w hex: nazwa, wersja, DHCP, restart.
- Zaimplementowane i przetestowane: `core/rly_frame.c`, `core/rly_json.c`.

---

## 5. Stan: co działa, co nie

**Działa (zmierzone 2026-11-07):** zegar 72 MHz (DWT), 8/8 przekaźników, PHY pod MDIO=1,
**link 100 Mb/s full-duplex** (`physts=0x0715`), MAC+DMA w MII, lwIP, **DHCP** (adres
w sieci switcha), **ping 300/300 bez strat** (także 1400 B).

Co blokowało Ethernet (naprawione):
1. **Brak zegara PHY** — X1 DP83848 jest podłączony do PA8/MCO; firmware nie włączał MCO.
   PHY odpowiadał po MDIO, ale nie nadawał (`physts` bez „signal detect”, Mac `inactive`).
   Fix: `clock_phy_mco()` w `port/f107/clock.c`, wołane z `eth_init()`.
2. **Zły zegar MCU** — HSE to 25 MHz, nie 8 MHz; PLL ×9 → ~139 MHz. Fix: łańcuch PLL2.
3. **Tryb RMII wymuszany na PHY** strapowanym na MII — teraz domyślnie `ETH_MODE_MII=1`
   (wejścia MII CRS/COL/RX_ER/TX_CLK jako input). RMII alternatywnie: `-DETH_MODE_MII=0`
   (MCO = PLL3 50 MHz; link wstaje też w RMII, ścieżka danych RMII nie testowana).
4. **lwIP nie wysyłał** — brak `netif_set_link_up()`; teraz `main.c` śledzi link PHY.
5. **Deskryptory DMA** — dodane `TCH`/`RCH` (tryb łańcuchowy), RX czytany po kolei.

**Panel WWW, konsola USB, konfiguracja we flash, mDNS, przycisk, dioda — działają
(zmierzone 2026-11-08):** 6 równoległych żądań HTTP OK (max 0,48 s), zapis ustawień → restart
→ `cfg_loaded=1`, factory przyciskiem → strony flash `0xFF`, konsola USB enumeruje się jako
`RLY-8 open firmware` (`0483:5740`), autodetekcja znalazła broker HA.

**Otwarte:** MQTT `mqtt_state=3` — broker HA wymaga loginu; wpisać `mqtt_user`/`mqtt_pass`
w panelu i sprawdzić 8 przełączników w HA.

---

## 6. Programy diagnostyczne (`firmware/port/probe/`)

| cel | komenda |
| --- | --- |
| skan MDIO (32 adresy, diagnostyka) | `make probe` → `build/probe.elf` |
| soak MDIO (1000 rund) | `make probe-soak` |
| czy PHY wstaje późno | `make probe-wait` |
| cały bank pinów naraz + maska | `make probe-bank EXTRA="-DBANK=1 -DMASK=0x7FFF"` |
| **wszystkie piny poza przekaźnikami i SWD** | `make probe-all EXTRA=-DDRIVE_LOW=1` |
| wszystkie piny + jednoczesny skan MDIO | `make probe-all-mdio` |
| identyfikacja przekaźnika po liczbie kliknięć | `make probe-count EXTRA="-DPORT_INDEX=2 -DSTART_PIN=8 -DEND_PIN=15"` |
| jeden pin naraz, bez liczenia (przebieg „hold") | `make probe-hold EXTRA="-DPORT_INDEX=2 -DSTART_PIN=0 -DEND_PIN=7"` |
| sweep opisujący każdy pin inną liczbą kliknięć | `make probe-multi` |
| świecenie całego banku (stary wariant) | `make probe-relay` |
| skan pinów GPIO z MDIO (starsza sonda) | `make probe-gpio` |

Wszystkie sondy zapisują wynik w RAM pod `0x20008000` (magic `0x524C5931`) i licznik startów
pod `0x2000F000` (wzorzec `0xB007`) — ten drugi odróżnia „pętla" od „płytka się resetuje".

---

## 7. Dalsze kroki (w tej kolejności)

1. ~~Diagnostyka PHY / link~~ — **zrobione** (patrz §5). Bez DHCP firmware po 15 s
   ustawia `192.168.9.50/24` (`static_ip=1`).
3. **MQTT w HA** — wpisać login MQTT w panelu, sprawdzić w HA 8 przełączników
   (discovery), przełączenie z HA → przekaźnik (pole `relay_mask` w `boot_status`).
4. **Serwer JSON na TCP:2000** — zgodność z oryginalnym DFRobot (użyć `core/rly_json.c`).
5. **RS485 / ramki `55 AA`** — zgodność z magistralą (`core/rly_frame.c`).
6. Porządki: watchdog (IWDG), OTA, opcjonalnie zapamiętywanie stanu przekaźników po restarcie
   (dziś: po starcie wszystkie OFF, bez sekwencji klikania), uwierzytelnianie konsoli USB
   (dziś: bez hasła — zakładamy fizyczny dostęp).

---

## 8. Lekcje (żeby nie powtarzać cudzych błędów)

- **Nie ufaj „HSERDY = kwarc X MHz”** — częstotliwość zmierz: włącz DWT_CYCCNT
  (`DEMCR` bit 24, `DWT_CTRL` bit 0), przełącz `SW=HSE` i licz cykle w czasie hosta.
- **PHY, który odpowiada po MDIO, wcale nie musi mieć zegara** — interfejs MDIO jest taktowany
  MDC. Brak „signal detect” (PHYSTS bit 10) + cisza po drugiej stronie kabla = sprawdź X1/MCO.
- lwIP 2.x: bez `netif_set_link_up()` żadna ramka nie wyjdzie (`tx_frames=0`).
- **RAM 64 KB jest ciasny**: `--gc-sections` wyrzuca locale/malloc newlib (2 KB data);
  ramki kopiujemy DMA↔pbuf bezpośrednio (`eth_dma_rx_peek/tx_acquire`), bez buforów pośrednich.
- **Pętla główna bez opóźnień** → stan PHY czytać co 20 ms (MDIO jest wolne, a licznik
  ponawiania autonegocjacji liczy wywołania).
- **mDNS bez IGMP**: lwIP przyjmuje multicast przy `LWIP_IGMP=0`, ale MAC musi mieć `PAM`
  (`MACFFR` bit 4), inaczej ramki do 224.0.0.251 giną w filtrze.
- **USB OTG FS na F107 nie ma `NOVBUSSENS`** — bez VBUS na PA9 jedyna droga to wymusić PA9.

- **`MACMIIAR` to `MAC_BASE + 0x10`**, `MACMIIDR` to `+0x14`. Zły adres wygląda jak
  „MB wisi na zawsze", a jest odczytem własnego zapisu.
- **Tabela wektorów tylko z desygnowanymi indeksami** (`[15] = SysTick_Handler`). Lista
  pozycyjna przesunęła raz SysTick na slot PendSV → skok pod adres 0 → `INVSTATE` +
  HardFault (`CFSR=0x00020000`, `HFSR=0x40000000`).
- **lwIP `NO_SYS=1` + `SYS_LIGHTWEIGHT_PROT=1`**: `lwip/sys.h` nie wciąga `arch/sys_arch.h`
  w gałęzi NO_SYS, więc `sys_prot_t` trzeba zdefiniować z `cc.h`.
- **newlib**: linkować `-nostartfiles -specs=nosys.specs` (mamy własny startup), a w linker
  script dodać `end`, `_end`, `__bss_start__`, `__bss_end__` (newlib `sbrk`).
- **STM32F1: tryb ETH wybiera `AFIO_MAPR` bit 23** (`MII_RMII_SEL`), nie sam MAC.
- **Bufory na stosie**: ramka 1524 B + pakiety MQTT 512 B rozsadzają mały stos — trzymać je
  jako `static` (u nas `eth_dma.c`, `ethernetif.c`, `rly_session.c`).
- **`make` i katalog `port/`**: target o nazwie katalogu jest zawsze „up to date" — dlatego
  `make port-layer`, nie `make port`.
- **Rzutowanie `uint32_t` → wskaźnik** przez `uintptr_t`, inaczej plik nie kompiluje się na
  hoście (testy) przy `-Werror`.

---

## 9. Dokumentacja w repo

| plik | zawartość |
| --- | --- |
| `docs/STATUS.md` | jednostronicowy stan projektu (sprzęt, wgrywanie, mapa, Ethernet, firmware) |
| `docs/hardware-findings.md` | wszystkie pomiary sprzętowe + pinout PHY do multimetru |
| `docs/relay-mapping.md` | mapa przekaźników, metodologia (bisekcja maską) i przebiegi |
| `docs/protocol.md` | protokół oryginału: ramki `55 AA` + JSON |
| `docs/firmware-plan.md` | plan wdrożenia, kamienie milowe, ryzyka |
| `tools/dump-original-fw.md` | procedura zrzutu oryginalnego firmware (tylko odczyt) |
